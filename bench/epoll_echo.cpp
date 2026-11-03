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
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

// Parses one flag value as an unsigned integer, rejecting trailing junk.
auto parse_u64(std::string_view text, std::string_view flag) -> std::uint64_t {
    std::uint64_t value  = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size())
        fail(std::format("bad value for {}", flag));
    return value;
}

// Parses the whole command line; every flag takes a value except --any.
auto parse(int argc, char **argv) -> config {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--any") {
            cfg.any = true;
            continue;
        }
        if (i + 1 >= argc)
            fail(std::format("missing value for {}", arg));
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
    config                                   cfg_;
    int                                      epoll_fd_  = -1;
    int                                      listen_fd_ = -1;
    int                                      signal_fd_ = -1;
    bool                                     running_   = true;
    stats                                    st_;
    std::vector<std::unique_ptr<connection>> table_; // indexed by fd

  public:
    // Keeps the configuration; the sockets are opened by run().
    explicit server(config cfg) : cfg_{cfg} {}

    // Runs the accept/echo loop until SIGINT or SIGTERM.
    void run() {
        listen_fd_    = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(cfg_.port);
        addr.sin_addr.s_addr = htonl(cfg_.any ? INADDR_ANY : INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr *>(&addr), sizeof addr) < 0)
            fail(std::format("bind: {}", std::strerror(errno)));
        ::listen(listen_fd_, SOMAXCONN);

        sigset_t set;
        ::sigemptyset(&set);
        ::sigaddset(&set, SIGUSR1);
        ::sigaddset(&set, SIGINT);
        ::sigaddset(&set, SIGTERM);
        ::sigprocmask(SIG_BLOCK, &set, nullptr);
        signal_fd_ = ::signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);

        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event listener_ev{.events = EPOLLIN, .data = {.u64 = LISTENER_TAG}};
        epoll_event signal_ev{.events = EPOLLIN, .data = {.u64 = SIGNAL_TAG}};
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &listener_ev);
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, signal_fd_, &signal_ev);

        std::println("[listen] {}:{} chunk={}", cfg_.any ? "0.0.0.0" : "127.0.0.1", cfg_.port,
                     cfg_.chunk);
        std::fflush(stdout);

        std::array<epoll_event, BATCH> events{};
        while (running_) {
            const int ready = ::epoll_wait(epoll_fd_, events.data(), BATCH, -1);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                fail(std::format("epoll_wait: {}", std::strerror(errno)));
            }
            for (int i = 0; i < ready; ++i) {
                const std::uint64_t tag = events[i].data.u64;
                if (tag == LISTENER_TAG)
                    accept_all();
                else if (tag == SIGNAL_TAG)
                    handle_signal();
                else
                    handle(static_cast<int>(tag), events[i].events);
            }
        }
        report("stats"); // the driver reconciles this line against the client's msgs
    }

  private:
    // Drains the accept queue; each accepted socket is non-blocking and no-delay.
    void accept_all() {
        for (;;) {
            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                ++st_.errors;
                return;
            }
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            if (static_cast<std::size_t>(fd) >= table_.size())
                table_.resize(static_cast<std::size_t>(fd) + 1);
            table_[static_cast<std::size_t>(fd)] = std::make_unique<connection>(
                connection{.fd = fd, .buf = std::vector<char>(cfg_.chunk)});
            epoll_event ev{.events = EPOLLIN, .data = {.u64 = static_cast<std::uint64_t>(fd)}};
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
            table_[static_cast<std::size_t>(fd)]->armed = EPOLLIN;
            ++st_.accepted;
        }
    }

    // Routes one connection event; a pending echo is flushed before more is read.
    void handle(int fd, std::uint32_t events) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            ++st_.errors;
            close_conn(fd);
            return;
        }
        if (events & EPOLLOUT) {
            flush(fd);
            if (table_[static_cast<std::size_t>(fd)] == nullptr)
                return;
        }
        if (events & EPOLLIN)
            read_echo(fd);
    }

    // Reads at most one chunk and echoes it immediately, like llco's echo_server.
    void read_echo(int fd) {
        connection &c = *table_[static_cast<std::size_t>(fd)];
        const auto  n = ::recv(fd, c.buf.data(), c.buf.size(), 0);
        if (n == 0) {
            close_conn(fd);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            ++st_.errors;
            close_conn(fd);
            return;
        }
        st_.bytes_in += static_cast<std::uint64_t>(n);
        c.out_off = 0;
        c.out_len = static_cast<std::size_t>(n);
        flush(fd);
    }

    // Writes the pending echo; a short write leaves the tail for EPOLLOUT.
    void flush(int fd) {
        connection &c = *table_[static_cast<std::size_t>(fd)];
        while (c.out_off < c.out_len) {
            const auto n =
                ::send(fd, c.buf.data() + c.out_off, c.out_len - c.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<std::size_t>(n);
                st_.bytes_out += static_cast<std::uint64_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                arm(fd, EPOLLOUT);
                return;
            }
            ++st_.errors;
            close_conn(fd);
            return;
        }
        arm(fd, EPOLLIN);
    }

    // Changes the interest set only when it actually changes.
    void arm(int fd, std::uint32_t mask) {
        connection &c = *table_[static_cast<std::size_t>(fd)];
        if (c.armed == mask)
            return;
        epoll_event ev{.events = mask, .data = {.u64 = static_cast<std::uint64_t>(fd)}};
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
        c.armed = mask;
    }

    // Drops a connection; close() also removes it from the epoll set.
    void close_conn(int fd) {
        ::close(fd);
        table_[static_cast<std::size_t>(fd)].reset();
    }

    // Consumes signalfd records: SIGUSR1 reports stats, SIGINT/SIGTERM stops the loop.
    void handle_signal() {
        signalfd_siginfo info{};
        while (::read(signal_fd_, &info, sizeof info) == static_cast<ssize_t>(sizeof info)) {
            if (info.ssi_signo == SIGUSR1) {
                report("stats");
                continue;
            }
            running_ = false;
            return;
        }
    }

    // Prints the counters on one line and flushes, so a piped log still shows them live.
    void report(std::string_view tag) const {
        std::println("[{}] accepted={} bytes_in={} bytes_out={} errors={}", tag, st_.accepted,
                     st_.bytes_in, st_.bytes_out, st_.errors);
        std::fflush(stdout);
    }
};

} // namespace

// Entry point: parse the flags and serve until a signal arrives.
auto main(int argc, char **argv) -> int {
    server{parse(argc, argv)}.run();
    return 0;
}
