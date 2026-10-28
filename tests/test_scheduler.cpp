#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/core/task.hpp>
#include <llco/runtime/run.hpp>
#include <llco/runtime/scheduler.decl.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto POLL = 1ms;

task<> noop() {
    co_return;
}

task<> touch(std::atomic<int> *done) {
    ++*done;
    co_return;
}

// Pins an engine thread inside the loop until released, so its queue cannot drain.
task<> hold(std::atomic<unsigned> *started, std::atomic<bool> *released) {
    started->fetch_add(1);
    while (!released->load())
        std::this_thread::yield();
    co_return;
}

// Waits until every pinned engine thread is spinning inside hold().
void wait_for_holds(std::atomic<unsigned> *started, unsigned count) {
    while (started->load() < count)
        std::this_thread::sleep_for(POLL);
}

// Fills one engine to capacity; the caller owns the frames.
auto fill(engine &e, std::vector<std::coroutine_handle<>> &frames) -> bool {
    frames.reserve(QUEUE_CAPACITY);
    for (unsigned i = 0; i < QUEUE_CAPACITY; ++i) {
        frames.push_back(noop().release());
        if (!e.try_add(frames.back()))
            return false;
    }
    return true;
}

} // namespace

TEST(scheduler, round_robin_distributes) {
    constexpr unsigned count = 4;
    constexpr unsigned batch = 8;

    scheduler::init(count);
    std::atomic<unsigned> started{0};
    std::atomic<bool>     released{false};
    std::atomic<int>      done{0};

    for (unsigned i = 0; i < count; ++i)
        ASSERT_TRUE(scheduler::engine_at(i).spawn(hold(&started, &released)));
    wait_for_holds(&started, count);

    for (unsigned i = 0; i < batch; ++i)
        ASSERT_TRUE(scheduler::submit(touch(&done)));

    for (unsigned i = 0; i < count; ++i)
        EXPECT_EQ(scheduler::engine_at(i).load(), batch / count)
            << "engine " << i << " got the wrong share";

    released.store(true);
    scheduler::stop();
    EXPECT_EQ(done.load(), static_cast<int>(batch));
}

TEST(scheduler, run_submits_every_task) {
    scheduler::init(2);
    std::atomic<int> done{0};

    llco::run(touch(&done), touch(&done));
    scheduler::stop(); // each engine drains what it was given
    EXPECT_EQ(done.load(), 2) << "run() did not submit every task";
}

TEST(scheduler, run_throws_when_full) {
    scheduler::init(1);
    std::atomic<unsigned>                started{0};
    std::atomic<bool>                    released{false};
    std::atomic<int>                     done{0};
    std::vector<std::coroutine_handle<>> frames;

    ASSERT_TRUE(scheduler::engine_at(0).spawn(hold(&started, &released)));
    wait_for_holds(&started, 1);
    ASSERT_TRUE(fill(scheduler::engine_at(0), frames));

    EXPECT_THROW(llco::run(touch(&done)), std::runtime_error)
        << "a full scheduler must not swallow the task";

    released.store(true);
    scheduler::stop();
    EXPECT_EQ(done.load(), 0);

    for (auto frame : frames)
        frame.destroy();
}

TEST(scheduler, retries_on_full_engine) {
    scheduler::init(2);
    std::atomic<unsigned>                started{0};
    std::atomic<bool>                    released{false};
    std::atomic<int>                     done{0};
    std::vector<std::coroutine_handle<>> frames;

    for (unsigned i = 0; i < 2; ++i)
        ASSERT_TRUE(scheduler::engine_at(i).spawn(hold(&started, &released)));
    wait_for_holds(&started, 2);

    ASSERT_TRUE(fill(scheduler::engine_at(0), frames)) << "engine 0 did not accept a full queue";

    // Round robin starts at engine 0, which is full: the frame must move on to engine 1.
    ASSERT_TRUE(scheduler::submit(touch(&done)));
    EXPECT_EQ(scheduler::engine_at(0).load(), QUEUE_CAPACITY) << "the full engine kept the frame";
    EXPECT_EQ(scheduler::engine_at(1).load(), 1U) << "the frame did not reach the second engine";

    released.store(true);
    scheduler::stop();
    EXPECT_EQ(done.load(), 1);

    for (auto frame : frames)
        frame.destroy();
}

TEST(scheduler, submit_returns_false_when_all_full) {
    scheduler::init(1);
    std::atomic<unsigned>                started{0};
    std::atomic<bool>                    released{false};
    std::atomic<int>                     done{0};
    std::vector<std::coroutine_handle<>> frames;

    ASSERT_TRUE(scheduler::engine_at(0).spawn(hold(&started, &released)));
    wait_for_holds(&started, 1);
    ASSERT_TRUE(fill(scheduler::engine_at(0), frames));

    EXPECT_FALSE(scheduler::submit(touch(&done))) << "a full scheduler must reject the task";

    released.store(true);
    scheduler::stop();
    EXPECT_EQ(done.load(), 0);

    for (auto frame : frames)
        frame.destroy();
}

TEST(scheduler, submit_is_rejected_after_stop) {
    scheduler::init(1);
    scheduler::stop();

    EXPECT_FALSE(scheduler::submit(noop())) << "a stopped scheduler must not accept work";
}

TEST(scheduler, init_rebuilds_the_pool) {
    std::atomic<int> done{0};

    scheduler::init(2);
    ASSERT_TRUE(scheduler::submit(touch(&done)));
    scheduler::stop();
    EXPECT_EQ(done.load(), 1) << "stop() must drain what was queued";

    scheduler::init(2); // the same count still means a fresh pool
    ASSERT_EQ(scheduler::size(), 2U);
    for (unsigned i = 0; i < scheduler::size(); ++i)
        EXPECT_EQ(scheduler::engine_at(i).load(), 0U) << "engine " << i << " kept old work";
}

TEST(scheduler, sync_run_waits_for_every_task) {
    std::atomic<int> done{0};
    scheduler::init(2);

    sync_run(touch(&done), touch(&done), touch(&done));

    EXPECT_EQ(done.load(), 3) << "sync_run returned before the tasks finished";
    scheduler::stop();
}

TEST(scheduler, run_accepts_a_callable) {
    std::atomic<int> done{0};
    scheduler::init(1);

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines): run() stores the closure in a frame
    llco::run([&done]() -> task<> {
        ++done;
        co_return;
    });
    scheduler::stop();

    EXPECT_EQ(done.load(), 1) << "run() never invoked the callable";
}