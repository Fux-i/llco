#include <gtest/gtest.h>
#include <llco/runtime/engine.decl.hpp>
#include <llco/core/task.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace llco;

namespace {

task<> touch(std::atomic<int> *counter) {
    ++*counter;
    co_return;
}

} // namespace

TEST(threaded_engine, runs_added_tasks) {
    constexpr int task_count = 100;

    engine e;
    e.start();

    std::atomic<int> done{0};
    for (int i = 0; i < task_count; ++i)
        ASSERT_TRUE(e.spawn(touch(&done)));

    e.stop(); // drains what is already queued
    EXPECT_EQ(done.load(), task_count);
}

TEST(threaded_engine, idle_does_not_spin) {
    constexpr auto idle_time = std::chrono::milliseconds{100};

    engine e;
    e.start();
    std::this_thread::sleep_for(idle_time);
    e.stop();

    EXPECT_EQ(e.loop_count(), 2U) << "an idle loop must sleep between wakeups";
}

TEST(threaded_engine, stop_wakes_idle_thread) {
    constexpr auto settle_time = std::chrono::milliseconds{20};
    constexpr auto budget      = std::chrono::milliseconds{50};

    engine e;
    e.start();
    std::this_thread::sleep_for(settle_time);

    const auto begin = std::chrono::steady_clock::now();
    e.stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_LT(elapsed, budget) << "stop did not wake the sleeping thread";
}

TEST(threaded_engine, destructor_drains_queued_tasks) {
    constexpr int task_count = 20;

    std::atomic<int> done{0};
    {
        engine e;
        e.start();
        for (int i = 0; i < task_count; ++i)
            ASSERT_TRUE(e.spawn(touch(&done)));
    } // ~engine == stop(), which runs what was already queued
    EXPECT_EQ(done.load(), task_count);
}
