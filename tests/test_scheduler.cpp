#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/runtime/scheduler.decl.hpp>
#include <llco/core/task.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
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

    scheduler             s{count};
    std::atomic<unsigned> started{0};
    std::atomic<bool>     released{false};
    std::atomic<int>      done{0};

    for (unsigned i = 0; i < count; ++i)
        ASSERT_TRUE(s.engine_at(i).spawn(hold(&started, &released)));
    wait_for_holds(&started, count);

    for (unsigned i = 0; i < batch; ++i)
        ASSERT_TRUE(s.submit(touch(&done)));

    for (unsigned i = 0; i < count; ++i)
        EXPECT_EQ(s.engine_at(i).load(), batch / count) << "engine " << i << " got the wrong share";

    released.store(true);
    s.stop();
    EXPECT_EQ(done.load(), static_cast<int>(batch));
}

TEST(scheduler, retries_on_full_engine) {
    scheduler             s{2};
    std::atomic<unsigned> started{0};
    std::atomic<bool>     released{false};
    std::atomic<int>      done{0};
    std::vector<std::coroutine_handle<>> frames;

    for (unsigned i = 0; i < 2; ++i)
        ASSERT_TRUE(s.engine_at(i).spawn(hold(&started, &released)));
    wait_for_holds(&started, 2);

    ASSERT_TRUE(fill(s.engine_at(0), frames)) << "engine 0 did not accept a full queue";

    // Round robin starts at engine 0, which is full: the frame must move on to engine 1.
    ASSERT_TRUE(s.submit(touch(&done)));
    EXPECT_EQ(s.engine_at(0).load(), QUEUE_CAPACITY) << "the full engine kept the frame";
    EXPECT_EQ(s.engine_at(1).load(), 1U) << "the frame did not reach the second engine";

    released.store(true);
    s.stop();
    EXPECT_EQ(done.load(), 1);

    for (auto frame : frames)
        frame.destroy();
}

TEST(scheduler, submit_returns_false_when_all_full) {
    scheduler             s{1};
    std::atomic<unsigned> started{0};
    std::atomic<bool>     released{false};
    std::atomic<int>      done{0};
    std::vector<std::coroutine_handle<>> frames;

    ASSERT_TRUE(s.engine_at(0).spawn(hold(&started, &released)));
    wait_for_holds(&started, 1);
    ASSERT_TRUE(fill(s.engine_at(0), frames));

    EXPECT_FALSE(s.submit(touch(&done))) << "a full scheduler must reject the task";

    released.store(true);
    s.stop();
    EXPECT_EQ(done.load(), 0);

    for (auto frame : frames)
        frame.destroy();
}
