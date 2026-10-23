#include <gtest/gtest.h>
#include <llco/core/task.hpp>
#include <llco/runtime/engine.decl.hpp>
#include <llco/runtime/scheduler.decl.hpp>
#include <llco/sync/event.hpp>
#include <llco/sync/sync_wait.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto POLL               = 1ms;
constexpr int  VALUE              = 42;
constexpr int  CROSS_ENGINE_VALUE = 7;
constexpr int  ROOT_VALUE         = 11;

task<> waiter(event<> *e, int *steps) {
    *steps = 1;
    co_await e->wait();
    *steps = 2;
    co_return;
}

task<> setter(event<> *e) {
    e->set();
    co_return;
}

task<> count_waiter(event<> *e, std::atomic<int> *done) {
    co_await e->wait();
    ++*done;
    co_return;
}

task<> value_waiter(event<int> *e, int *out) {
    *out = co_await e->wait();
    co_return;
}

task<> value_setter(event<int> *e) {
    e->set(VALUE);
    co_return;
}

// On a threaded engine, set() must post the waiter instead of resuming it inside set().
task<> posting_waiter(event<> *e, std::atomic<bool> *setter_returned, std::atomic<bool> *resumed,
                      std::atomic<bool> *ran_after_set) {
    co_await e->wait();
    ran_after_set->store(setter_returned->load());
    resumed->store(true);
    co_return;
}

task<> posting_setter(event<> *e, std::atomic<bool> *setter_returned) {
    e->set();
    setter_returned->store(true);
    co_return;
}

task<> thread_waiter(event<int> *e, std::atomic<std::thread::id> *before,
                     std::atomic<std::thread::id> *after, std::atomic<int> *value,
                     std::atomic<bool> *armed) {
    before->store(std::this_thread::get_id());
    armed->store(true);
    *value = co_await e->wait();
    after->store(std::this_thread::get_id());
    co_return;
}

task<> thread_setter(event<int> *e, std::atomic<bool> *armed, std::atomic<bool> *done) {
    while (!armed->load())
        std::this_thread::yield();
    std::this_thread::sleep_for(1ms); // give the waiter time to publish
    e->set(CROSS_ENGINE_VALUE);
    done->store(true);
    co_return;
}

task<int> root_waiter(event<int> *e, std::atomic<bool> *waiting) {
    waiting->store(true);
    co_return co_await e->wait();
}

task<> root_setter(event<int> *e, std::atomic<bool> *waiting) {
    while (!waiting->load())
        std::this_thread::yield();
    e->set(ROOT_VALUE);
    co_return;
}

} // namespace

TEST(event, wakes_a_waiter) {
    engine  e;
    event<> ev;
    int     steps = 0;

    ASSERT_TRUE(e.spawn(waiter(&ev, &steps)));
    ASSERT_TRUE(e.run_once());
    EXPECT_EQ(steps, 1) << "the waiter did not suspend";

    ASSERT_TRUE(e.spawn(setter(&ev)));
    ASSERT_TRUE(e.run_once());
    EXPECT_EQ(steps, 2) << "set() did not resume the waiter";
}

TEST(event, set_before_wait_completes_immediately) {
    engine  e;
    event<> ev;
    int     steps = 0;

    ev.set();
    ASSERT_TRUE(e.spawn(waiter(&ev, &steps)));
    ASSERT_TRUE(e.run_once());
    EXPECT_EQ(steps, 2) << "an already set event must not suspend";
}

TEST(event, value_reaches_the_waiter) {
    engine    e;
    event<int> ev;
    int        value = 0;

    ASSERT_TRUE(e.spawn(value_waiter(&ev, &value)));
    ASSERT_TRUE(e.run_once());
    EXPECT_EQ(value, 0);

    ASSERT_TRUE(e.spawn(value_setter(&ev)));
    ASSERT_TRUE(e.run_once());
    EXPECT_EQ(value, VALUE);
}

TEST(event, broadcasts_to_every_waiter) {
    engine           e;
    event<>          ev;
    std::atomic<int> done{0};

    ASSERT_TRUE(e.spawn(count_waiter(&ev, &done)));
    ASSERT_TRUE(e.spawn(count_waiter(&ev, &done)));
    EXPECT_EQ(e.drain(), 2U) << "both waiters must suspend";
    EXPECT_EQ(done.load(), 0);

    ASSERT_TRUE(e.spawn(setter(&ev)));
    EXPECT_EQ(e.drain(), 1U) << "only the setter went through the queue";
    EXPECT_EQ(done.load(), 2) << "set() must resume every waiter inline on a manual engine";
}

TEST(event, set_posts_even_on_the_same_engine) {
    engine            e;
    event<>           ev;
    std::atomic<bool> setter_returned{false};
    std::atomic<bool> resumed{false};
    std::atomic<bool> ran_after_set{false};

    e.start();
    ASSERT_TRUE(e.spawn(posting_waiter(&ev, &setter_returned, &resumed, &ran_after_set)));
    ASSERT_TRUE(e.spawn(posting_setter(&ev, &setter_returned)));

    while (!resumed.load())
        std::this_thread::sleep_for(POLL);
    e.stop();

    EXPECT_TRUE(ran_after_set.load()) << "the waiter ran before set() returned: not posted";
}

TEST(event, wakes_a_waiter_on_its_own_engine) {
    scheduler                    s{2};
    event<int>                   ev;
    std::atomic<std::thread::id> before{};
    std::atomic<std::thread::id> after{};
    std::atomic<int>             value{0};
    std::atomic<bool>            armed{false};
    std::atomic<bool>            done{false};

    ASSERT_TRUE(s.engine_at(0).spawn(thread_waiter(&ev, &before, &after, &value, &armed)));
    ASSERT_TRUE(s.engine_at(1).spawn(thread_setter(&ev, &armed, &done)));

    while (!done.load())
        std::this_thread::sleep_for(POLL);
    s.stop();

    EXPECT_EQ(value.load(), CROSS_ENGINE_VALUE);
    EXPECT_EQ(before.load(), after.load()) << "the waiter must resume on its own engine";
    EXPECT_NE(before.load(), std::this_thread::get_id()) << "the waiter never ran on an engine";
}

TEST(event, wakes_a_sync_wait_root) {
    engine            e;
    event<int>        ev;
    std::atomic<bool> waiting{false};

    e.start();
    ASSERT_TRUE(e.spawn(root_setter(&ev, &waiting)));
    EXPECT_EQ(sync_wait(root_waiter(&ev, &waiting)), ROOT_VALUE);
    e.stop();
}
