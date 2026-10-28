#pragma once

#include <llco/attr.hpp>
#include <llco/concepts/executor.hpp>

#include <coroutine>

namespace llco {

// Resumes the current coroutine on another executor instead of continuing inline.
template <class E>
class [[LLCO_AWAIT_HINT]] resume_on_awaiter {
  private:
    E &m_executor;

  public:
    explicit resume_on_awaiter(E &executor) noexcept : m_executor(executor) {}

    auto await_ready() const noexcept -> bool {
        return false;
    }

    // hands the frame to the target; a full queue continues inline (ADR-0004)
    auto await_suspend(std::coroutine_handle<> handle) noexcept -> std::coroutine_handle<> {
        return m_executor.try_add(handle) ? std::noop_coroutine() : handle;
    }

    void await_resume() const noexcept {}
};

// resumes the current coroutine on executor
template <Executor E>
auto resume_on(E &executor) -> resume_on_awaiter<E> {
    return resume_on_awaiter<E>{executor};
}

} // namespace llco