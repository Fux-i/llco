#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/trace/trace.decl.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace llco;

namespace {

constexpr auto TRACE_PATH = "trace_test.json";

auto slurp(const char *path) -> std::string {
    const std::ifstream in{path};
    std::ostringstream  text;
    text << in.rdbuf();
    return text.str();
}

auto count_of(const std::string &text, std::string_view needle) -> std::size_t {
    std::size_t count = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1))
        ++count;
    return count;
}

// every "ts":<int> in file order
auto timestamps(const std::string &text) -> std::vector<long long> {
    std::vector<long long> out;
    constexpr auto         key = std::string_view{R"("ts":)"};
    for (auto pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + 1))
        out.push_back(std::stoll(text.substr(pos + key.size())));
    return out;
}

} // namespace

TEST(trace, pairs_slices_and_names_lanes) {
    trace::set_thread_name("t0");
    {
        const trace::scope outer{"outer"};
        const trace::scope inner{"inner"};
    }
    ASSERT_TRUE(trace::flush(TRACE_PATH)) << "flush could not write the file";

    const auto json = slurp(TRACE_PATH);
    EXPECT_EQ(std::remove(TRACE_PATH), 0);

    ASSERT_FALSE(json.empty()) << "flush wrote nothing";
    EXPECT_EQ(json.front(), '[') << "the file must be a JSON array";

    EXPECT_EQ(count_of(json, R"("ph":"M")"), 1U)
        << "each lane needs one thread_name metadata event";
    EXPECT_NE(json.find(R"("name":"thread_name")"), std::string::npos);
    EXPECT_NE(json.find(R"("name":"t0")"), std::string::npos);

    EXPECT_EQ(count_of(json, R"("ph":"B")"), 2U);
    EXPECT_EQ(count_of(json, R"("ph":"E")"), 2U);

    // nesting order: outer B, inner B, inner E, outer E
    const auto outer_b = json.find(R"("ph":"B")");
    const auto inner_b = json.find(R"("ph":"B")", outer_b + 1);
    const auto inner_e = json.find(R"("ph":"E")");
    const auto outer_e = json.find(R"("ph":"E")", inner_e + 1);
    EXPECT_LT(outer_b, inner_b);
    EXPECT_LT(inner_b, inner_e);
    EXPECT_LT(inner_e, outer_e);

    const auto ts = timestamps(json);
    ASSERT_EQ(ts.size(), 4U);
    EXPECT_TRUE(std::ranges::is_sorted(ts)) << "timestamps must not go backwards within a lane";
}

TEST(trace, keeps_one_lane_per_thread) {
    trace::set_thread_name("main");
    { const trace::scope slice{"main-slice"}; }

    std::thread worker{[] {
        trace::set_thread_name("worker");
        const trace::scope slice{"worker-slice"};
    }};
    worker.join(); // its lane must survive the thread

    ASSERT_TRUE(trace::flush(TRACE_PATH)) << "flush could not write the file";
    const auto json = slurp(TRACE_PATH);
    EXPECT_EQ(std::remove(TRACE_PATH), 0);

    EXPECT_EQ(count_of(json, R"("ph":"M")"), 2U) << "one metadata event per lane";
    EXPECT_NE(json.find(R"("name":"worker")"), std::string::npos);
    EXPECT_EQ(count_of(json, R"("ph":"B")"), 2U);
    EXPECT_EQ(count_of(json, R"("ph":"E")"), 2U);
}

#if LLCO_TRACE
#include <llco/core/task.hpp>
#include <llco/runtime/engine.decl.hpp>
#include <llco/runtime/scheduler.decl.hpp>

#include <atomic>
#include <chrono>

namespace {

constexpr auto LANE_POLL = std::chrono::milliseconds{1};

task<> noop() {
    co_return;
}

task<> touch(std::atomic<int> *done) {
    ++*done;
    co_return;
}

} // namespace

TEST(trace, engine_emits_resume_slices) {
    trace::set_thread_name("engine");
    engine e;

    ASSERT_TRUE(e.spawn(noop()));
    EXPECT_EQ(e.drain(), 1U);
    ASSERT_TRUE(trace::flush(TRACE_PATH)) << "flush could not write the file";

    const auto json = slurp(TRACE_PATH);
    std::remove(TRACE_PATH);

    EXPECT_EQ(count_of(json, R"("name":"resume","ph":"B")"), 1U);
    EXPECT_EQ(count_of(json, R"("name":"resume","ph":"E")"), 1U);
}

TEST(trace, scheduler_names_lanes) {
    scheduler::init(2);
    std::atomic<int> done{0};
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(scheduler::submit(touch(&done)));
    while (done.load() < 4)
        std::this_thread::sleep_for(LANE_POLL);
    scheduler::stop();

    ASSERT_TRUE(trace::flush(TRACE_PATH)) << "flush could not write the file";
    const auto json = slurp(TRACE_PATH);
    std::remove(TRACE_PATH);

    EXPECT_NE(json.find(R"("name":"engine-0")"), std::string::npos) << "engines need lane names";
    EXPECT_NE(json.find(R"("name":"engine-1")"), std::string::npos);
}
#endif
