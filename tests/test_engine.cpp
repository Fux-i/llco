#include <gtest/gtest.h>
#include <llco/engine.decl.hpp>
#include <llco/queue.decl.hpp>
#include <llco/task.hpp>

#include <vector>

using namespace llco;

namespace {

std::vector<int> g_log;

task<> record(int id) {
    g_log.push_back(id);
    co_return;
}

} // namespace

TEST(engine, manual_runs_handles_in_fifo_order) {
    g_log.clear();
    engine<> e;

    auto first  = record(1);
    auto second = record(2);
    auto third  = record(3);

    EXPECT_TRUE(e.try_add(first.handle()));
    EXPECT_TRUE(e.try_add(second.handle()));
    EXPECT_TRUE(e.try_add(third.handle()));
    EXPECT_TRUE(g_log.empty()) << "tasks ran before the engine was driven";

    EXPECT_EQ(e.drain(), 3U);
    EXPECT_EQ(g_log, (std::vector<int>{1, 2, 3}));
}

TEST(engine, add_fails_when_full) {
    g_log.clear();
    engine<queue<std::coroutine_handle<>, 4>> e;

    auto one   = record(1);
    auto two   = record(2);
    auto three = record(3);
    auto four  = record(4);
    auto five  = record(5);

    EXPECT_TRUE(e.try_add(one.handle()));
    EXPECT_TRUE(e.try_add(two.handle()));
    EXPECT_TRUE(e.try_add(three.handle()));
    EXPECT_TRUE(e.try_add(four.handle()));
    EXPECT_FALSE(e.try_add(five.handle())) << "a 4-slot queue accepted a 5th handle";

    EXPECT_EQ(e.drain(), 4U);
    EXPECT_EQ(g_log, (std::vector<int>{1, 2, 3, 4}));
}

TEST(engine, drain_returns_count) {
    g_log.clear();
    engine<> e;

    EXPECT_EQ(e.drain(), 0U) << "an empty engine must drain nothing";

    auto first  = record(1);
    auto second = record(2);
    EXPECT_TRUE(e.try_add(first.handle()));
    EXPECT_TRUE(e.try_add(second.handle()));

    EXPECT_EQ(e.drain(), 2U);
    EXPECT_EQ(e.drain(), 0U);
    EXPECT_EQ(g_log, (std::vector<int>{1, 2}));
}
