#include <gtest/gtest.h>
#include <llco/core/task.hpp>
#include <llco/runtime/engine.decl.hpp>
#include <llco/runtime/scheduler.decl.hpp>
#include <llco/sync/latch.hpp>
#include <llco/sync/sync_wait.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto        POLL            = 1ms;
constexpr auto        CHILD_SLEEP     = 50ms;
constexpr auto        CONCURRENCY_MAX = 150ms;
constexpr std::size_t CHILDREN        = 4;

task<> latch_waiter_steps(latch *l, int *steps) {
    *steps = 1;
    co_await l->wait();
    *steps = 2;
    co_return;
}

task<> thread_waiter(latch *l, std::atomic<std::thread::id> *before,
                     std::atomic<std::thread::id> *after, std::atomic<bool> *ready,
                     std::atomic<bool> *done) {
    before->store(std::this_thread::get_id());
    ready->store(true);
    co_await l->wait();
    after->store(std::this_thread::get_id());
    done->store(true);
    co_return;
}

task<> sleeping_child(latch *done) {
    std::this_thread::sleep_for(CHILD_SLEEP);
    done->count_down();
    co_return;
}

task<> fan_out(latch *done, std::size_t children) {
    for (std::size_t i = 0; i < children; ++i)
        (void)scheduler::submit(sleeping_child(done));
    co_await done->wait();
    co_return;
}

task<> cross_engine_counter(latch *l, std::atomic<bool> *ready) {
    while (!ready->load())
        std::this_thread::yield();
    std::this_thread::sleep_for(1ms); // give the waiter time to publish
    l->count_down();
    l->count_down();
    co_return;
}

} // namespace

TEST(latch, blocks_until_zero) {
    engine e;
    latch  l{2};
    int    steps = 0;

    ASSERT_TRUE(e.spawn(latch_waiter_steps(&l, &steps)));
    ASSERT_TRUE(e.run_once());
    EXPECT_EQ(steps, 1) << "the waiter did not suspend";

    l.count_down();
    EXPECT_EQ(e.drain(), 0U);
    EXPECT_EQ(steps, 1) << "one count_down must not release the waiter";

    l.count_down();
    EXPECT_EQ(steps, 2) << "the last count_down must resume the waiter";
}

TEST(latch, zero_count_is_ready) {
    engine e;
    latch  l{0};
    int    steps = 0;

    ASSERT_TRUE(e.spawn(latch_waiter_steps(&l, &steps)));
    ASSERT_TRUE(e.run_once());
    EXPECT_EQ(steps, 2) << "a zero latch must not suspend";
}

TEST(latch, fan_out_runs_concurrently) {
    scheduler::init(static_cast<unsigned>(CHILDREN));
    latch done{CHILDREN};

    const auto begin = std::chrono::steady_clock::now();
    sync_wait(fan_out(&done, CHILDREN));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    scheduler::stop();

    EXPECT_LT(elapsed, CONCURRENCY_MAX) << "children ran serially instead of concurrently";
}

TEST(latch, cross_engine_countdown) {
    scheduler::init(2);
    latch                        l{2};
    std::atomic<std::thread::id> before{};
    std::atomic<std::thread::id> after{};
    std::atomic<bool>            ready{false};
    std::atomic<bool>            done{false};

    ASSERT_TRUE(scheduler::engine_at(0).spawn(thread_waiter(&l, &before, &after, &ready, &done)));
    ASSERT_TRUE(scheduler::engine_at(1).spawn(cross_engine_counter(&l, &ready)));

    while (!done.load())
        std::this_thread::sleep_for(POLL);
    scheduler::stop();

    EXPECT_EQ(before.load(), after.load()) << "the waiter must resume on its own engine";
    EXPECT_NE(before.load(), std::this_thread::get_id()) << "the waiter never ran on an engine";
}
