// Raw io_uring echo server: the zero-framework control group for bench/echo_bench.cpp.
//
// The op sequence is the one llco runs: a one-shot accept, then per connection one-shot recv
// and one-shot send, one op in flight at a time. No coroutine, no queue, no scheduler and no
// engine: one thread, one ring, one eventfd.
#include <liburing.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t DEFAULT_PORT  = 7000;
constexpr std::size_t   DEFAULT_CHUNK = 4096;
constexpr unsigned      RING_ENTRIES  = 64;

// Command-line configuration; the defaults match echo_bench.
struct config {
    std::uint16_t port  = DEFAULT_PORT;
    std::size_t   chunk = DEFAULT_CHUNK;
};

// Counters reported at exit, in echo_bench's order.
struct statistics {
    std::uint64_t accepted  = 0;
    std::uint64_t live      = 0;
    std::uint64_t bytes_in  = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t errors    = 0;
};

// Aborts with one line: the server cannot run without its ring and sockets.
[[noreturn]]
void fail(std::string_view msg) {
    std::println(stderr, "error: {}", msg);
    (void)std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

// Prints the accepted flags and exits 2, the status a bad argument earns.
[[noreturn]]
void usage() {
    std::println(stderr, "usage: raw_uring_echo [--port N] [--chunk N]");
    std::exit(2);
}

// Parses one flag value as an unsigned integer; a malformed value is a usage error.
auto parse_u64(std::string_view text) -> std::uint64_t {
    std::uint64_t value  = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size())
        usage();
    return value;
}

// Parses the whole command line; every flag takes a value.
auto parse(int argc, char **argv) -> config {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (i + 1 >= argc)
            usage();
        const std::string_view value{argv[++i]};
        if (arg == "--port")
            cfg.port = static_cast<std::uint16_t>(parse_u64(value));
        else if (arg == "--chunk")
            cfg.chunk = parse_u64(value);
        else
            usage();
    }
    return cfg;
}

// Where a connection stands: the op it has in flight, or the one it owes next.
enum class State : std::uint8_t {
    RecvReady,
    RecvArmed,
    SendReady,
    SendArmed
};

// What a completion belongs to; the tag is the io_uring user_data of one submission.
enum class OpKind : std::uint8_t {
    Accept,
    Recv,
    Send
};

struct conn;

// The tag one submission carries: its kind and, for a connection, its owner.
struct op {
    conn  *owner;
    OpKind kind;
};

// One accepted socket, its echo buffer, and the tag of the single op it may have in flight.
struct conn {
    op                pending; // rewritten on every arm
    int               fd      = -1;
    State             st      = State::RecvReady;
    std::size_t       out_off = 0; // bytes of buf already sent
    std::size_t       out_len = 0; // bytes of buf that still owe a send
    std::vector<char> buf;

    // Binds the completion tag to this connection and allocates the echo buffer.
    conn(int socket_fd, std::size_t chunk)
        : pending{.owner = this, .kind = OpKind::Recv}, fd{socket_fd}, buf(chunk) {}
};

// Set by the signal handler; the loop checks it and returns.
volatile std::sig_atomic_t g_stopped  = 0;
int                        g_event_fd = -1;

// Asks the loop to stop and wakes its only blocking read; async-signal-safe.
extern "C" void on_signal(int) {
    g_stopped               = 1;
    const std::uint64_t one = 1;
    (void)::write(g_event_fd, &one, sizeof one);
}

// Single-threaded io_uring echo server: one ring, one eventfd, one op per socket at a time.
class server {
    config                             cfg_;
    statistics                         st_;
    io_uring                           ring_{};
    std::vector<std::unique_ptr<conn>> conns_;
    op                                 accept_op_{.owner = nullptr, .kind = OpKind::Accept};
    int                                listen_fd_    = -1;
    int                                event_fd_     = -1;
    unsigned                           in_flight_    = 0;
    bool                               accept_armed_ = false;

  public:
    // Keeps the configuration; run() opens the ring and the listener.
    explicit server(config cfg) : cfg_{cfg} {}

    // Serves until SIGINT or SIGTERM, then cancels every op and reports the counters.
    void run() {
        open_listener();
        if (::io_uring_queue_init(RING_ENTRIES, &ring_, 0) < 0)
            fail("io_uring_queue_init");
        event_fd_ = ::eventfd(0, EFD_CLOEXEC);
        if (event_fd_ < 0)
            fail("eventfd");
        if (::io_uring_register_eventfd(&ring_, event_fd_) < 0)
            fail("io_uring_register_eventfd");
        g_event_fd = event_fd_;
        install_signals();

        std::println("[listen] 127.0.0.1:{}", cfg_.port);
        (void)std::fflush(stdout);
        loop();
        stop();
    }

  private:
    // Binds 127.0.0.1:port non-blocking and close-on-exec, then starts listening.
    void open_listener() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0)
            fail("socket");
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        const ::sockaddr_in address{.sin_family = AF_INET,
                                    .sin_port   = htons(cfg_.port),
                                    .sin_addr   = {.s_addr = htonl(INADDR_LOOPBACK)},
                                    .sin_zero   = {}};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_in is a sockaddr
        if (::bind(listen_fd_, reinterpret_cast<const ::sockaddr *>(&address), sizeof address) < 0)
            fail("bind");
        if (::listen(listen_fd_, SOMAXCONN) < 0)
            fail("listen");
    }

    // Routes SIGINT and SIGTERM to the eventfd writer; nothing else stops the loop.
    void install_signals() {
        struct sigaction action {};
        action.sa_handler = on_signal;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);
    }

    // Arms every ready op, submits, reaps; only wait() blocks, and only on an empty CQ.
    void loop() {
        while (g_stopped == 0) {
            drain();
            submit();
            if (reap() == 0)
                wait();
        }
    }

    // Arms the pending accept and every connection that owes the ring an op.
    void drain() {
        if (!accept_armed_)
            arm_accept();
        for (const auto &c : conns_)
            arm_ready(*c);
    }

    // Blocks until the kernel posts a completion or a signal writes the same eventfd.
    void wait() const {
        std::uint64_t counter{};
        while (::read(event_fd_, &counter, sizeof counter) < 0 && errno == EINTR) {
        }
    }

    // Hands every queued SQE to the kernel; an empty queue costs no syscall.
    void submit() {
        if (::io_uring_submit(&ring_) < 0)
            fail("io_uring_submit");
    }

    // Takes a submission slot, handing the full batch over first.
    auto next_sqe() -> ::io_uring_sqe * {
        if (auto *sqe = ::io_uring_get_sqe(&ring_); sqe != nullptr)
            return sqe;
        submit();
        return ::io_uring_get_sqe(&ring_);
    }

    // Reaps the completions already queued; the count lets the loop skip a needless block.
    auto reap() -> unsigned {
        ::io_uring_cqe *cqe  = nullptr;
        unsigned        seen = 0;
        while (::io_uring_peek_cqe(&ring_, &cqe) == 0) {
            const int res     = cqe->res;
            auto     *pending = static_cast<op *>(::io_uring_cqe_get_data(cqe));
            ::io_uring_cqe_seen(&ring_, cqe); // before the handler: it may close the connection
            --in_flight_;
            if (pending != nullptr)
                complete(*pending, res);
            ++seen;
        }
        return seen;
    }

    // Routes one completion to its handler; a cancel carries no tag and is only counted above.
    void complete(op tag, int res) {
        switch (tag.kind) {
        case OpKind::Accept:
            on_accept(res);
            break;
        case OpKind::Recv:
            on_recv(*tag.owner, res);
            break;
        case OpKind::Send:
            on_send(*tag.owner, res);
            break;
        }
    }

    // Takes the new socket, or counts a transient accept error; drain() re-arms the accept.
    void on_accept(int res) {
        accept_armed_ = false;
        if (res < 0) {
            if (res != -ECANCELED)
                ++st_.errors;
            return;
        }
        ++st_.accepted;
        ++st_.live;
        conns_.push_back(adopt(res));
    }

    // Echoes what arrived; EOF, a cancel or an error drops the connection.
    void on_recv(conn &c, int res) {
        if (res <= 0) {
            if (res < 0 && res != -ECANCELED)
                ++st_.errors;
            close_conn(c);
            return;
        }
        st_.bytes_in += static_cast<std::uint64_t>(res);
        c.out_off = 0;
        c.out_len = static_cast<std::size_t>(res);
        c.st      = State::SendReady;
    }

    // Accounts for the bytes written, then re-arms the tail or the next recv.
    void on_send(conn &c, int res) {
        if (res < 0) {
            ++st_.errors;
            close_conn(c);
            return;
        }
        c.out_off += static_cast<std::size_t>(res);
        st_.bytes_out += static_cast<std::uint64_t>(res);
        c.st = c.out_off < c.out_len ? State::SendReady : State::RecvReady;
    }

    // Arms the single pending accept; the new socket is picked up in on_accept().
    void arm_accept() {
        auto *sqe = next_sqe();
        ::io_uring_prep_accept(sqe, listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        ::io_uring_sqe_set_data(sqe, &accept_op_);
        accept_armed_ = true;
        ++in_flight_;
    }

    // Arms the op a connection owes next: a recv when idle, the un-sent tail while echoing.
    void arm_ready(conn &c) {
        if (c.st == State::RecvReady)
            arm_recv(c);
        else if (c.st == State::SendReady)
            arm_send(c);
    }

    // Waits for the peer to send more; its buffer lives until the completion is reaped.
    void arm_recv(conn &c) {
        auto *sqe = next_sqe();
        ::io_uring_prep_recv(sqe, c.fd, c.buf.data(), c.buf.size(), 0);
        ::io_uring_sqe_set_data(sqe, &c.pending);
        c.pending.kind = OpKind::Recv;
        c.st           = State::RecvArmed;
        ++in_flight_;
    }

    // Writes the un-sent tail back; a short send re-arms itself.
    void arm_send(conn &c) {
        auto *sqe = next_sqe();
        ::io_uring_prep_send(sqe, c.fd, c.buf.data() + c.out_off, c.out_len - c.out_off,
                             MSG_NOSIGNAL);
        ::io_uring_sqe_set_data(sqe, &c.pending);
        c.pending.kind = OpKind::Send;
        c.st           = State::SendArmed;
        ++in_flight_;
    }

    // Cancels every op the kernel still holds on this fd; the cancel's own completion is noise.
    void arm_cancel(int fd) {
        auto *sqe = next_sqe();
        ::io_uring_prep_cancel_fd(sqe, fd, 0);
        ::io_uring_sqe_set_data(sqe, nullptr);
        ++in_flight_;
    }

    // Takes a freshly accepted socket: no-delay, chunk buffer, one recv armed next.
    auto adopt(int fd) -> std::unique_ptr<conn> {
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        return std::make_unique<conn>(fd, cfg_.chunk);
    }

    // Half-closes then closes the socket, like echo_bench's conn.shutdown() plus its destructor.
    void close_conn(conn &c) {
        (void)::shutdown(c.fd, SHUT_WR);
        ::close(c.fd);
        --st_.live;
        std::erase_if(conns_, [&c](const auto &entry) { return entry.get() == &c; });
    }

    // Cancels the accept and every in-flight echo, drains the kernel, then reports.
    void stop() {
        if (accept_armed_)
            arm_cancel(listen_fd_);
        for (const auto &c : conns_)
            if (c->st == State::RecvArmed || c->st == State::SendArmed)
                arm_cancel(c->fd);
        for (;;) {
            submit();
            if (in_flight_ == 0)
                break;
            if (reap() == 0)
                wait();
        }
        for (const auto &c : conns_) {
            ::close(c->fd);
            --st_.live;
        }
        conns_.clear();
        print_stats();
    }

    // Prints echo_bench's stats line and flushes, so a piped log still shows it.
    void print_stats() const {
        std::println("[stats] accepted={} live={} bytes_in={} bytes_out={} errors={}", st_.accepted,
                     st_.live, st_.bytes_in, st_.bytes_out, st_.errors);
        (void)std::fflush(stdout);
    }
};

} // namespace

// Entry point: parse the flags and serve until a signal arrives.
auto main(int argc, char **argv) -> int {
    server{parse(argc, argv)}.run();
    return 0;
}
