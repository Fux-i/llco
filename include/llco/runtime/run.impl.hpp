#pragma once

#include <llco/runtime/run.decl.hpp>
#include <llco/runtime/scheduler.decl.hpp>

#include <atomic>
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
    explicit batch(unsigned count) noexcept : m_pending{count} {
        if (count == 0)
            m_done.release();
    }

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

template <class... T>
void run(task<T> &&...tasks) {
    if (!(scheduler::submit(std::move(tasks)) && ...))
        throw std::runtime_error{"scheduler is full"};
}

template <TaskMaker F>
void run(F &&make) {
    run(detail::invoked<std::decay_t<F>>(std::forward<F>(make)));
}

template <class R>
    requires std::ranges::range<R> && Task<std::ranges::range_value_t<R>> &&
             (!std::is_lvalue_reference_v<R>)
void run(R &&tasks) {
    for (auto &t : std::forward<R>(tasks))
        if (!scheduler::submit(std::move(t)))
            throw std::runtime_error{"scheduler is full"};
}

template <class... T>
void sync_run(task<T> &&...tasks) {
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

template <TaskMaker F>
void sync_run(F &&make) {
    sync_run(detail::invoked<std::decay_t<F>>(std::forward<F>(make)));
}

template <class R>
    requires std::ranges::range<R> && std::ranges::sized_range<R> &&
             Task<std::ranges::range_value_t<R>> && (!std::is_lvalue_reference_v<R>)
void sync_run(R &&tasks) {
    detail::batch done{static_cast<unsigned>(std::ranges::size(tasks))};
    bool          all = true;
    for (auto &t : std::forward<R>(tasks)) {
        if (scheduler::submit(detail::counted(&t, &done)))
            continue;
        all = false;
        done.count_down();
    }
    done.wait();
    if (!all)
        throw std::runtime_error{"scheduler is full"};
}

} // namespace llco