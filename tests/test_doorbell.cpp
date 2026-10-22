#include <gtest/gtest.h>
#include <llco/engine.decl.hpp>
#include <llco/task.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto SETTLE        = 50ms; // let the loop fall asleep before measuring
constexpr auto POLL_INTERVAL = 1ms;

task<> touch(std::atomic<long> *counter) {
    ++*counter;
    co_return;
}

// Keeps the engine thread busy so later pushes clearly arrive while it is awake.
task<> hold(std::atomic<bool> *started, std::atomic<long> *done, std::chrono::milliseconds span) {
    started->store(true);
    std::this_thread::sleep_for(span);
    ++*done;
    co_return;
}

} // namespace

TEST(doorbell, a_busy_engine_is_not_rung) {
    constexpr auto hold_span = 500ms;
    constexpr int  extra     = 50;

    engine            e;
    std::atomic<bool> started{false};
    std::atomic<long> done{0};
    e.start();
    std::this_thread::sleep_for(SETTLE); // measure from the sleeping state

    ASSERT_TRUE(e.spawn(hold(&started, &done, hold_span))); // the engine is asleep: one bell
    while (!started.load())
        std::this_thread::sleep_for(POLL_INTERVAL);
    EXPECT_EQ(e.wakeup_count(), 1U) << "the push that woke an idle engine must ring";

    for (int i = 0; i < extra; ++i)
        ASSERT_TRUE(e.spawn(touch(&done)));
    EXPECT_EQ(e.wakeup_count(), 1U) << "an engine that is running must not be rung";

    e.stop();
    EXPECT_EQ(done.load(), long{extra} + 1);
}

TEST(doorbell, no_lost_wakeup_under_churn) {
    constexpr int  producers    = 4;
    constexpr long per_producer = 25'000;

    engine            e;
    std::atomic<long> accepted{0};
    std::atomic<long> done{0};
    e.start();
    std::this_thread::sleep_for(SETTLE); // start from the sleeping state, not a racing one

    std::vector<std::jthread> workers;
    workers.reserve(producers);
    for (int p = 0; p < producers; ++p) {
        workers.emplace_back([&] {
            for (long i = 0; i < per_producer; ++i) {
                while (!e.spawn(touch(&done))) {
                } // a rejected frame is already destroyed, so build a new one
                ++accepted;
            }
        });
    }
    workers.clear(); // joins every producer before the engine stops
    e.stop();

    EXPECT_EQ(done.load(), accepted.load()) << "a doorbell was lost under churn";
    // The gate turns 100k pushes into a bell only when the loop actually sleeps: measured 1 on an
    // idle machine and ~2.3k while the host was running four builds, so 10% is a safe ceiling.
    EXPECT_LT(e.wakeup_count(), static_cast<std::size_t>(accepted.load()) / 10)
        << "pushes kept ringing the bell; the sleeping check is not working";
}