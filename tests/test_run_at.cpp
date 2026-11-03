#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/core/task.hpp>
#include <llco/runtime/engine.decl.hpp>
#include <llco/runtime/run.decl.hpp>
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

task<> record_engine(std::atomic<engine *> *seen, std::atomic<bool> *done) {
    seen->store(current_engine());
    done->store(true);
    co_return;
}

// Pins an engine thread inside the loop until released, so its queue cannot drain.
task<> hold(std::atomic<unsigned> *started, std::atomic<bool> *released) {
    started->fetch_add(1);
    while (!released->load())
        std::this_thread::yield();
    co_return;
}

// Waits until the pinned engine thread is spinning inside hold().
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

TEST(run_at, pins_the_task_to_that_engine) {
    scheduler::init(4);
    std::atomic<engine *> seen{nullptr};
    std::atomic<bool>     done{false};

    run_at(2, record_engine(&seen, &done));
    scheduler::stop(); // each engine drains what it was given

    EXPECT_TRUE(done.load()) << "the task never ran";
    EXPECT_EQ(seen.load(), &scheduler::engine_at(2)) << "the task did not land on engine 2";
}

TEST(run_at, throws_when_the_pinned_engine_is_full) {
    scheduler::init(2);
    std::atomic<unsigned>                started{0};
    std::atomic<bool>                    released{false};
    std::atomic<int>                     done{0};
    std::vector<std::coroutine_handle<>> frames;

    ASSERT_TRUE(scheduler::engine_at(0).spawn(hold(&started, &released)));
    wait_for_holds(&started, 1);
    ASSERT_TRUE(fill(scheduler::engine_at(0), frames)) << "engine 0 did not accept a full queue";

    EXPECT_THROW(run_at(0, touch(&done)), std::runtime_error)
        << "pin must not fall back to another engine";
    ASSERT_TRUE(scheduler::submit(touch(&done))) << "round robin still has engine 1 to fall back on";

    released.store(true);
    scheduler::stop();
    EXPECT_EQ(done.load(), 1) << "only the round-robin task should have run";

    for (auto frame : frames)
        frame.destroy();
}

TEST(run_at, throws_after_stop) {
    scheduler::init(1);
    scheduler::stop();

    EXPECT_THROW(run_at(0, noop()), std::runtime_error)
        << "a stopped scheduler must not queue work nobody will run";
}

TEST(run_at, accepts_a_callable_and_a_range) {
    scheduler::init(2);
    std::atomic<int> done{0};

    run_at(1, [&done]() -> task<> { // NOLINT(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        ++done;
        co_return;
    });

    std::vector<task<>> batch;
    batch.push_back(touch(&done));
    sync_run_at(1, std::move(batch));

    EXPECT_EQ(done.load(), 2) << "the callable or the range overload dropped a task";
    scheduler::stop();
}

TEST(sync_run_at, waits_on_the_pinned_engine) {
    scheduler::init(2);
    std::atomic<engine *> seen{nullptr};
    std::atomic<bool>     done{false};

    sync_run_at(1, record_engine(&seen, &done));

    EXPECT_TRUE(done.load()) << "sync_run_at returned before the task finished";
    EXPECT_EQ(seen.load(), &scheduler::engine_at(1)) << "the task did not land on engine 1";
    scheduler::stop();
}
