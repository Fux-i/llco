#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/engine.decl.hpp>
#include <llco/task.hpp>

#include <coroutine>
#include <vector>

using namespace llco;

namespace {

std::vector<int> g_log;

task<> record(int id) {
    g_log.push_back(id);
    co_return;
}

task<> noop() {
    co_return;
}

} // namespace

TEST(engine, manual_runs_handles_in_fifo_order) {
    g_log.clear();
    engine e;

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
    engine e;

    std::vector<std::coroutine_handle<>> frames; // the test owns the frames
    frames.reserve(QUEUE_CAPACITY);
    for (unsigned i = 0; i < QUEUE_CAPACITY; ++i) {
        frames.push_back(noop().release());
        EXPECT_TRUE(e.try_add(frames.back()));
    }

    auto extra = noop().release();
    EXPECT_FALSE(e.try_add(extra)) << "queue accepted one handle past capacity";
    extra.destroy();

    EXPECT_EQ(e.drain(), QUEUE_CAPACITY);
    for (auto frame : frames)
        frame.destroy();
}

TEST(engine, load_tracks_queued_handles) {
    engine e;
    EXPECT_EQ(e.load(), 0U) << "a fresh engine holds no queued work";

    auto first  = record(1);
    auto second = record(2);
    EXPECT_TRUE(e.try_add(first.handle()));
    EXPECT_TRUE(e.try_add(second.handle()));
    EXPECT_EQ(e.load(), 2U);

    EXPECT_TRUE(e.run_once());
    EXPECT_EQ(e.load(), 1U) << "a resumed handle is no longer queued";
    EXPECT_EQ(e.drain(), 1U);
    EXPECT_EQ(e.load(), 0U);
}

TEST(engine, drain_returns_count) {
    g_log.clear();
    engine e;

    EXPECT_EQ(e.drain(), 0U) << "an empty engine must drain nothing";

    auto first  = record(1);
    auto second = record(2);
    EXPECT_TRUE(e.try_add(first.handle()));
    EXPECT_TRUE(e.try_add(second.handle()));

    EXPECT_EQ(e.drain(), 2U);
    EXPECT_EQ(e.drain(), 0U);
    EXPECT_EQ(g_log, (std::vector<int>{1, 2}));
}
