#include <gtest/gtest.h>
#include <llco/core/task.hpp>

#include <string>
#include <vector>

using namespace llco;

namespace {

std::vector<std::string> g_log;

task<int> leaf(int value) {
    g_log.emplace_back("leaf.start");
    co_return value * 2;
}

task<int> middle(int input) {
    g_log.emplace_back("middle.before");
    const int output = co_await leaf(input + 1);
    g_log.emplace_back("middle.resumed");
    co_return output + 1;
}

task<int> top() {
    g_log.emplace_back("top.before");
    const int value = co_await middle(10);
    g_log.emplace_back("top.resumed");
    co_return value + 1;
}

task<> note(std::string what) {
    g_log.push_back(std::move(what));
    co_return;
}

task<> step(const char *name) {
    co_await note(std::string{name} + ".in");
    co_await note(std::string{name} + ".out");
}

task<> run() {
    co_await step("A");
    co_await step("B");
}

} // namespace

TEST(transfer, nested_three_levels) {
    g_log.clear();
    auto root = top();
    root.resume();

    EXPECT_EQ(root.result(), 24);
    EXPECT_EQ(g_log, (std::vector<std::string>{"top.before", "middle.before", "leaf.start",
                                               "middle.resumed", "top.resumed"}));
}

TEST(transfer, void_chain) {
    g_log.clear();
    auto root = run();
    root.resume();

    EXPECT_NO_THROW(root.result());
    EXPECT_EQ(g_log, (std::vector<std::string>{"A.in", "A.out", "B.in", "B.out"}));
}
