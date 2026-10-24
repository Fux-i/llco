#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <llco/io/io_uring.decl.hpp>
#include <llco/llco.hpp>
#include <llco/net/socket.decl.hpp>
#include <print>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

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

// Echoes one connection until the peer stops writing, then closes it.
task<> echo_connection(engine *e, int fd) {
    std::array<std::byte, CHUNK> buffer{};
    try {
        for (;;) {
            const auto bytes = co_await io::recv(e->ring(), fd, buffer);
            if (bytes == 0)
                break;
            (void)co_await io::send(e->ring(), fd, std::span{buffer}.first(bytes));
            std::println("[echo] {} bytes", bytes);
        }
    } catch (const std::system_error &error) {
        // a peer reset is normal here; the fd still has to go
        std::println("[echo] {}", error.what());
    }
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
}

// Accepts until the listener is cancelled; every connection gets its own task.
task<> accept_loop(engine *e, int listener) {
    for (;;) {
        int fd = 0;
        try {
            fd = co_await io::accept(e->ring(), listener);
        } catch (const std::system_error &error) {
            if (error.code().value() == ECANCELED)
                co_return;
            throw;
        }
        std::println("[accept] fd {}", fd);
        if (!e->spawn(echo_connection(e, fd))) {
            ::close(fd);
            std::println("[accept] queue full, connection dropped");
        }
        // A connection can land between the signal and the cancel; leave instead of arming
        // another accept that nothing would ever interrupt.
        if (g_interrupted != 0)
            co_return;
    }
}

// Runs on the engine thread: the hop comes first, the cancel belongs to that ring.
task<> cancel_accept(engine *e, int listener) {
    co_await resume_on(*e);
    const bool canceled = co_await io::cancel(e->ring(), listener);
    std::println("[cancel] listener {}: {}", listener, canceled ? "canceled" : "already idle");
}

} // namespace

int main() {
    std::signal(SIGINT, on_signal);  // NOLINT(cert-err33-c): a demo has nothing to do if this fails
    std::signal(SIGTERM, on_signal); // NOLINT(cert-err33-c): same

    const int listener = net::listen_tcp(PORT);
    std::println("[listen] 127.0.0.1:{}", net::local_port(listener));

    engine e;
    e.start();
    if (!e.spawn(accept_loop(&e, listener)))
        return 1;

    while (g_interrupted == 0)
        std::this_thread::sleep_for(STOP_POLL);

    sync_wait(cancel_accept(&e, listener));
    e.stop();
    ::close(listener);
    std::println("[stop] {} loop iterations, running io: {}", e.loop_count(),
                 e.ring().running_io());
}
