#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/engine.decl.hpp>
#include <llco/resume_on.hpp>
#include <llco/task.hpp>

#include <coroutine>
#include <vector>

using namespace llco;

namespace {

task<> noop() {
    co_return;
}

task<> hopper(engine &target, int &steps) {
    steps = 1;
    co_await resume_on(target);
    steps = 2;
}

} // namespace

TEST(resume_on, hops_through_queue) {
    engine e;
    int    steps = 0;

    ASSERT_TRUE(e.spawn(hopper(e, steps)));
    ASSERT_TRUE(e.run_once()) << "the task did not start";
    EXPECT_EQ(steps, 1) << "the body continued inline instead of hopping";

    EXPECT_EQ(e.drain(), 1U) << "the hopped frame must wait in the queue";
    EXPECT_EQ(steps, 2);
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

    auto hopper_task = hopper(e, steps);
    hopper_task.resume(); // resumed by hand, so the queue is still full when it hops
    EXPECT_EQ(steps, 2) << "a full queue must not stall the body";

    EXPECT_EQ(e.drain(), QUEUE_CAPACITY) << "the hop must not enqueue anything";
    for (auto frame : frames)
        frame.destroy();
}
