#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <llco/core/resume_on.hpp>
#include <llco/io/op.decl.hpp>
#include <llco/llco.hpp>
#include <llco/net/socket.decl.hpp>
#include <print>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto     STOP_POLL    = 50ms;
constexpr auto     PORT         = std::uint16_t{7000};
constexpr auto     CHUNK        = std::size_t{4096};
constexpr unsigned ENGINE_COUNT = 4;

volatile std::sig_atomic_t g_interrupted = 0;

void on_signal(int) {
    g_interrupted = 1;
}

// Echoes one connection; the scheduler already placed it on this engine.
task<> echo(int fd, std::vector<std::atomic<unsigned>> *landed) {
    for (unsigned i = 0; i < scheduler::size(); ++i)
        if (&scheduler::engine_at(i) == current_engine())
            (*landed)[i].fetch_add(1);
    net::connection         conn{current_engine()->ring(), fd};
    std::array<char, CHUNK> buffer{};
    for (;;) {
        const auto bytes = co_await conn.recv(buffer);
        if (!bytes) {
            std::println("[echo] {}", bytes.error().message()); // a peer reset is normal here
            break;
        }
        if (*bytes == 0)
            break;
        (void)co_await conn.send(std::string_view{buffer.data(), *bytes});
        std::println("[echo] {} bytes", *bytes);
    }
    conn.shutdown();
}

// Runs on engine 0: accepts, then lets the scheduler place the connection.
task<> accept_loop(net::listener *listener, std::vector<std::atomic<unsigned>> *landed) {
    for (;;) {
        const auto accepted = co_await listener->accept_fd();
        if (!accepted) {
            if (accepted.error() == std::errc::operation_canceled)
                co_return;
            std::println("[accept] {}", accepted.error().message());
            continue;
        }
        std::println("[accept] fd {}", *accepted);
        if (!scheduler::submit(echo(*accepted, landed)))
            std::println("[accept] queue full, connection dropped");
        // A connection can land between the signal and the cancel; leave instead of arming
        // another accept that nothing would ever interrupt.
        if (g_interrupted != 0)
            co_return;
    }
}

// Runs on the acceptor's engine: the cancel belongs to that ring.
task<> stop_accept(net::listener *listener) {
    co_await resume_on(scheduler::engine_at(0));
    const auto canceled = co_await listener->cancel();
    std::println("[cancel] listener {}: {}", listener->fd(),
                 (canceled && *canceled) ? "canceled" : "already idle");
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);  // NOLINT(cert-err33-c): a demo has nothing to do if this fails
    std::signal(SIGTERM, on_signal); // NOLINT(cert-err33-c): same

    scheduler::init(ENGINE_COUNT);
    net::listener                      listener{scheduler::engine_at(0).ring(), PORT};
    std::vector<std::atomic<unsigned>> landed(ENGINE_COUNT);
    std::println("[listen] 127.0.0.1:{} on {} engines", listener.port(), scheduler::size());

    if (!scheduler::engine_at(0).spawn(accept_loop(&listener, &landed)))
        return 1;

    while (g_interrupted == 0)
        std::this_thread::sleep_for(STOP_POLL);

    sync_wait(stop_accept(&listener));
    scheduler::stop(); // waits for live connections too: close them before Ctrl-C
    for (unsigned i = 0; i < scheduler::size(); ++i)
        std::println("[stop] engine {}: {} connections, {} loops", i, landed[i].load(),
                     scheduler::engine_at(i).loop_count());
}
