// Bare-TCP echo load generator: numbered fixed-size messages, one epoll per thread, own histogram.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t NS_PER_SECOND   = 1'000'000'000;
constexpr std::size_t   RECV_CAP        = 64 * 1024;   // per-connection receive buffer floor
constexpr std::size_t   BATCH           = 64;          // epoll events per wait
constexpr std::uint64_t FINISH_GRACE_NS = 250'000'000; // cap on the post-run drain
constexpr int           FINISH_POLL_MS  = 10;

// Command-line configuration; the defaults match the echo server's defaults.
struct config {
    std::string_view host    = "127.0.0.1";
    std::uint16_t    port    = 7000;
    std::size_t      conns   = 64;
    std::size_t      depth   = 1;
    std::size_t      size    = 4096;
    std::uint64_t    seconds = 10;
    std::uint64_t    warmup  = 2;
    std::size_t      threads = 1;
    bool             csv     = false;
};

// Aborts with one line: a bad command line or an unreachable server cannot be measured.
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

// Parses the whole command line; every flag takes a value except --csv.
auto parse(int argc, char **argv) -> config {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--csv") {
            cfg.csv = true;
            continue;
        }
        if (i + 1 >= argc)
            fail(std::format("missing value for {}", arg));
        const std::string_view value{argv[++i]};
        if (arg == "--host")
            cfg.host = value;
        else if (arg == "--port")
            cfg.port = static_cast<std::uint16_t>(parse_u64(value, arg));
        else if (arg == "--conns")
            cfg.conns = parse_u64(value, arg);
        else if (arg == "--depth")
            cfg.depth = parse_u64(value, arg);
        else if (arg == "--size")
            cfg.size = parse_u64(value, arg);
        else if (arg == "--seconds")
            cfg.seconds = parse_u64(value, arg);
        else if (arg == "--warmup")
            cfg.warmup = parse_u64(value, arg);
        else if (arg == "--threads")
            cfg.threads = parse_u64(value, arg);
        else
            fail(std::format("unknown flag {}", arg));
    }
    if (cfg.size < 8)
        fail("--size must be at least 8");
    if (cfg.depth == 0)
        fail("--depth must be positive");
    if (cfg.threads == 0 || cfg.conns == 0 || cfg.conns % cfg.threads != 0)
        fail("--conns must be a positive multiple of --threads");
    if (cfg.seconds == 0)
        fail("--seconds must be positive");
    return cfg;
}

// Fixed-bucket microsecond histogram: 1 us buckets below 2 ms, 16 us buckets above.
class histogram {
    static constexpr std::size_t   BUCKETS     = 4096;
    static constexpr std::size_t   FINE        = 2048;
    static constexpr std::uint64_t FINE_NS     = 1'000;
    static constexpr std::uint64_t COARSE_NS   = 16'000;
    static constexpr std::uint64_t FINE_MAX_NS = FINE * FINE_NS;

    std::array<std::uint64_t, BUCKETS> counts_{};
    std::uint64_t                      total_ = 0;

  public:
    // Records one latency sample in nanoseconds; anything past the last bucket lands there.
    void record(std::uint64_t ns) {
        const auto index =
            ns < FINE_MAX_NS ? ns / FINE_NS
                             : FINE + std::min((ns - FINE_MAX_NS) / COARSE_NS, BUCKETS - FINE - 1);
        ++counts_[index];
        ++total_;
    }

    // Adds another histogram's counts, used to merge the per-thread samples.
    void merge(const histogram &other) {
        for (std::size_t i = 0; i < BUCKETS; ++i)
            counts_[i] += other.counts_[i];
        total_ += other.total_;
    }

    // Percentile in microseconds, linearly interpolated inside the bucket that holds the rank.
    auto quantile(double q) const -> double {
        if (total_ == 0)
            return 0.0;
        const auto    rank   = q * static_cast<double>(total_);
        std::uint64_t before = 0;
        for (std::size_t i = 0; i < BUCKETS; ++i) {
            const std::uint64_t count = counts_[i];
            if (before + count > rank) {
                const auto lo   = static_cast<double>(lower_ns(i));
                const auto hi   = static_cast<double>(upper_ns(i));
                const auto frac = (rank - static_cast<double>(before)) / static_cast<double>(count);
                return (lo + (hi - lo) * frac) / 1'000.0;
            }
            before += count;
        }
        return static_cast<double>(upper_ns(BUCKETS - 1)) / 1'000.0;
    }

  private:
    // Lower bound of a bucket in nanoseconds.
    static auto lower_ns(std::size_t index) -> std::uint64_t {
        return index < FINE ? index * FINE_NS : FINE_MAX_NS + (index - FINE) * COARSE_NS;
    }

    // Upper bound of a bucket in nanoseconds.
    static auto upper_ns(std::size_t index) -> std::uint64_t {
        return lower_ns(index) + (index < FINE ? FINE_NS : COARSE_NS);
    }
};

// One socket: a reusable message buffer, a receive buffer, and a ring of send timestamps.
struct connection {
    int                        fd      = -1;
    std::uint32_t              armed   = 0;
    std::size_t                out_off = 0; // bytes of the current message already sent
    std::size_t                in_len  = 0;
    std::uint64_t              sent    = 0; // messages fully handed to the socket
    std::uint64_t              echoed  = 0; // messages fully received back
    std::vector<std::uint64_t> ring;        // send timestamps, indexed by seq % depth
    std::vector<char>          out;         // the next message; the seq is patched in per send
    std::vector<char>          in;          // bytes received but not yet framed
};

// Everything one worker thread contributes to the final report.
struct result {
    std::uint64_t begin_ns = 0; // measurement window start
    std::uint64_t end_ns   = 0; // measurement window end
    std::uint64_t msgs     = 0;
    std::uint64_t errors   = 0;
    std::uint64_t max_ns   = 0;
    histogram     hist;
};

// Monotonic nanosecond stamp, used for both deadlines and latency samples.
auto now_ns() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Resolves the peer once; IPv4 only, matching the echo server.
auto resolve(std::string_view host, std::uint16_t port) -> sockaddr_in {
    const std::string name{host};
    const std::string service{std::to_string(port)};
    addrinfo          hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found   = nullptr;
    if (::getaddrinfo(name.c_str(), service.c_str(), &hints, &found) != 0 || found == nullptr)
        fail(std::format("cannot resolve {}", name));
    sockaddr_in addr{};
    std::memcpy(&addr, found->ai_addr, sizeof addr);
    ::freeaddrinfo(found);
    return addr;
}

// Opens one connected socket with TCP_NODELAY and O_NONBLOCK.
auto connect_to(const sockaddr_in &addr) -> int {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof addr) < 0)
        fail(std::format("connect: {}", std::strerror(errno)));
    const int one   = 1;
    const int flags = ::fcntl(fd, F_GETFL);
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

// User plus system CPU seconds consumed by the whole process so far.
auto cpu_seconds() -> double {
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec) / 1e6 +
           static_cast<double>(usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
}

// One event-loop thread: its own epoll instance, its own sockets, its own histogram.
class worker {
    const config           &cfg_;
    const sockaddr_in      &addr_;
    std::barrier<>         &connected_; // released once every socket is up
    std::barrier<>         &starting_;  // released when the warmup window ends
    std::barrier<>         &measured_;  // released when every measurement window closed
    int                     epoll_fd_  = -1;
    bool                    measuring_ = false;
    std::vector<connection> conns_;
    result                  res_;

  public:
    // Binds one worker to the shared phase barriers and the peer address.
    worker(const config &cfg, const sockaddr_in &addr, std::barrier<> &connected,
           std::barrier<> &starting, std::barrier<> &measured)
        : cfg_{cfg}, addr_{addr}, connected_{connected}, starting_{starting}, measured_{measured} {}

    // Connects, warms up, measures, and returns this thread's samples.
    auto run() -> result {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        conns_.resize(cfg_.conns / cfg_.threads);
        for (auto &c : conns_) {
            c.fd = connect_to(addr_);
            c.ring.resize(cfg_.depth);
            c.out.assign(cfg_.size, 0);
            c.in.resize(std::max(cfg_.size, RECV_CAP));
            epoll_event ev{.events = EPOLLIN, .data = {.ptr = &c}};
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, c.fd, &ev);
            c.armed = EPOLLIN;
            pump(c); // depth messages go out before the first event arrives
        }

        connected_.arrive_and_wait();
        run_until(now_ns() + cfg_.warmup * NS_PER_SECOND, false);
        starting_.arrive_and_wait();
        res_.begin_ns = now_ns();
        run_until(res_.begin_ns + cfg_.seconds * NS_PER_SECOND, true);
        res_.end_ns = now_ns();
        measured_.arrive_and_wait(); // the CPU snapshot must not include the drain
        finish();

        ::close(epoll_fd_);
        return res_;
    }

  private:
    // Drives the epoll loop until the monotonic deadline, recording only while measuring.
    void run_until(std::uint64_t deadline, bool measuring) {
        measuring_ = measuring;
        std::array<epoll_event, BATCH> events{};
        for (;;) {
            const auto now = now_ns();
            if (now >= deadline)
                return;
            const auto timeout =
                std::max<std::int64_t>(1, static_cast<std::int64_t>(deadline - now) / 1'000'000);
            const int ready =
                ::epoll_wait(epoll_fd_, events.data(), BATCH, static_cast<int>(timeout));
            for (int i = 0; i < ready; ++i)
                dispatch(events[i]);
        }
    }

    // Routes one epoll event to its connection.
    void dispatch(const epoll_event &ev) {
        auto &c = *static_cast<connection *>(ev.data.ptr);
        if (ev.events & (EPOLLERR | EPOLLHUP)) {
            close_conn(c);
            return;
        }
        if (ev.events & EPOLLOUT)
            pump(c);
        if (c.fd >= 0 && (ev.events & EPOLLIN))
            drain(c);
    }

    // Sends until depth messages are in flight or the socket blocks.
    void pump(connection &c) {
        while (c.sent - c.echoed < cfg_.depth) {
            if (c.out_off == 0) {
                const std::uint64_t seq = c.sent + 1;
                std::memcpy(c.out.data(), &seq, sizeof seq);
                c.ring[seq % cfg_.depth] = now_ns();
            }
            const auto n =
                ::send(c.fd, c.out.data() + c.out_off, cfg_.size - c.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<std::size_t>(n);
                if (c.out_off == cfg_.size) {
                    c.out_off = 0;
                    ++c.sent;
                }
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                arm(c, EPOLLIN | EPOLLOUT);
                return;
            }
            close_conn(c);
            return;
        }
        if (c.out_off == 0)
            arm(c, EPOLLIN);
    }

    // Reads what is available, frames whole messages, then resumes sending.
    void drain(connection &c) {
        for (;;) {
            const auto n = ::recv(c.fd, c.in.data() + c.in_len, c.in.size() - c.in_len, 0);
            if (n == 0) {
                close_conn(c);
                return;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                close_conn(c);
                return;
            }
            c.in_len += static_cast<std::size_t>(n);
            frame(c);
        }
        pump(c);
    }

    // Cuts the receive buffer into fixed-size messages, then slides the tail down.
    void frame(connection &c) {
        std::size_t off = 0;
        while (c.in_len - off >= cfg_.size) {
            on_message(c, c.in.data() + off);
            off += cfg_.size;
        }
        if (off > 0) {
            std::memmove(c.in.data(), c.in.data() + off, c.in_len - off);
            c.in_len -= off;
        }
    }

    // Verifies one echo byte-for-byte and, while measuring, records its round trip.
    void on_message(connection &c, const char *msg) {
        std::uint64_t               seq      = 0;
        const std::uint64_t         expected = c.echoed + 1;
        const std::span<const char> body{msg + 8, cfg_.size - 8};
        std::memcpy(&seq, msg, sizeof seq);
        if (seq != expected || !std::ranges::all_of(body, [](char b) { return b == 0; })) {
            if (res_.errors++ == 0)
                std::println(stderr, "echo mismatch: expected seq {}", expected);
        } else if (measuring_) {
            const auto latency = now_ns() - c.ring[expected % cfg_.depth];
            res_.hist.record(latency);
            res_.max_ns = std::max(res_.max_ns, latency);
            ++res_.msgs;
        }
        ++c.echoed;
    }

    // Changes the epoll interest set only when it actually changes.
    void arm(connection &c, std::uint32_t mask) {
        if (c.armed == mask)
            return;
        epoll_event ev{.events = mask, .data = {.ptr = &c}};
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, c.fd, &ev);
        c.armed = mask;
    }

    // Half-closes every socket and swallows the echoes still in flight, so the server sees EOF.
    void finish() {
        std::size_t alive = 0;
        for (auto &c : conns_)
            if (c.fd >= 0) {
                ::shutdown(c.fd, SHUT_WR);
                arm(c, EPOLLIN);
                ++alive;
            }
        std::array<epoll_event, BATCH> events{};
        const auto                     deadline = now_ns() + FINISH_GRACE_NS;
        while (alive > 0 && now_ns() < deadline) {
            const int ready = ::epoll_wait(epoll_fd_, events.data(), BATCH, FINISH_POLL_MS);
            for (int i = 0; i < ready; ++i) {
                auto &c = *static_cast<connection *>(events[i].data.ptr);
                if (c.fd < 0 || !at_eof(c))
                    continue;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, c.fd, nullptr);
                ::close(c.fd);
                c.fd = -1;
                --alive;
            }
        }
        for (auto &c : conns_)
            if (c.fd >= 0)
                ::close(c.fd);
    }

    // Reads and discards whatever the server still owes; true once the socket reached EOF.
    auto at_eof(connection &c) -> bool {
        for (;;) {
            const auto n = ::recv(c.fd, c.in.data(), c.in.size(), 0);
            if (n > 0)
                continue;
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return false;
            if (n < 0)
                ++res_.errors; // a real reset, unlike the clean EOF this half-close asked for
            return true;
        }
    }

    // Drops a dead connection; its outstanding echoes will never arrive.
    void close_conn(connection &c) {
        ++res_.errors;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, c.fd, nullptr);
        ::close(c.fd);
        c.fd = -1;
    }
};

} // namespace

// Entry point: parse the flags, run the load, and print the report.
auto main(int argc, char **argv) -> int {
    const auto cfg  = parse(argc, argv);
    const auto addr = resolve(cfg.host, cfg.port);

    const auto                barrier_count = static_cast<std::ptrdiff_t>(cfg.threads) + 1;
    std::barrier<>            connected{barrier_count};
    std::barrier<>            starting{barrier_count};
    std::barrier<>            measured{barrier_count};
    std::vector<result>       results(cfg.threads);
    std::vector<std::jthread> threads;
    threads.reserve(cfg.threads);
    for (std::size_t i = 0; i < cfg.threads; ++i)
        threads.emplace_back(
            [&, i] { results[i] = worker{cfg, addr, connected, starting, measured}.run(); });

    connected.arrive_and_wait(); // every socket is connected
    starting.arrive_and_wait();  // the warmup window is over

    const auto cpu_before = cpu_seconds();
    measured.arrive_and_wait(); // every measurement window is closed
    const auto cpu_after = cpu_seconds();
    for (auto &thread : threads)
        thread.join();

    auto   begin = results.front().begin_ns;
    auto   end   = results.front().end_ns;
    result total;
    for (const auto &r : results) {
        begin = std::min(begin, r.begin_ns);
        end   = std::max(end, r.end_ns);
        total.msgs += r.msgs;
        total.errors += r.errors;
        total.max_ns = std::max(total.max_ns, r.max_ns);
        total.hist.merge(r.hist);
    }

    const auto wall = static_cast<double>(end - begin) / static_cast<double>(NS_PER_SECOND);
    const auto rps  = static_cast<double>(total.msgs) / wall;
    const auto cpu_pct =
        (cpu_after - cpu_before) / (wall * static_cast<double>(cfg.threads)) * 100.0;
    const auto p50    = total.hist.quantile(0.50);
    const auto p99    = total.hist.quantile(0.99);
    const auto p999   = total.hist.quantile(0.999);
    const auto max_us = static_cast<double>(total.max_ns) / 1'000.0;

    if (!cfg.csv) {
        std::println("threads={} conns={} depth={} size={}", cfg.threads, cfg.conns, cfg.depth,
                     cfg.size);
        std::println("msgs={} wall={:.3f}s rps={:.0f}", total.msgs, wall, rps);
        std::println("p50={:.2f}us p99={:.2f}us p999={:.2f}us max={:.2f}us", p50, p99, p999,
                     max_us);
        std::println("client_cpu={:.1f}% errors={}", cpu_pct, total.errors);
    }
    std::println("{},{},{},{},{},{:.1f},{:.2f},{:.2f},{:.2f},{:.2f},{:.1f},{}", cfg.threads,
                 cfg.conns, cfg.depth, cfg.size, total.msgs, rps, p50, p99, p999, max_us, cpu_pct,
                 total.errors);
    return 0;
}
