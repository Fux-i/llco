#include <gtest/gtest.h>
#include <llco/runtime/engine.decl.hpp>
#include <llco/core/resume_on.hpp>
#include <llco/sync/sync_wait.hpp>
#include <llco/core/task.hpp>

#include <stdexcept>

using namespace llco;

namespace {

constexpr int TARGET_VALUE = 7;

task<int> value_task(int value) {
    co_return value;
}

task<int> failing_task() {
    throw std::runtime_error{"boom"};
    co_return 0;
}

task<int> resuming_task(engine *target) {
    co_await resume_on(*target);
    co_return TARGET_VALUE;
}

} // namespace

TEST(sync_wait, returns_value) {
    constexpr int expected = 42;
    EXPECT_EQ(sync_wait(value_task(expected)), expected);
}

TEST(sync_wait, propagates_exception) {
    EXPECT_THROW(sync_wait(failing_task()), std::runtime_error);
}

TEST(sync_wait, waits_for_cross_thread_resume) {
    engine e;
    e.start();
    EXPECT_EQ(sync_wait(resuming_task(&e)), TARGET_VALUE);
    e.stop();
}