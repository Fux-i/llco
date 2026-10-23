#include <gtest/gtest.h>
#include <llco/runtime/engine.decl.hpp>
#include <llco/io/io_uring.decl.hpp>
#include <llco/core/task.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <liburing.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

// How long a test waits for a completion to show up.
constexpr auto BUDGET        = 1s;
constexpr auto POLL_INTERVAL = 1ms;
constexpr auto SETTLE        = 50ms;

// The deadline lives outside the coroutine: the kernel reads it until the CQE lands.
task<> arm_timeouts(engine *e, __kernel_timespec *deadline, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        auto *sqe = e->ring().sqe();
        ::io_uring_prep_timeout(sqe, deadline, 0, 0);
        ::io_uring_sqe_set_data64(sqe, i);
    }
    e->ring().submit();
    co_return;
}

task<> touch(std::atomic<int> *counter) {
    ++*counter;
    co_return;
}

// The engine thread reaps into an atomic counter, so the test thread can just poll it.
auto wait_for_completions(engine &e, std::size_t expected) -> bool {
    const auto end = std::chrono::steady_clock::now() + BUDGET;
    while (e.ring().completions() < expected && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(POLL_INTERVAL);
    return e.ring().completions() >= expected;
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
