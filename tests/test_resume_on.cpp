#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/runtime/engine.decl.hpp>
#include <llco/core/resume_on.hpp>
#include <llco/runtime/scheduler.decl.hpp>
#include <llco/core/task.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <thread>
#include <vector>

using namespace llco;

namespace {

constexpr auto POLL = std::chrono::milliseconds{1};

task<> noop() {
    co_return;
}

task<> hopper(engine *target, int *steps) {
    *steps = 1;
    co_await resume_on(*target);
    *steps = 2;
}

task<> thread_hopper(engine *target, std::atomic<std::thread::id> *before,
                     std::atomic<std::thread::id> *after, std::atomic<bool> *done) {
    before->store(std::this_thread::get_id());
    co_await resume_on(*target);
    after->store(std::this_thread::get_id());
    done->store(true);
}

} // namespace

TEST(resume_on, hops_through_queue) {
    engine e;
    int    steps = 0;

    ASSERT_TRUE(e.spawn(hopper(&e, &steps)));
    ASSERT_TRUE(e.run_once()) << "the task did not start";
    EXPECT_EQ(steps, 1) << "the body continued inline instead of hopping";

    EXPECT_EQ(e.drain(), 1U) << "the hopped frame must wait in the queue";
    EXPECT_EQ(steps, 2);
}

TEST(resume_on, cross_engine_hop_changes_thread) {
    scheduler                    s{2};
    std::atomic<std::thread::id> before{};
    std::atomic<std::thread::id> after{};
    std::atomic<bool>            done{false};

    ASSERT_TRUE(s.engine_at(0).spawn(thread_hopper(&s.engine_at(1), &before, &after, &done)));
    while (!done.load())
        std::this_thread::sleep_for(POLL);
    s.stop();

    const auto main_id = std::this_thread::get_id();
    EXPECT_NE(before.load(), main_id) << "the task did not start on an engine thread";
    EXPECT_NE(after.load(), before.load()) << "the hop stayed on the same thread";
}

TEST(resume_on, full_queue_continues_inline) {
    engine e;
    int    steps = 0;

    std::vector<std::coroutine_handle<>> frames; // the test owns the frames
    frames.reserve(QUEUE_CAPACITY);
    for (unsigned i = 0; i < QUEUE_CAPACITY; ++i) {
        frames.push_back(noop().release());
        ASSERT_TRUE(e.try_add(frames.back()));
    }

    auto hopper_task = hopper(&e, &steps);
    hopper_task.resume(); // resumed by hand, so the queue is still full when it hops
    EXPECT_EQ(steps, 2) << "a full queue must not stall the body";

    EXPECT_EQ(e.drain(), QUEUE_CAPACITY) << "the hop must not enqueue anything";
    for (auto frame : frames)
        frame.destroy();
}
