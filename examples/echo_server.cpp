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

#include <csignal>
#include <pthread.h>

using namespace llco;

namespace {

constexpr auto PORT  = std::uint16_t{7000};
constexpr auto CHUNK = std::size_t{4096};

// Echoes one connection until the peer stops writing.
task<> echo(int fd) {
    const trace::scope      slice{"echo"}; // spans the whole connection
    net::connection         conn{current_engine()->ring(), fd};
    std::array<char, CHUNK> buffer{};
    for (;;) {
        const auto bytes = co_await conn.recv(buffer);
        if (!bytes || *bytes == 0)
            break;
        co_await conn.send(std::string_view{buffer.data(), *bytes});
        std::println("[echo] {} bytes", *bytes);
    }
    conn.shutdown();
    co_return;
}

// Runs on engine 0: accepts, then lets the scheduler place the connection.
task<> accept_loop(net::listener *listener) {
    for (;;) {
        const auto fd = co_await listener->accept_fd();
        if (!fd)
            co_return; // the cancel below is the normal way out
        std::println("[accept] fd {}", *fd);
        run(echo(*fd));
    }
}

// Cancels the pending accept; it must run on the listener's engine.
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
    net::listener listener{scheduler::engine_at(0).ring(), PORT};
    std::println("[listen] 127.0.0.1:{}", listener.port());
    (void)scheduler::engine_at(0).spawn(accept_loop(&listener));

    int received = 0;
    sigwait(&set, &received);

    (void)scheduler::engine_at(0).spawn(cancel_accept(&listener));
    scheduler::stop(); // drains the cancel, then waits for the live connections
#if LLCO_TRACE
    (void)trace::flush("trace.json");
#endif
}
