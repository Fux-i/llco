#include <gtest/gtest.h>
#include <llco/runtime/engine.decl.hpp>
#include <llco/io/io_uring.decl.hpp>
#include <llco/core/task.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

#include <liburing.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

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

// A bare ring needs no engine: resume() arms the op, submit() hands it over, reap() completes it.
task<std::size_t> read_once(io::ring *r, int fd, std::span<std::byte> buffer) {
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

TEST(io, doorbell_wake_with_empty_cq_is_harmless) {
    engine           e;
    std::atomic<int> done{0};
    e.start();

    std::this_thread::sleep_for(SETTLE); // idle: blocked on read(eventfd)
    ASSERT_TRUE(e.spawn(touch(&done)));  // doorbell rings with an empty CQ
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
    EXPECT_EQ(r.running_io(), 1U) << "arming did not count the op";
    r.submit();
    ASSERT_EQ(::write(fds[1], PAYLOAD.data(), PAYLOAD.size()),
              static_cast<ssize_t>(PAYLOAD.size()));
    while (!t.handle().done())
        r.reap();

    EXPECT_EQ(t.result(), PAYLOAD.size());
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

    auto raised = false;
    try {
        (void)t.result();
    } catch (const std::system_error &error) {
        raised = true;
        EXPECT_EQ(error.code().value(), EBADF);
    }
    EXPECT_TRUE(raised) << "a failed op did not raise";
    EXPECT_EQ(r.running_io(), 0U);
}

TEST(io, sq_full_flushes_and_retries) {
    io::ring r{2}; // two SQEs: arming a third op must flush the queue first
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
