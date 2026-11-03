#include <array>
#include <cstddef>
#include <cstdint>
#include <llco/config.hpp>
#include <llco/io/op.decl.hpp>
#include <llco/llco.hpp>
#include <llco/net/socket.decl.hpp>
#include <llco/trace/trace.decl.hpp>
#include <print>
#include <string_view>
#include <system_error>

#include <csignal>
#include <netinet/tcp.h>
#include <pthread.h>
#include <unistd.h>

using namespace llco;

namespace {

constexpr auto PORT  = std::uint16_t{7000};
constexpr auto CHUNK = std::size_t{4096};

// Echoes one connection until the peer stops writing.
task<> echo(int fd) {
#if LLCO_TRACE
    const trace::async_scope slice{"echo"}; // survives the suspensions, so it needs no nesting
#endif
    net::connection         conn{fd};
    const int               one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // no Nagle delay
    std::array<char, CHUNK> buffer{};
    for (;;) {
        const auto bytes = co_await conn.recv(buffer);
        if (!bytes || *bytes == 0)
            break;
        co_await conn.send(std::string_view{buffer.data(), *bytes});
        std::println("[echo] {} bytes", *bytes);
    }
    std::println("[close] fd {}", fd);
    conn.shutdown();
    co_return;
}

// Runs on engine 0: accepts, then lets the scheduler place the connection.
task<> accept_loop(net::listener *listener) {
    for (;;) {
        const auto fd = co_await listener->accept_fd();
        if (!fd) {
            if (fd.error() == std::errc::operation_canceled)
                co_return; // the cancel below is the normal way out
            continue;      // EMFILE and friends are transient
        }
        std::println("[accept] fd {}", *fd);
        if (!scheduler::submit(echo(*fd)))
            ::close(*fd); // queue full: drop the connection, not the process (ADR-0004)
    }
}

// Cancels the pending accept; sync_run_at(0, ...) is what puts it on engine 0.
task<> cancel_accept(net::listener *listener) {
    co_await listener->cancel();
    co_return;
}

} // namespace

int main() {
#if LLCO_TRACE
    trace::set_thread_name("main");
#endif

    // Block the signals before any engine thread exists: they inherit the mask, so sigwait() is
    // the only receiver and Ctrl-C cannot kill the process behind stop()'s back.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    scheduler::init(4);
    net::listener listener{PORT};
    std::println("[listen] 127.0.0.1:{}", listener.port());
    run_at(0, accept_loop(&listener));

    int received = 0;
    sigwait(&set, &received);

    sync_run_at(0, cancel_accept(&listener));
    scheduler::stop(); // drains the cancel, then waits for the live connections
#if LLCO_TRACE
    (void)trace::flush("trace.json");
#endif
}
