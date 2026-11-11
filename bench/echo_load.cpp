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
constexpr double        NS_PER_US       = 1'000.0;                // nanoseconds per microsecond
constexpr double        US_PER_SECOND   = 1'000'000.0;            // microseconds per second
constexpr std::size_t   SEQ_BYTES       = sizeof(std::uint64_t);  // message header: the sequence
constexpr std::size_t   RECV_CAP        = std::size_t{64} * 1024; // per-connection receive floor
constexpr std::size_t   BATCH           = 64;                     // epoll events per wait
constexpr std::uint64_t FINISH_GRACE_NS = 250'000'000;            // cap on the post-run drain
constexpr int           FINISH_POLL_MS  = 10;

// Command-line configuration; the defaults match the echo server's defaults.
struct config {
    std::string_view      host    = "127.0.0.1";
    std::uint16_t         port    = 7000;
    std::size_t           conns   = 64;
    std::size_t           depth   = 1;
    std::size_t           size    = 4096;
    std::uint64_t         seconds = 10;
    std::uint64_t         warmup  = 2;
    std::size_t           threads = 1;
    std::vector<unsigned> cpus; // worker i pins to cpus[i % size]; empty leaves them unpinned
    bool                  csv = false;
};

// Aborts with one line: a bad command line or an unreachable server cannot be measured.
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

// Parses the whole command line; every flag takes a value except --csv.
auto parse(int argc, char **argv) -> config {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
        const std::string_view arg{argv[i]};
        if (arg == "--csv") {
            cfg.csv = true;
            continue;
        }
        if (i + 1 >= argc)
            fail(std::format("missing value for {}", arg));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
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
        else if (arg == "--cpus")
            cfg.cpus = parse_cpus(value);
        else
            fail(std::format("unknown flag {}", arg));
    }
    if (cfg.size < SEQ_BYTES)
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

    std::array<std::uint64_t, BUCKETS> m_counts{};
    std::uint64_t                      m_total = 0;

  public:
    // Records one latency sample in nanoseconds; anything past the last bucket lands there.
    void record(std::uint64_t ns) {
        const auto index =
            ns < FINE_MAX_NS ? ns / FINE_NS
                             : FINE + std::min((ns - FINE_MAX_NS) / COARSE_NS, BUCKETS - FINE - 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): index is clipped
        ++m_counts[index];
        ++m_total;
    }

    // Adds another histogram's counts, used to merge the per-thread samples.
    void merge(const histogram &other) {
        for (std::size_t i = 0; i < BUCKETS; ++i)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): index is bounded
            m_counts[i] += other.m_counts[i];
        m_total += other.m_total;
    }

    // Percentile in microseconds, linearly interpolated inside the bucket that holds the rank.
    auto quantile(double q) const -> double {
        if (m_total == 0)
            return 0.0;
        const auto    rank   = q * static_cast<double>(m_total);
        std::uint64_t before = 0;
        for (std::size_t i = 0; i < BUCKETS; ++i) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): index is bounded
            const std::uint64_t count = m_counts[i];
            if (static_cast<double>(before + count) > rank) {
                const auto lo   = static_cast<double>(lower_ns(i));
                const auto hi   = static_cast<double>(upper_ns(i));
                const auto frac = (rank - static_cast<double>(before)) / static_cast<double>(count);
                return (lo + ((hi - lo) * frac)) / NS_PER_US;
            }
            before += count;
        }
        return static_cast<double>(upper_ns(BUCKETS - 1)) / NS_PER_US;
    }

  private:
    // Lower bound of a bucket in nanoseconds.
    static auto lower_ns(std::size_t index) -> std::uint64_t {
        return index < FINE ? index * FINE_NS : FINE_MAX_NS + ((index - FINE) * COARSE_NS);
    }

    // Upper bound of a bucket in nanoseconds.
    static auto upper_ns(std::size_t index) -> std::uint64_t {
        return lower_ns(index) + (index < FINE ? FINE_NS : COARSE_NS);
    }
};

// One socket: a reusable message buffer, a receive buffer, and a ring of send timestamps.
struct connection {
    int                        fd      = -1;
    std::uint32_t              events  = 0;
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
    if (fd < 0)
        fail(std::format("socket: {}", std::strerror(errno)));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_in is a sockaddr
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof addr) < 0)
        fail(std::format("connect: {}", std::strerror(errno)));
    const int one   = 1;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no typed wrapper
    const int flags = ::fcntl(fd, F_GETFL);
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no typed wrapper
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

// User plus system CPU seconds consumed by the whole process so far.
auto cpu_seconds() -> double {
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    const auto user = static_cast<double>(usage.ru_utime.tv_sec) +
                      (static_cast<double>(usage.ru_utime.tv_usec) / US_PER_SECOND);
    const auto sys = static_cast<double>(usage.ru_stime.tv_sec) +
                     (static_cast<double>(usage.ru_stime.tv_usec) / US_PER_SECOND);
    return user + sys;
}

// One event-loop thread: its own epoll instance, its own sockets, its own histogram.
class worker {
    const config           &m_cfg;
    const sockaddr_in      &m_addr;
    std::barrier<>         &m_connected; // released once every socket is up
    std::barrier<>         &m_starting;  // released when the warmup window ends
    std::barrier<>         &m_measured;  // released when every measurement window closed
    int                     m_epoll_fd  = -1;
    bool                    m_measuring = false;
    std::vector<connection> m_conns;
    result                  m_res;

  public:
    // Binds one worker to the shared phase barriers and the peer address.
    worker(const config &cfg, const sockaddr_in &addr, std::barrier<> &connected,
           std::barrier<> &starting, std::barrier<> &measured)
        : m_cfg{cfg}, m_addr{addr}, m_connected{connected}, m_starting{starting},
          m_measured{measured} {}

    // Connects, warms up, measures, and returns this thread's samples. The caller pins the thread
    // before it gets here, because a cpu the kernel rejects would otherwise be silently ignored.
    auto run() -> result {
        m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        m_conns.resize(m_cfg.conns / m_cfg.threads);
        for (auto &c : m_conns) {
            c.fd = connect_to(m_addr);
            c.ring.resize(m_cfg.depth);
            c.out.assign(m_cfg.size, 0);
            c.in.resize(std::max(m_cfg.size, RECV_CAP));
            epoll_event ev{.events = EPOLLIN, .data = {.ptr = &c}};
            ::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, c.fd, &ev);
            c.events = EPOLLIN;
            pump(c); // depth messages go out before the first event arrives
        }

        m_connected.arrive_and_wait();
        run_until(now_ns() + (m_cfg.warmup * NS_PER_SECOND), false);
        m_starting.arrive_and_wait();
        m_res.begin_ns = now_ns();
        run_until(m_res.begin_ns + (m_cfg.seconds * NS_PER_SECOND), true);
        m_res.end_ns = now_ns();
        m_measured.arrive_and_wait(); // the CPU snapshot must not include the drain
        finish();

        ::close(m_epoll_fd);
        return m_res;
    }

  private:
    // Drives the epoll loop until the monotonic deadline, recording only while measuring.
    void run_until(std::uint64_t deadline, bool measuring) {
        m_measuring = measuring;
        std::array<epoll_event, BATCH> events{};
        for (;;) {
            const auto now = now_ns();
            if (now >= deadline)
                return;
            const auto timeout =
                std::max<std::int64_t>(1, static_cast<std::int64_t>(deadline - now) / 1'000'000);
            const int ready =
                ::epoll_wait(m_epoll_fd, events.data(), BATCH, static_cast<int>(timeout));
            const auto ready_events =
                std::span{events}.first(static_cast<std::size_t>(std::max(ready, 0)));
            for (const epoll_event &ev : ready_events)
                dispatch(ev);
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
        while (c.sent - c.echoed < m_cfg.depth) {
            if (c.out_off == 0) {
                const std::uint64_t seq = c.sent + 1;
                std::memcpy(c.out.data(), &seq, sizeof seq);
                c.ring[seq % m_cfg.depth] = now_ns();
            }
            const auto n =
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): inside out_buf
                ::send(c.fd, c.out.data() + c.out_off, m_cfg.size - c.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<std::size_t>(n);
                if (c.out_off == m_cfg.size) {
                    c.out_off = 0;
                    ++c.sent;
                }
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                watch(c, EPOLLIN | EPOLLOUT);
                return;
            }
            close_conn(c);
            return;
        }
        if (c.out_off == 0)
            watch(c, EPOLLIN);
    }

    // Reads what is available, frames whole messages, then resumes sending.
    void drain(connection &c) {
        for (;;) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): inside in_buf
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
        while (c.in_len - off >= m_cfg.size) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): inside in_buf
            on_message(c, c.in.data() + off);
            off += m_cfg.size;
        }
        if (off > 0) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): inside in_buf
            std::memmove(c.in.data(), c.in.data() + off, c.in_len - off);
            c.in_len -= off;
        }
    }

    // Verifies one echo byte-for-byte and, while measuring, records its round trip.
    void on_message(connection &c, const char *msg) {
        std::uint64_t               seq      = 0;
        const std::uint64_t         expected = c.echoed + 1;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): past the seq header
        const std::span<const char> body{msg + SEQ_BYTES, m_cfg.size - SEQ_BYTES};
        std::memcpy(&seq, msg, sizeof seq);
        if (seq != expected || !std::ranges::all_of(body, [](char b) { return b == 0; })) {
            if (m_res.errors++ == 0)
                std::println(stderr, "echo mismatch: expected seq {}", expected);
        } else if (m_measuring) {
            const auto latency = now_ns() - c.ring[expected % m_cfg.depth];
            m_res.hist.record(latency);
            m_res.max_ns = std::max(m_res.max_ns, latency);
            ++m_res.msgs;
        }
        ++c.echoed;
    }

    // Changes the epoll interest set only when it actually changes.
    void watch(connection &c, std::uint32_t events) const {
        if (c.events == events)
            return;
        epoll_event ev{.events = events, .data = {.ptr = &c}};
        ::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, c.fd, &ev);
        c.events = events;
    }

    // Half-closes every socket and swallows the echoes still in flight, so the server sees EOF.
    void finish() {
        std::size_t alive = 0;
        for (auto &c : m_conns)
            if (c.fd >= 0) {
                ::shutdown(c.fd, SHUT_WR);
                watch(c, EPOLLIN);
                ++alive;
            }
        std::array<epoll_event, BATCH> events{};
        const auto                     deadline = now_ns() + FINISH_GRACE_NS;
        while (alive > 0 && now_ns() < deadline) {
            const int  ready = ::epoll_wait(m_epoll_fd, events.data(), BATCH, FINISH_POLL_MS);
            const auto ready_events =
                std::span{events}.first(static_cast<std::size_t>(std::max(ready, 0)));
            for (const epoll_event &ev : ready_events) {
                auto &c = *static_cast<connection *>(ev.data.ptr);
                if (c.fd < 0 || !at_eof(c))
                    continue;
                ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, c.fd, nullptr);
                ::close(c.fd);
                c.fd = -1;
                --alive;
            }
        }
        for (auto &c : m_conns)
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
                ++m_res.errors; // a real reset, unlike the clean EOF this half-close asked for
            return true;
        }
    }

    // Drops a dead connection; its outstanding echoes will never arrive.
    void close_conn(connection &c) {
        ++m_res.errors;
        ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, c.fd, nullptr);
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
        threads.emplace_back([&, i] {
            // Each worker gets its own core: isolated cores get no load balancing, so unpinned
            // workers would all land on one core and the client would cap itself.
            if (!cfg.cpus.empty()) {
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(cfg.cpus.at(i % cfg.cpus.size()), &set);
                [[maybe_unused]]
                auto rc = ::pthread_setaffinity_np(::pthread_self(), sizeof set, &set);
            }
            results[i] = worker{cfg, addr, connected, starting, measured}.run();
        });

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
