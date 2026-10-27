#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <llco/core/resume_on.hpp>
#include <llco/io/io_uring.decl.hpp>
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
task<> echo(scheduler *s, int fd, std::vector<std::atomic<unsigned>> *landed) {
    for (unsigned i = 0; i < s->size(); ++i)
        if (&s->engine_at(i) == current_engine())
            (*landed)[i].fetch_add(1);
    net::connection         conn{current_engine()->ring(), fd};
    std::array<char, CHUNK> buffer{};
    try {
        for (;;) {
            const auto bytes = co_await conn.recv(buffer);
            if (bytes == 0)
                break;
            (void)co_await conn.send(std::string_view{buffer.data(), bytes});
            std::println("[echo] {} bytes", bytes);
        }
    } catch (const std::system_error &error) {
        // a peer reset is normal here; the fd still has to go
        std::println("[echo] {}", error.what());
    }
    conn.shutdown();
}

// Runs on engine 0: accepts, then lets the scheduler place the connection.
task<> accept_loop(scheduler *s, net::listener *listener,
                   std::vector<std::atomic<unsigned>> *landed) {
    for (;;) {
        try {
            const int fd = co_await listener->accept_fd();
            std::println("[accept] fd {}", fd);
            if (!s->submit(echo(s, fd, landed)))
                std::println("[accept] queue full, connection dropped");
        } catch (const std::system_error &error) {
            if (error.code().value() == ECANCELED)
                co_return;
            throw;
        }
        // A connection can land between the signal and the cancel; leave instead of arming
        // another accept that nothing would ever interrupt.
        if (g_interrupted != 0)
            co_return;
    }
}

// Runs on the acceptor's engine: the cancel belongs to that ring.
task<> stop_accept(scheduler *s, net::listener *listener) {
    co_await resume_on(s->engine_at(0));
    const bool canceled = co_await listener->cancel();
    std::println("[cancel] listener {}: {}", listener->fd(), canceled ? "canceled" : "already idle");
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);  // NOLINT(cert-err33-c): a demo has nothing to do if this fails
    std::signal(SIGTERM, on_signal); // NOLINT(cert-err33-c): same

    scheduler                         s{ENGINE_COUNT};
    net::listener                     listener{s.engine_at(0).ring(), PORT};
    std::vector<std::atomic<unsigned>> landed(ENGINE_COUNT);
    std::println("[listen] 127.0.0.1:{} on {} engines", listener.port(), s.size());

    if (!s.engine_at(0).spawn(accept_loop(&s, &listener, &landed)))
        return 1;

    while (g_interrupted == 0)
        std::this_thread::sleep_for(STOP_POLL);

    sync_wait(stop_accept(&s, &listener));
    s.stop(); // waits for live connections too: close them before Ctrl-C
    for (unsigned i = 0; i < s.size(); ++i)
        std::println("[stop] engine {}: {} connections, {} loops", i, landed[i].load(),
                     s.engine_at(i).loop_count());
}
