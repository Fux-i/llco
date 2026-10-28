#pragma once

#include <llco/core/task.hpp>
#include <llco/runtime/scheduler.decl.hpp>

#include <atomic>
#include <concepts>
#include <semaphore>
#include <stdexcept>
#include <utility>

namespace llco {

namespace detail {

// Waits for a batch of submitted tasks; the last count_down releases the blocked caller.
class batch {
  private:
    std::atomic<unsigned> m_pending;
    std::binary_semaphore m_done{0};

  public:
    explicit batch(unsigned count) noexcept : m_pending{count} {}

    void count_down() noexcept {
        if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            m_done.release();
    }

    void wait() noexcept {
        m_done.acquire();
    }
};

// Runs the task for the batch and drops its value; the detached frame self-destroys.
// The pointer is the caller's task, which outlives the call because sync_run blocks.
template <class T>
task<> counted(task<T> *t, batch *b) {
    (void)co_await *t;
    b->count_down();
}

// Holds the callable inside a frame: a lambda coroutine keeps its closure as "this", so the
// closure must outlive the frame it starts.
template <class F>
task<> invoked(F make) {
    co_await make();
}

} // namespace detail

// Submits every task and returns; throws std::runtime_error on the first rejection (ADR-0004).
template <class... T>
void run(task<T> &&...tasks) {
    if (!(scheduler::submit(std::move(tasks)) && ...))
        throw std::runtime_error{"scheduler is full"};
}

// Runs what the callable returns, e.g. llco::run([]() -> task<> { co_return; });
template <class F>
    requires std::invocable<F &>
void run(F &&make) {
    run(detail::invoked<std::decay_t<F>>(std::forward<F>(make)));
}

// Submits every task and blocks until all of them have finished.
template <class... T>
void sync_run(task<T> &&...tasks) {
    if constexpr (sizeof...(T) > 0) {
        detail::batch done{static_cast<unsigned>(sizeof...(T))};
        bool          all        = true;
        auto          submit_one = [&done, &all](auto &t) {
            if (scheduler::submit(detail::counted(&t, &done)))
                return;
            all = false;
            done.count_down();
        };
        (submit_one(tasks), ...);
        done.wait();
        if (!all)
            throw std::runtime_error{"scheduler is full"};
    }
}

// Runs what the callable returns, then blocks, e.g. llco::sync_run([]() -> task<> { co_return; });
template <class F>
    requires std::invocable<F &>
void sync_run(F &&make) {
    sync_run(detail::invoked<std::decay_t<F>>(std::forward<F>(make)));
}

} // namespace llco