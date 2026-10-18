#pragma once

#include "attr.hpp"
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>

namespace llco {

template <typename T>
class task;

struct promise_base {
    using handle_type = std::coroutine_handle<>;

  protected:
    handle_type        m_continuation;
    std::exception_ptr m_exception;

  public:
    friend struct final_awaiter;
    struct final_awaiter {
        auto await_ready() const noexcept -> bool {
            return false;
        }
        template <typename P>
        auto await_suspend(std::coroutine_handle<P> caller) const noexcept -> handle_type {
            auto &promise = caller.promise();
            if (promise.m_continuation != nullptr)
                return promise.m_continuation;
            else
                return std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };

    promise_base()  = default;
    ~promise_base() = default;
    CLASS_NO_COPY_MOVE(promise_base);

    void set_continuation(handle_type handle) {
        m_continuation = handle;
    }

    auto initial_suspend() -> std::suspend_always {
        return {};
    }
    auto final_suspend() noexcept -> final_awaiter {
        return {};
    }
};

template <typename T>
struct promise : promise_base {
    using task_type = task<T>;

  private:
    std::optional<T> m_value;

  public:
    auto get_return_object() -> task_type {
        auto handle = std::coroutine_handle<promise>::from_promise(*this);
        return task<T>(handle);
    }
    void return_value(const T &value) {
        m_value.emplace(value);
    }
    void return_value(T &&value) {
        m_value.emplace(std::move(value));
    }
    auto result() -> T {
        if (m_exception != nullptr)
            std::rethrow_exception(m_exception);
        if (m_value.has_value())
            return std::move(*m_value);
        throw std::runtime_error{"The return value was never set, did you execute the coroutine?"};
    }

    void unhandled_exception() {
        m_exception = std::current_exception();
    }
};

template <typename T = void>
class task {
  public:
    using promise_type = promise<T>;
    using handle_type  = std::coroutine_handle<promise<T>>;

    // set my caller and continue on me. Then get the result from my promise.
    struct awaiter {
        handle_type m_handle;

        auto await_ready() const noexcept -> bool {
            return false;
        }
        auto await_suspend(std::coroutine_handle<> caller) -> handle_type {
            m_handle.promise().set_continuation(caller);
            return m_handle;
        }
        auto await_resume() -> T {
            return m_handle.promise().result();
        }
    };

  private:
    handle_type m_handle;

  public:
    explicit task(handle_type handle) noexcept : m_handle(handle) {}
    ~task() {
        if (m_handle != nullptr)
            m_handle.destroy();
    }
    CLASS_NO_COPY_MOVE(task);

    auto operator co_await() -> awaiter {
        return awaiter{m_handle};
    }

    auto resume() const -> void {
        m_handle.resume();
    }
    auto result() const -> T {
        return m_handle.promise().result();
    }
};

template <>
struct promise<void> : promise_base {
    using task_type = task<void>;

  public:
    auto get_return_object() -> task_type {
        auto handle = std::coroutine_handle<promise>::from_promise(*this);
        return task_type(handle);
    }
    void return_void() {}
    void result() {
        if (m_exception != nullptr)
            std::rethrow_exception(m_exception);
    }
    void unhandled_exception() {
        m_exception = std::current_exception();
    }
};
} // namespace llco
