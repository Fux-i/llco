#include <gtest/gtest.h>
#include <llco/net/socket.decl.hpp>
#include <llco/runtime/engine.decl.hpp>
#include <llco/io/op.decl.hpp>
#include <llco/io/ring.decl.hpp>
#include <llco/core/task.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

// The io awaiters report kernel errors as values; tests assert on them directly.
template <class T>
using io_result = std::expected<T, std::error_code>;

// How long a test waits for a completion to show up.
constexpr auto BUDGET        = 1s;
constexpr auto POLL_INTERVAL = 1ms;
constexpr auto SETTLE        = 50ms;

// How long the writer thread delays before feeding the pipe that stop() is draining.
constexpr auto WRITER_DELAY = 20ms;

// Raw ops are never dispatched to a coroutine, so they must clear user_data themselves:
// get_sqe() leaves the previous slot's value in place.
task<> arm_timeouts(engine *e, __kernel_timespec *deadline, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        auto *sqe = e->ring().sqe();
        ::io_uring_prep_timeout(sqe, deadline, 0, 0);
        ::io_uring_sqe_set_data(sqe, nullptr);
    }
    e->ring().submit();
    co_return;
}

task<> touch(std::atomic<int> *counter) {
    ++*counter;
    co_return;
}

// Polls until the predicate holds, or the budget runs out.
template <class Predicate>
auto wait_until(Predicate done) -> bool {
    const auto end = std::chrono::steady_clock::now() + BUDGET;
    while (!done() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(POLL_INTERVAL);
    return done();
}

// The engine thread reaps into an atomic counter, so the test thread can just poll it.
auto wait_for_completions(engine &e, std::size_t expected) -> bool {
    return wait_until([&] { return e.ring().completions() >= expected; });
}

// One payload shared by the read and write cases.
constexpr std::string_view PAYLOAD = "hello";

// A bare ring needs no engine: resume() prepares the op, submit() hands it over, reap() completes it.
task<io_result<std::size_t>> read_once(io::ring *r, int fd, std::span<std::byte> buffer) {
    co_return co_await io::read(*r, fd, buffer);
}

task<> read_into(engine *e, int fd, std::span<std::byte> buffer, std::atomic<bool> *done) {
    (void)co_await io::read(e->ring(), fd, buffer);
    done->store(true);
}

task<> write_once(io::ring *r, int fd, std::span<std::byte> buffer) {
    (void)co_await io::write(*r, fd, buffer);
    co_return;
}

task<io_result<std::size_t>> recv_once(io::ring *r, int fd, std::span<std::byte> buffer) {
    co_return co_await io::recv(*r, fd, buffer);
}

task<> send_once(io::ring *r, int fd, std::span<std::byte> buffer) {
    (void)co_await io::send(*r, fd, buffer);
    co_return;
}

task<> send_chars(io::ring *r, int fd, std::string_view payload) {
    (void)co_await io::send(*r, fd, payload);
    co_return;
}

task<io_result<std::size_t>> recv_chars(io::ring *r, int fd, std::span<char> buffer) {
    co_return co_await io::recv(*r, fd, buffer);
}

task<io_result<int>> accept_once(io::ring *r, int listener) {
    co_return co_await io::accept(*r, listener);
}

task<io_result<bool>> cancel_once(io::ring *r, int fd) {
    co_return co_await io::cancel(*r, fd);
}

task<io_result<void>> connect_once(io::ring *r, int fd, ::sockaddr_in address) {
    co_return co_await io::connect(*r, fd, address);
}

// A plain blocking client for the accept case; connect has its own async test below.
auto connect_blocking(std::uint16_t port) -> int {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        throw std::system_error{errno, std::system_category(), "socket"};
    const auto address = net::loopback(port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_in is a sockaddr
    if (::connect(fd, reinterpret_cast<const ::sockaddr *>(&address), sizeof address) != 0)
        throw std::system_error{errno, std::system_category(), "connect"};
    return fd;
}

} // namespace

TEST(io, ring_created_once_per_engine) {
    engine e;
    e.start();
    auto *ring = &e.ring();
    e.stop();
    e.start();
    EXPECT_EQ(&e.ring(), ring) << "start() rebuilt the ring";

    constexpr auto grace    = 50ms;
    auto           deadline = io::to_kernel_time(grace);
    ASSERT_TRUE(e.spawn(arm_timeouts(&e, &deadline, 1)));
    EXPECT_TRUE(wait_for_completions(e, 1)) << "the reused ring never reported a completion";
    e.stop();
    EXPECT_EQ(e.ring().completions(), 1U);
}

TEST(io, nop_completion_wakes_engine) {
    constexpr auto timeout  = 200ms;
    auto           deadline = io::to_kernel_time(timeout);

    engine e;
    e.start();

    const auto begin = std::chrono::steady_clock::now();
    ASSERT_TRUE(e.spawn(arm_timeouts(&e, &deadline, 1)));
    const bool woken   = wait_for_completions(e, 1);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    e.stop();

    EXPECT_TRUE(woken) << "the completion never woke the engine";
    EXPECT_GE(elapsed, timeout / 2) << "the engine reported the completion before it expired";
    EXPECT_EQ(e.ring().completions(), 1U);
}

TEST(io, wake_with_empty_cq_is_harmless) {
    engine           e;
    std::atomic<int> done{0};
    e.start();

    std::this_thread::sleep_for(SETTLE); // idle: blocked on read(eventfd)
    ASSERT_TRUE(e.spawn(touch(&done)));  // a wakeup with an empty CQ
    std::this_thread::sleep_for(SETTLE);
    ASSERT_TRUE(e.spawn(touch(&done))); // and the loop still takes work afterwards
    e.stop();

    EXPECT_EQ(done.load(), 2) << "the engine stopped taking work after an empty wake";
    EXPECT_EQ(e.ring().completions(), 0U)
        << "a completion was counted although no op was submitted";
}

TEST(io, two_completions_one_notification) {
    constexpr auto batch    = 100ms;
    auto           deadline = io::to_kernel_time(batch);

    engine e;
    e.start();

    ASSERT_TRUE(e.spawn(arm_timeouts(&e, &deadline, 2)));
    const bool woken = wait_for_completions(e, 2);
    e.stop();

    EXPECT_TRUE(woken) << "one notification must drain both CQEs";
    EXPECT_EQ(e.ring().completions(), 2U);
}

TEST(io, read_completes_with_bytes) {
    io::ring           r;
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    std::array<std::byte, PAYLOAD.size()> buffer{};

    auto t = read_once(&r, fds[0], buffer);
    t.resume();
    EXPECT_EQ(r.running_io(), 1U) << "preparing did not count the op";
    r.submit();
    ASSERT_EQ(::write(fds[1], PAYLOAD.data(), PAYLOAD.size()),
              static_cast<ssize_t>(PAYLOAD.size()));
    while (!t.handle().done())
        r.reap();

    const auto got = t.result();
    ASSERT_TRUE(got) << got.error().message();
    EXPECT_EQ(*got, PAYLOAD.size());
    EXPECT_EQ(std::memcmp(buffer.data(), PAYLOAD.data(), PAYLOAD.size()), 0) << "wrong bytes";
    EXPECT_EQ(r.running_io(), 0U) << "the op was not uncounted";
}

TEST(io, read_wakes_only_the_right_coroutine) {
    io::ring           r;
    std::array<int, 2> first{};
    std::array<int, 2> second{};
    ASSERT_EQ(::pipe(first.data()), 0);
    ASSERT_EQ(::pipe(second.data()), 0);
    std::array<std::byte, 1> first_buffer{};
    std::array<std::byte, 1> second_buffer{};

    auto a = read_once(&r, first[0], first_buffer);
    auto b = read_once(&r, second[0], second_buffer);
    a.resume();
    b.resume();
    EXPECT_EQ(r.running_io(), 2U);
    r.submit();

    ASSERT_EQ(::write(first[1], "a", 1), 1);
    while (!a.handle().done())
        r.reap();
    EXPECT_FALSE(b.handle().done()) << "the completion woke the wrong coroutine";

    ASSERT_EQ(::write(second[1], "b", 1), 1);
    while (!b.handle().done())
        r.reap();
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, failed_op_propagates_negative_errno) {
    io::ring                 r;
    std::array<int, 2>       fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    std::array<std::byte, 1> buffer{};

    auto t = read_once(&r, fds[1], buffer); // the write end refuses reads
    t.resume();
    r.submit();
    while (!t.handle().done())
        r.reap();

    const auto got = t.result();
    ASSERT_FALSE(got) << "a failed op reported success";
    EXPECT_EQ(got.error().value(), EBADF);
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, sq_full_flushes_and_retries) {
    io::ring r{2}; // two SQEs: preparing a third op must flush the queue first
    std::array<std::array<int, 2>, 3>        fds{};
    std::array<std::array<std::byte, 1>, 3> buffers{};
    for (auto &pair : fds)
        ASSERT_EQ(::pipe(pair.data()), 0);

    auto first  = read_once(&r, fds[0][0], buffers[0]);
    auto second = read_once(&r, fds[1][0], buffers[1]);
    auto third  = read_once(&r, fds[2][0], buffers[2]);
    first.resume();
    second.resume();
    third.resume(); // the queue is full here
    EXPECT_EQ(r.sq_pending(), 1U) << "the full queue was not flushed before taking a new slot";
    EXPECT_EQ(r.running_io(), 3U);
    r.submit();
    for (auto &pair : fds)
        ASSERT_EQ(::write(pair[1], "x", 1), 1);

    while (!first.handle().done())
        r.reap();
    while (!second.handle().done())
        r.reap();
    while (!third.handle().done())
        r.reap();
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, send_writes_to_the_peer) {
    io::ring           r;
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    std::array<char, PAYLOAD.size()> payload{};
    std::ranges::copy(PAYLOAD, payload.begin());

    auto t = send_once(&r, fds[0], std::as_writable_bytes(std::span{payload}));
    t.resume();
    r.submit();
    while (!t.handle().done())
        r.reap();

    std::array<char, PAYLOAD.size()> received{};
    ASSERT_EQ(::read(fds[1], received.data(), received.size()),
              static_cast<ssize_t>(received.size()));
    const std::string_view back{received.data(), received.size()};
    EXPECT_EQ(back, PAYLOAD);
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, recv_send_char_overloads) {
    io::ring           r;
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    std::array<char, PAYLOAD.size()> buffer{};

    auto sent = send_chars(&r, fds[0], PAYLOAD);
    sent.resume();
    r.submit();
    while (!sent.handle().done())
        r.reap();

    auto received = recv_chars(&r, fds[1], buffer);
    received.resume();
    r.submit();
    while (!received.handle().done())
        r.reap();

    const auto got = received.result();
    ASSERT_TRUE(got) << got.error().message();
    EXPECT_EQ(*got, PAYLOAD.size());
    const std::string_view back{buffer.data(), buffer.size()};
    EXPECT_EQ(back, PAYLOAD);
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, recv_returns_zero_at_eof) {
    io::ring           r;
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    std::array<char, PAYLOAD.size()> payload{};
    std::ranges::copy(PAYLOAD, payload.begin());
    ASSERT_EQ(::write(fds[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));

    std::array<char, PAYLOAD.size()> received{};
    auto first = recv_once(&r, fds[0], std::as_writable_bytes(std::span{received}));
    first.resume();
    r.submit();
    while (!first.handle().done())
        r.reap();
    const auto first_got = first.result();
    ASSERT_TRUE(first_got) << first_got.error().message();
    EXPECT_EQ(*first_got, PAYLOAD.size());

    ::close(fds[1]); // the peer goes away: the next recv must report EOF, not an error
    auto eof = recv_once(&r, fds[0], std::as_writable_bytes(std::span{received}));
    eof.resume();
    r.submit();
    while (!eof.handle().done())
        r.reap();
    const auto eof_got = eof.result();
    ASSERT_TRUE(eof_got) << eof_got.error().message();
    EXPECT_EQ(*eof_got, 0U);
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, write_puts_bytes_into_the_pipe) {
    io::ring           r;
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    std::array<char, PAYLOAD.size()> payload{};
    std::ranges::copy(PAYLOAD, payload.begin());

    auto t = write_once(&r, fds[1], std::as_writable_bytes(std::span{payload}));
    t.resume();
    r.submit();
    while (!t.handle().done())
        r.reap();

    std::array<char, PAYLOAD.size()> received{};
    ASSERT_EQ(::read(fds[0], received.data(), received.size()),
              static_cast<ssize_t>(received.size()));
    const std::string_view back{received.data(), received.size()};
    EXPECT_EQ(back, PAYLOAD);
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, stop_waits_for_running_io) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    std::array<std::byte, 1> buffer{};
    std::atomic<bool>        read_done{false};

    engine e;
    e.start();
    ASSERT_TRUE(e.spawn(read_into(&e, fds[0], buffer, &read_done)));
    ASSERT_TRUE(wait_until([&] { return e.ring().running_io() == 1; }))
        << "the read never reached the ring";

    const std::jthread writer{[fd = fds[1]] {
        std::this_thread::sleep_for(WRITER_DELAY);
        const char byte = 'x';
        (void)::write(fd, &byte, 1); // arrives after stop() has begun
    }};
    const auto begin = std::chrono::steady_clock::now();
    e.stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_GE(elapsed, WRITER_DELAY / 2) << "stop() returned before the op completed";
    EXPECT_TRUE(read_done.load()) << "stop() dropped the work it was waiting for";
    EXPECT_EQ(e.ring().running_io(), 0U);
}

TEST(io, accept_returns_a_socket) {
    io::ring  r;
    const int listener = net::listen_tcp(0);

    auto t = accept_once(&r, listener);
    t.resume();
    EXPECT_EQ(r.running_io(), 1U);
    r.submit();
    EXPECT_EQ(r.reap(), 0U) << "the accept completed before a client arrived";

    const int client = connect_blocking(net::local_port(listener));
    while (!t.handle().done())
        r.reap();
    const auto got = t.result();
    ASSERT_TRUE(got) << got.error().message() << " (the accept did not hand back a socket)";
    EXPECT_EQ(r.running_io(), 0U);
    ::close(*got);
    ::close(client);
    ::close(listener);
}

TEST(io, connect_reaches_a_listener) {
    io::ring  r;
    const int listener = net::listen_tcp(0);
    const int fd       = net::tcp_socket();

    auto t = connect_once(&r, fd, net::loopback(net::local_port(listener)));
    t.resume();
    EXPECT_EQ(r.running_io(), 1U);
    r.submit();
    while (!t.handle().done())
        r.reap();

    const auto connected = t.result();
    ASSERT_TRUE(connected) << connected.error().message();
    const int accepted = ::accept(listener, nullptr, nullptr);
    EXPECT_GE(accepted, 0) << "the listener never saw the connection";
    ::close(accepted);
    ::close(fd);
    ::close(listener);
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, cancel_stops_a_pending_accept) {
    io::ring  r;
    const int listener = net::listen_tcp(0);

    auto pending = accept_once(&r, listener);
    pending.resume();
    r.submit(); // the accept is in the kernel now

    auto canceller = cancel_once(&r, listener);
    canceller.resume();
    r.submit();
    while (!pending.handle().done() || !canceller.handle().done())
        r.reap();

    const auto canceled = canceller.result();
    ASSERT_TRUE(canceled) << canceled.error().message();
    EXPECT_TRUE(*canceled) << "the cancel found nothing to cancel";

    const auto got = pending.result();
    ASSERT_FALSE(got) << "the cancelled accept reported success";
    EXPECT_EQ(got.error().value(), ECANCELED);
    EXPECT_EQ(r.running_io(), 0U);
    ::close(listener);
}

TEST(io, engine_dispatches_completion) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    std::array<std::byte, PAYLOAD.size()> buffer{};
    std::atomic<bool>                     read_done{false};

    engine e;
    e.start();
    ASSERT_TRUE(e.spawn(read_into(&e, fds[0], buffer, &read_done)));
    ASSERT_TRUE(wait_until([&] { return e.ring().running_io() == 1; }))
        << "the read never reached the ring";
    ASSERT_EQ(::write(fds[1], PAYLOAD.data(), PAYLOAD.size()),
              static_cast<ssize_t>(PAYLOAD.size()));
    EXPECT_TRUE(wait_until([&] { return read_done.load(); }))
        << "the completion never woke the engine";
    e.stop();
    EXPECT_EQ(e.ring().running_io(), 0U);
}
