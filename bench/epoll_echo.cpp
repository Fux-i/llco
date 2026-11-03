// Single-threaded epoll echo server: the control group for llco's echo_server.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <print>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t LISTENER_TAG = 0;
constexpr std::uint64_t SIGNAL_TAG   = 1;
constexpr std::size_t   BATCH        = 64; // epoll events per wait

// Command-line configuration; the defaults match examples/echo_server.cpp.
struct config {
    std::uint16_t port  = 7000;
    std::size_t   chunk = 4096;
    bool          any   = false;
};

// Aborts with one line: the server cannot run without a listening socket.
[[noreturn]]
void fail(std::string_view msg) {
    std::println(stderr, "error: {}", msg);
    (void)std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

// Parses one flag value as an unsigned integer, rejecting trailing junk.
auto parse_u64(std::string_view text, std::string_view flag) -> std::uint64_t {
    const char *const first = text.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): a [first, last) range
    const char       *last  = first + text.size();
    std::uint64_t     value = 0;
    const auto [end, ec]    = std::from_chars(first, last, value);
    if (ec != std::errc{} || end != last)
        fail(std::format("bad value for {}", flag));
    return value;
}

// Parses the whole command line; every flag takes a value except --any.
auto parse(int argc, char **argv) -> config {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
        const std::string_view arg{argv[i]};
        if (arg == "--any") {
            cfg.any = true;
            continue;
        }
        if (i + 1 >= argc)
            fail(std::format("missing value for {}", arg));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
        const std::string_view value{argv[++i]};
        if (arg == "--port")
            cfg.port = static_cast<std::uint16_t>(parse_u64(value, arg));
        else if (arg == "--chunk")
            cfg.chunk = parse_u64(value, arg);
        else
            fail(std::format("unknown flag {}", arg));
    }
    if (cfg.chunk == 0)
        fail("--chunk must be positive");
    return cfg;
}

// Counters reported on SIGUSR1 and at exit.
struct stats {
    std::uint64_t accepted  = 0;
    std::uint64_t bytes_in  = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t errors    = 0;
};

// One accepted connection: the current echo stays in buf until the socket drains.
struct connection {
    int               fd      = -1;
    std::uint32_t     armed   = 0;
    std::size_t       out_off = 0; // bytes of buf already sent
    std::size_t       out_len = 0; // bytes of buf that still owe a send
    std::vector<char> buf;
};

// Single-threaded level-triggered epoll server: read up to chunk bytes, write them straight back.
class server {
    config                                   m_cfg;
    int                                      m_epoll_fd  = -1;
    int                                      m_listen_fd = -1;
    int                                      m_signal_fd = -1;
    bool                                     m_running   = true;
    stats                                    m_stats;
    std::vector<std::unique_ptr<connection>> m_table; // indexed by fd

  public:
    // Keeps the configuration; the sockets are opened by run().
    explicit server(config cfg) : m_cfg{cfg} {}

    // Runs the accept/echo loop until SIGINT or SIGTERM.
    void run() {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0)
            fail(std::format("socket: {}", std::strerror(errno)));
        const int one = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(m_cfg.port);
        addr.sin_addr.s_addr = htonl(m_cfg.any ? INADDR_ANY : INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_in is a sockaddr
        if (::bind(m_listen_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof addr) < 0)
            fail(std::format("bind: {}", std::strerror(errno)));
        ::listen(m_listen_fd, SOMAXCONN);

        sigset_t set;
        ::sigemptyset(&set);
        ::sigaddset(&set, SIGUSR1);
        ::sigaddset(&set, SIGINT);
        ::sigaddset(&set, SIGTERM);
        ::sigprocmask(SIG_BLOCK, &set, nullptr);
        m_signal_fd = ::signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);

        m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event listener_ev{.events = EPOLLIN, .data = {.u64 = LISTENER_TAG}};
        epoll_event signal_ev{.events = EPOLLIN, .data = {.u64 = SIGNAL_TAG}};
        ::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_listen_fd, &listener_ev);
        ::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_signal_fd, &signal_ev);

        std::println("[listen] {}:{} chunk={}", m_cfg.any ? "0.0.0.0" : "127.0.0.1", m_cfg.port,
                     m_cfg.chunk);
        (void)std::fflush(stdout);

        std::array<epoll_event, BATCH> events{};
        while (m_running) {
            const int ready = ::epoll_wait(m_epoll_fd, events.data(), BATCH, -1);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                fail(std::format("epoll_wait: {}", std::strerror(errno)));
            }
            const auto ready_events = std::span{events}.first(static_cast<std::size_t>(ready));
            for (const epoll_event &ev : ready_events) {
                const std::uint64_t tag = ev.data.u64;
                if (tag == LISTENER_TAG)
                    accept_all();
                else if (tag == SIGNAL_TAG)
                    handle_signal();
                else
                    handle(static_cast<int>(tag), ev.events);
            }
        }
        report("stats"); // the driver reconciles this line against the client's msgs
    }

  private:
    // Drains the accept queue; each accepted socket is non-blocking and no-delay.
    void accept_all() {
        for (;;) {
            const int fd = ::accept4(m_listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                ++m_stats.errors;
                return;
            }
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            if (static_cast<std::size_t>(fd) >= m_table.size())
                m_table.resize(static_cast<std::size_t>(fd) + 1);
            m_table[static_cast<std::size_t>(fd)] = std::make_unique<connection>(
                connection{.fd = fd, .buf = std::vector<char>(m_cfg.chunk)});
            epoll_event ev{.events = EPOLLIN, .data = {.u64 = static_cast<std::uint64_t>(fd)}};
            ::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
            m_table[static_cast<std::size_t>(fd)]->armed = EPOLLIN;
            ++m_stats.accepted;
        }
    }

    // Routes one connection event; a pending echo is flushed before more is read.
    void handle(int fd, std::uint32_t events) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            ++m_stats.errors;
            close_conn(fd);
            return;
        }
        if (events & EPOLLOUT) {
            flush(fd);
            if (m_table[static_cast<std::size_t>(fd)] == nullptr)
                return;
        }
        if (events & EPOLLIN)
            read_echo(fd);
    }

    // Reads at most one chunk and echoes it immediately, like llco's echo_server.
    void read_echo(int fd) {
        connection &c = *m_table[static_cast<std::size_t>(fd)];
        const auto  n = ::recv(fd, c.buf.data(), c.buf.size(), 0);
        if (n == 0) {
            close_conn(fd);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            ++m_stats.errors;
            close_conn(fd);
            return;
        }
        m_stats.bytes_in += static_cast<std::uint64_t>(n);
        c.out_off = 0;
        c.out_len = static_cast<std::size_t>(n);
        flush(fd);
    }

    // Writes the pending echo; a short write leaves the tail for EPOLLOUT.
    void flush(int fd) {
        connection &c = *m_table[static_cast<std::size_t>(fd)];
        while (c.out_off < c.out_len) {
            const auto n =
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): inside out_buf
                ::send(fd, c.buf.data() + c.out_off, c.out_len - c.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<std::size_t>(n);
                m_stats.bytes_out += static_cast<std::uint64_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                arm(fd, EPOLLOUT);
                return;
            }
            ++m_stats.errors;
            close_conn(fd);
            return;
        }
        arm(fd, EPOLLIN);
    }

    // Changes the interest set only when it actually changes.
    void arm(int fd, std::uint32_t mask) {
        connection &c = *m_table[static_cast<std::size_t>(fd)];
        if (c.armed == mask)
            return;
        epoll_event ev{.events = mask, .data = {.u64 = static_cast<std::uint64_t>(fd)}};
        ::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        c.armed = mask;
    }

    // Drops a connection; close() also removes it from the epoll set.
    void close_conn(int fd) {
        ::close(fd);
        m_table[static_cast<std::size_t>(fd)].reset();
    }

    // Consumes signalfd records: SIGUSR1 reports stats, SIGINT/SIGTERM stops the loop.
    void handle_signal() {
        signalfd_siginfo info{};
        while (::read(m_signal_fd, &info, sizeof info) == static_cast<ssize_t>(sizeof info)) {
            if (info.ssi_signo == SIGUSR1) {
                report("stats");
                continue;
            }
            m_running = false;
            return;
        }
    }

    // Prints the counters on one line and flushes, so a piped log still shows them live.
    void report(std::string_view tag) const {
        std::println("[{}] accepted={} bytes_in={} bytes_out={} errors={}", tag, m_stats.accepted,
                     m_stats.bytes_in, m_stats.bytes_out, m_stats.errors);
        (void)std::fflush(stdout);
    }
};

} // namespace

// Entry point: parse the flags and serve until a signal arrives.
auto main(int argc, char **argv) -> int {
    server{parse(argc, argv)}.run();
    return 0;
}
