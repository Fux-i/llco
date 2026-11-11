#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <llco/config.hpp>
#include <llco/io/op.decl.hpp>
#include <llco/llco.hpp>
#include <llco/net/socket.decl.hpp>
#include <mutex>
#include <print>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <csignal>
#include <netinet/tcp.h>
#include <pthread.h>
#include <unistd.h>

using namespace llco;

namespace {

constexpr auto CHUNK = std::size_t{4096};

struct options {
    std::uint16_t         port    = 7000;
    unsigned              engines = 4;
    std::vector<unsigned> cpus; // engine i pins to cpus[i % size]; empty leaves them unpinned
};

// Splits a comma-separated cpu list, the shape taskset takes.
auto parse_cpus(std::string_view text) -> std::vector<unsigned> {
    std::vector<unsigned> cpus;
    for (std::size_t at = 0; at < text.size();) {
        const auto comma = text.find(',', at);
        const auto stop  = comma == std::string_view::npos ? text.size() : comma;
        cpus.push_back(static_cast<unsigned>(std::stoi(std::string{text.substr(at, stop - at)})));
        at = stop + 1;
    }
    return cpus;
}

// Reads --port, --engines and --cpus; any other argument is a usage error.
auto parse(int argc, char **argv) -> options {
    options opt;
    for (int i = 1; i < argc; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
        const std::string_view arg{argv[i]};
        if (arg == "--port" && i + 1 < argc) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
            opt.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--engines" && i + 1 < argc) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
            opt.engines = static_cast<unsigned>(std::stoi(argv[++i]));
        } else if (arg == "--cpus" && i + 1 < argc) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
            opt.cpus = parse_cpus(argv[++i]);
        } else {
            std::println(stderr, "usage: echo_bench [--port N] [--engines N] [--cpus 2,4,6]");
            std::exit(2);
        }
    }
    return opt;
}

struct counters {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> live{0};
    std::atomic<std::uint64_t> bytes_in{0};
    std::atomic<std::uint64_t> bytes_out{0};
    std::atomic<std::uint64_t> errors{0};
};

counters g_counters;

// One entry per live connection (fd, owning engine), so shutdown can cancel every blocked recv.
std::mutex                             g_live_mutex;
std::vector<std::pair<int, engine *>> g_live;

// One line per run; the driver parses this, so the shape must stay stable.
void print_stats() {
    std::println("[stats] accepted={} live={} bytes_in={} bytes_out={} errors={}",
                 g_counters.accepted.load(), g_counters.live.load(),
                 g_counters.bytes_in.load(), g_counters.bytes_out.load(),
                 g_counters.errors.load());
    (void)std::fflush(stdout);
}

// Echoes one connection until the peer stops writing or shutdown cancels the recv.
task<> echo(int fd) {
    net::connection conn{fd};
    const int       one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    {
        const std::scoped_lock lock{g_live_mutex};
        g_live.emplace_back(fd, current_engine());
    }
    g_counters.live.fetch_add(1);

    std::array<char, CHUNK> buffer{};
    for (;;) {
        const auto got = co_await conn.recv(buffer);
        if (!got) {
            if (got.error() != std::errc::operation_canceled)
                g_counters.errors.fetch_add(1);
            break;
        }
        if (*got == 0)
            break;
        g_counters.bytes_in.fetch_add(*got);
        for (std::size_t sent = 0; sent < *got;) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): bounded by *got
            const auto out = co_await conn.send(std::string_view{buffer.data() + sent, *got - sent});
            if (!out) {
                g_counters.errors.fetch_add(1);
                break;
            }
            sent += *out;
            g_counters.bytes_out.fetch_add(*out);
        }
    }

    {
        const std::scoped_lock lock{g_live_mutex};
        std::erase_if(g_live, [fd](const auto &entry) { return entry.first == fd; });
    }
    g_counters.live.fetch_sub(1);
    conn.shutdown();
}

// Runs on engine 0: accepts, then lets the scheduler place the connection.
task<> accept_loop(net::listener *listener) {
    for (;;) {
        const auto fd = co_await listener->accept_fd();
        if (!fd) {
            if (fd.error() == std::errc::operation_canceled)
                co_return;
            // EMFILE and friends are transient: keep serving the connections we have.
            g_counters.errors.fetch_add(1);
            continue;
        }
        g_counters.accepted.fetch_add(1);
        if (!scheduler::submit(echo(*fd))) {
            g_counters.errors.fetch_add(1);
            ::close(*fd);
        }
    }
}

// Cancels the recv this connection is blocked on; hops to the engine that owns the ring.
task<> cancel_conn(int fd, engine *owner) {
    co_await resume_on(*owner);
    (void)co_await io::cancel(current_engine()->ring(), fd);
}

// Cancels the pending accept; sync_run_at(0, ...) is what puts it on engine 0.
task<> cancel_accept(net::listener *listener) {
    (void)co_await listener->cancel();
}

} // namespace

int main(int argc, char **argv) {
    const auto opt = parse(argc, argv);

    // Block the signals before any engine thread exists: they inherit the mask, so sigwait() is
    // the only receiver and Ctrl-C cannot kill the process behind stop()'s back.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    scheduler::init(opt.engines, opt.cpus);
    net::listener listener{opt.port};
    std::println("[listen] 127.0.0.1:{}", listener.port());
    (void)std::fflush(stdout);
    run_at(0, accept_loop(&listener));

    int signal_received = 0;
    sigwait(&set, &signal_received);

    // Stop serving: cancel the accept, then every blocked recv, then let stop() drain.
    sync_run_at(0, cancel_accept(&listener));
    for (const auto &[fd, owner] : std::exchange(g_live, {}))
        sync_run(cancel_conn(fd, owner));
    scheduler::stop();
    print_stats();
}