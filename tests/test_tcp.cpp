#include <gtest/gtest.h>
#include <llco/core/task.hpp>
#include <llco/io/op.decl.hpp>
#include <llco/io/ring.decl.hpp>
#include <llco/net/socket.decl.hpp>
#include <llco/runtime/engine.decl.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto BUDGET        = 1s;
constexpr auto POLL_INTERVAL = 1ms;
constexpr auto PAYLOAD       = std::string_view{"hello tcp"};
constexpr auto CHUNK         = std::size_t{64};
constexpr auto SPARE         = std::size_t{8};

// Polls until the predicate holds, or the budget runs out.
template <class Predicate>
auto wait_until(Predicate done) -> bool {
    const auto end = std::chrono::steady_clock::now() + BUDGET;
    while (!done() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(POLL_INTERVAL);
    return done();
}

// One echo round trip through the wrappers: accept, read once, write it back.
task<> echo_once(net::listener *listener, std::atomic<bool> *served) {
    auto accepted = co_await listener->accept();
    EXPECT_TRUE(accepted) << accepted.error().message();
    if (!accepted)
        co_return;
    auto                    conn = std::move(*accepted);
    std::array<char, CHUNK> buffer{};
    const auto              got = co_await conn.recv(buffer);
    EXPECT_TRUE(got) << got.error().message();
    if (!got)
        co_return;
    (void)co_await conn.send(std::string_view{buffer.data(), *got});
    served->store(true);
}

// Connects, sends the payload, then reads the echo back into reply.
task<> echo_client(engine *e, std::uint16_t port, std::span<char> reply, std::atomic<bool> *done) {
    net::connection conn{e->ring(), net::tcp_socket()};
    const auto      connected = co_await io::connect(e->ring(), conn.fd(), net::loopback(port));
    EXPECT_TRUE(connected) << connected.error().message();
    if (!connected)
        co_return;
    (void)co_await conn.send(PAYLOAD);
    (void)co_await conn.recv(reply);
    done->store(true);
}

task<> connect_refused(engine *e, std::uint16_t port, std::atomic<int> *code,
                       std::atomic<bool> *done) {
    const int  fd      = net::tcp_socket();
    const auto refused = co_await io::connect(e->ring(), fd, net::loopback(port));
    code->store(refused ? 0 : refused.error().value());
    done->store(true);
    ::close(fd);
}

// Reads the payload, echoes it, then keeps reading: the peer's half close must read as EOF.
task<> half_close_server(net::listener *listener, std::atomic<bool> *eof,
                         std::atomic<bool> *done) {
    auto accepted = co_await listener->accept();
    EXPECT_TRUE(accepted) << accepted.error().message();
    if (!accepted)
        co_return;
    auto                    conn = std::move(*accepted);
    std::array<char, CHUNK> buffer{};
    const auto              got = co_await conn.recv(buffer);
    EXPECT_TRUE(got) << got.error().message();
    if (!got)
        co_return;
    (void)co_await conn.send(std::string_view{buffer.data(), *got});
    const auto rest = co_await conn.recv(buffer);
    EXPECT_TRUE(rest) << rest.error().message();
    eof->store(rest && *rest == 0);
    done->store(true);
    conn.shutdown();
}

// Sends the payload, shuts down its write side, then drains the echo and the final EOF.
task<> half_close_client(engine *e, std::uint16_t port, std::span<char> reply,
                         std::atomic<bool> *eof, std::atomic<bool> *done) {
    net::connection conn{e->ring(), net::tcp_socket()};
    const auto      connected = co_await io::connect(e->ring(), conn.fd(), net::loopback(port));
    EXPECT_TRUE(connected) << connected.error().message();
    if (!connected)
        co_return;
    (void)co_await conn.send(PAYLOAD);
    conn.shutdown();
    (void)co_await conn.recv(reply);
    std::array<char, SPARE> spare{};
    const auto              rest = co_await conn.recv(spare);
    EXPECT_TRUE(rest) << rest.error().message();
    eof->store(rest && *rest == 0);
    done->store(true);
}

} // namespace

TEST(net, listen_tcp_reports_its_port) {
    const int listener = net::listen_tcp(0);
    const auto port    = net::local_port(listener);
    EXPECT_NE(port, 0) << "the kernel picked port 0";
    ::close(listener);
}

TEST(tcp, connect_and_echo) {
    std::array<char, PAYLOAD.size()> reply{};
    std::atomic<bool>                served{false};
    std::atomic<bool>                echoed{false};

    engine e;
    e.start();
    net::listener listener{e.ring(), 0};

    ASSERT_TRUE(e.spawn(echo_once(&listener, &served)));
    ASSERT_TRUE(e.spawn(echo_client(&e, listener.port(), reply, &echoed)));
    ASSERT_TRUE(wait_until([&] { return served.load() && echoed.load(); }))
        << "the round trip never finished";
    e.stop();

    const std::string_view back{reply.data(), reply.size()};
    EXPECT_EQ(back, PAYLOAD);
    EXPECT_EQ(e.ring().running_io(), 0U);
}

TEST(tcp, connect_refused) {
    const int  listener = net::listen_tcp(0);
    const auto port     = net::local_port(listener);
    ::close(listener); // nobody is listening on that port now

    std::atomic<int>  code{0};
    std::atomic<bool> done{false};

    engine e;
    e.start();
    ASSERT_TRUE(e.spawn(connect_refused(&e, port, &code, &done)));
    ASSERT_TRUE(wait_until([&] { return done.load(); })) << "the failed connect never came back";
    e.stop();

    EXPECT_EQ(code.load(), ECONNREFUSED);
}

TEST(tcp, half_close_ends_the_stream) {
    std::array<char, PAYLOAD.size()> reply{};
    std::atomic<bool>                server_eof{false};
    std::atomic<bool>                client_eof{false};
    std::atomic<bool>                served{false};
    std::atomic<bool>                echoed{false};

    engine e;
    e.start();
    net::listener listener{e.ring(), 0};

    ASSERT_TRUE(e.spawn(half_close_server(&listener, &server_eof, &served)));
    ASSERT_TRUE(e.spawn(half_close_client(&e, listener.port(), reply, &client_eof, &echoed)));
    ASSERT_TRUE(wait_until([&] { return served.load() && echoed.load(); }))
        << "the half-closed conversation never finished";
    e.stop();

    const std::string_view back{reply.data(), reply.size()};
    EXPECT_EQ(back, PAYLOAD);
    EXPECT_TRUE(server_eof.load()) << "the server did not see EOF after the half close";
    EXPECT_TRUE(client_eof.load()) << "the client did not see EOF after the echo";
}
