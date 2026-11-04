#include <gtest/gtest.h>
#include <llco/core/task.hpp>
#include <llco/io/op.decl.hpp>
#include <llco/io/ring.decl.hpp>
#include <llco/net/socket.decl.hpp>
#include <llco/runtime/run.decl.hpp>
#include <llco/runtime/scheduler.decl.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto        BUDGET        = 1s;
constexpr auto        POLL_INTERVAL = 1ms;
constexpr auto        PAYLOAD       = std::string_view{"hello scheduler"};
constexpr unsigned    ENGINES       = 4;
constexpr std::size_t CHUNK         = 64;

// Polls until the predicate holds, or the budget runs out.
template <class Predicate>
auto wait_until(Predicate done) -> bool {
    const auto end = std::chrono::steady_clock::now() + BUDGET;
    while (!done() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(POLL_INTERVAL);
    return done();
}

// Records the engine it first ran on, then echoes one message back.
task<> echo_counted(int fd, std::vector<std::atomic<unsigned>> *landed) {
    for (unsigned i = 0; i < scheduler::size(); ++i)
        if (&scheduler::engine_at(i) == current_engine())
            (*landed)[i].fetch_add(1);
    net::connection         conn{fd};
    std::array<char, CHUNK> buffer{};
    const auto              got = co_await conn.recv(buffer);
    if (!got)
        co_return;
    (void)co_await conn.send(std::string_view{buffer.data(), *got});
}

// Accepts until stopped or cancelled; each connection is placed by the scheduler.
task<> accept_loop(net::listener *listener, std::atomic<bool> *stop,
                   std::vector<std::atomic<unsigned>> *landed) {
    for (;;) {
        if (stop->load())
            co_return;
        const auto accepted = co_await listener->accept_fd();
        if (!accepted) {
            if (accepted.error() == std::errc::operation_canceled)
                co_return;
            continue;
        }
        if (!scheduler::submit(echo_counted(*accepted, landed)))
            ::close(*accepted);
    }
}

// Cancels the pending accept; sync_run_at(0, ...) puts it on the listener's engine.
task<> stop_accept(net::listener *listener) {
    (void)co_await listener->cancel();
}

// One task, sequential connects: only the acceptor calls submit, so the serve
// tasks take consecutive placement slots.
task<> drive_clients(std::uint16_t port, unsigned count, std::atomic<unsigned> *echoed) {
    for (unsigned i = 0; i < count; ++i) {
        net::connection conn{net::tcp_socket()};
        const auto      connected =
            co_await io::connect(current_engine()->ring(), conn.fd(), net::loopback(port));
        EXPECT_TRUE(connected) << connected.error().message();
        if (!connected)
            co_return;
        (void)co_await conn.send(PAYLOAD);
        std::array<char, PAYLOAD.size()> reply{};
        (void)co_await conn.recv(reply);
        if (std::string_view{reply.data(), reply.size()} == PAYLOAD)
            echoed->fetch_add(1);
    }
}

} // namespace

TEST(tcp, scheduler_places_connections_round_robin) {
    scheduler::init(ENGINES);
    net::listener                      listener{0};
    std::vector<std::atomic<unsigned>> landed(ENGINES);
    std::atomic<bool>                  stop{false};
    std::atomic<unsigned>              echoed{0};

    run_at(0, accept_loop(&listener, &stop, &landed));
    ASSERT_TRUE(scheduler::submit(drive_clients(listener.port(), ENGINES, &echoed)));

    ASSERT_TRUE(wait_until([&] { return echoed.load() == ENGINES; }))
        << "the echoes never came back";

    stop.store(true);
    sync_run_at(0, stop_accept(&listener));
    scheduler::stop();

    for (unsigned i = 0; i < ENGINES; ++i)
        EXPECT_EQ(landed[i].load(), 1U) << "engine " << i << " got the wrong share";
}

TEST(tcp, stop_cancels_the_pending_accept) {
    scheduler::init(2);
    net::listener                      listener{0};
    std::vector<std::atomic<unsigned>> landed(2);
    std::atomic<bool>                  stop{false};

    run_at(0, accept_loop(&listener, &stop, &landed));
    ASSERT_TRUE(wait_until([&] { return scheduler::engine_at(0).ring().running_io() == 1; }))
        << "the accept was never submitted";

    stop.store(true);
    sync_run_at(0, stop_accept(&listener));

    const auto begin = std::chrono::steady_clock::now();
    scheduler::stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_LT(elapsed, BUDGET) << "stop() waited on a pending accept";
    EXPECT_EQ(scheduler::engine_at(0).ring().running_io(), 0U);
}
