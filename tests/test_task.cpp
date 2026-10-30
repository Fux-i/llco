#include <gtest/gtest.h>
#include <llco/concepts/task_maker.hpp>
#include <llco/core/task.hpp>
#include <llco/runtime/run.decl.hpp>

#include <utility>
#include <vector>

using namespace llco;

namespace {

task<> noop() {
    co_return;
}

task<> maker() {
    co_return;
}

int one() {
    return 1;
}

// Task: only task<T> specializations qualify.
static_assert(Task<task<>>);
static_assert(Task<task<int>>);
static_assert(!Task<int>);

// TaskMaker: invocable and returning a task.
static_assert(TaskMaker<decltype(&maker)>);
static_assert(TaskMaker<decltype([] { return noop(); })>);
static_assert(!TaskMaker<decltype(&one)>);
static_assert(!TaskMaker<int>);

// A range of tasks is consumed from an rvalue only.
template <class R>
concept AcceptsRvalueRange = requires(R &r) { run(std::move(r)); };

template <class R>
concept AcceptsLvalueRange = requires(R &r) { run(r); };

static_assert(AcceptsRvalueRange<std::vector<task<>>>);
static_assert(!AcceptsLvalueRange<std::vector<task<>>>);

} // namespace

TEST(task, move_transfers_the_frame) {
    auto moved_from = noop();
    auto moved_to   = std::move(moved_from);

    // NOLINTNEXTLINE(bugprone-use-after-move): reading the moved-from handle is the assertion
    EXPECT_EQ(moved_from.handle(), nullptr) << "a moved-from task must not own the frame";
    EXPECT_NE(moved_to.handle(), nullptr);
    // Both destructors run at the end: a double destroy trips ASan.
}

TEST(task, moves_into_a_container) {
    std::vector<task<>> batch;
    batch.push_back(noop());
    batch.push_back(noop());

    EXPECT_EQ(batch.size(), 2U);
}