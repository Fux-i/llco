#include <gtest/gtest.h>
#include <llco/config.hpp>
#include <llco/runtime/engine.decl.hpp>
#include <llco/core/task.hpp>

#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

using namespace llco;

namespace {

// thrown by fail_raw; the test matches on the raw value
constexpr auto RAW_CODE = 42;

// lives in the coroutine frame; sets the flag when the frame is destroyed
struct probe {
    bool *flag;

    explicit probe(bool *f) noexcept : flag(f) {}
    probe(probe &&other) noexcept : flag(std::exchange(other.flag, nullptr)) {}
    ~probe() {
        if (flag != nullptr)
            *flag = true;
    }
};

task<> watch(probe) {
    co_return;
}

task<int> counted(probe, int value) {
    co_return value;
}

task<> noop() {
    co_return;
}

task<> fail() {
    throw std::runtime_error{"boom"};
    co_return;
}

task<> child_fail() {
    throw std::runtime_error{"child"};
    co_return;
}

task<> parent() {
    co_await child_fail();
    co_return;
}

task<> fail_raw() {
    // the point of this test is a non-std exception
    // NOLINTNEXTLINE(cert-err09-cpp, cert-err61-cpp, misc-throw-by-value-catch-by-reference)
    throw RAW_CODE;
    co_return;
}

} // namespace

TEST(task, release_transfers_ownership) {
    bool                    destroyed = false;
    std::coroutine_handle<> released;

    {
        auto t = watch(probe{&destroyed});
        t.resume();
        released = t.release();
        EXPECT_NE(released, nullptr);
    }
    EXPECT_FALSE(destroyed) << "~task destroyed a released frame";

    released.destroy();
    EXPECT_TRUE(destroyed);
}

TEST(spawn, runs_and_destroys_frame) {
    bool   destroyed = false;
    engine e;

    EXPECT_TRUE(e.spawn(watch(probe{&destroyed})));
    EXPECT_FALSE(destroyed) << "spawn ran the task before the engine was driven";

    EXPECT_EQ(e.drain(), 1U);
    EXPECT_TRUE(destroyed) << "detached frame outlived its completion";
}

TEST(spawn, value_task_destroys_frame) {
    bool   destroyed = false;
    engine e;

    EXPECT_TRUE(e.spawn(counted(probe{&destroyed}, 7)));
    EXPECT_EQ(e.drain(), 1U);

    EXPECT_TRUE(destroyed);
}

TEST(spawn, exception_goes_to_handler) {
    std::exception_ptr caught;
    engine             e;
    e.set_exception_handler([&caught](const std::exception_ptr &ep) { caught = ep; });

    EXPECT_TRUE(e.spawn(fail()));
    EXPECT_EQ(e.drain(), 1U);

    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}

TEST(spawn, child_exception_goes_to_handler) {
    std::exception_ptr caught;
    engine             e;
    e.set_exception_handler([&caught](const std::exception_ptr &ep) { caught = ep; });

    EXPECT_TRUE(e.spawn(parent()));
    EXPECT_EQ(e.drain(), 1U);

    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}

TEST(spawn, non_std_exception_goes_to_handler) {
    std::exception_ptr caught;
    engine             e;
    e.set_exception_handler([&caught](const std::exception_ptr &ep) { caught = ep; });

    EXPECT_TRUE(e.spawn(fail_raw()));
    EXPECT_EQ(e.drain(), 1U);

    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), int);
}

TEST(spawn, full_queue_returns_false) {
    bool   first  = false;
    bool   second = false;
    engine e;

    for (unsigned i = 0; i < QUEUE_CAPACITY; ++i)
        EXPECT_TRUE(e.spawn(watch(probe{&first})));

    EXPECT_FALSE(e.spawn(watch(probe{&second}))) << "spawn accepted a task into a full queue";
    EXPECT_TRUE(second) << "the rejected frame leaked";

    EXPECT_EQ(e.drain(), QUEUE_CAPACITY);
    EXPECT_TRUE(first);
}
