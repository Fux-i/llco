#include <gtest/gtest.h>
#include <llco/engine.decl.hpp>
#include <llco/resume_on.hpp>
#include <llco/sync_wait.decl.hpp>
#include <llco/task.hpp>

#include <stdexcept>

using namespace llco;

namespace {

constexpr int HOP_VALUE = 7;

task<int> value_task(int value) {
    co_return value;
}

task<int> failing_task() {
    throw std::runtime_error{"boom"};
    co_return 0;
}

task<int> hopping_task(engine *target) {
    co_await resume_on(*target);
    co_return HOP_VALUE;
}

} // namespace

TEST(sync_wait, returns_value) {
    constexpr int expected = 42;
    EXPECT_EQ(sync_wait(value_task(expected)), expected);
}

TEST(sync_wait, propagates_exception) {
    EXPECT_THROW(sync_wait(failing_task()), std::runtime_error);
}

TEST(sync_wait, waits_for_cross_thread_hop) {
    engine e;
    e.start();
    EXPECT_EQ(sync_wait(hopping_task(&e)), HOP_VALUE);
    e.stop();
}
