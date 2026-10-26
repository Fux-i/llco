#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <llco/io/io_uring.decl.hpp>
#include <llco/llco.hpp>
#include <llco/net/socket.decl.hpp>
#include <print>
#include <string_view>
#include <system_error>
#include <thread>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto STOP_POLL = 50ms;
constexpr auto PORT      = std::uint16_t{7000};
constexpr auto CHUNK     = std::size_t{4096};

volatile std::sig_atomic_t g_interrupted = 0;

void on_signal(int) {
    g_interrupted = 1;
}

// Echoes one connection until the peer stops writing; the connection closes itself.
task<> echo(net::connection conn) {
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

// Accepts until the listener is cancelled; every connection gets its own task.
task<> accept_loop(engine *e, net::listener *listener) {
    for (;;) {
        try {
            auto conn = co_await listener->accept();
            std::println("[accept] fd {}", conn.fd());
            if (!e->spawn(echo(std::move(conn))))
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

// Runs on the engine thread: the hop comes first, the cancel belongs to that ring.
task<> stop_accept(engine *e, net::listener *listener) {
    co_await resume_on(*e);
    const bool canceled = co_await listener->cancel();
    std::println("[cancel] listener {}: {}", listener->fd(), canceled ? "canceled" : "already idle");
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);  // NOLINT(cert-err33-c): a demo has nothing to do if this fails
    std::signal(SIGTERM, on_signal); // NOLINT(cert-err33-c): same

    engine e;
    e.start();
    net::listener listener{e.ring(), PORT};
    std::println("[listen] 127.0.0.1:{}", listener.port());

    if (!e.spawn(accept_loop(&e, &listener)))
        return 1;

    while (g_interrupted == 0)
        std::this_thread::sleep_for(STOP_POLL);

    sync_wait(stop_accept(&e, &listener));
    e.stop();
    std::println("[stop] {} loop iterations, running io: {}", e.loop_count(),
                 e.ring().running_io());
}
