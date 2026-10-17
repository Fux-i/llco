#pragma once

#include "attr.hpp"
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace llco {

template <typename T>
class task;

template <typename T>
struct promise {
    using task_type   = task<T>;
    using handle_type = std::coroutine_handle<>;

  private:
    handle_type        m_continuation;
    std::optional<T>   m_value;
    std::exception_ptr m_except;

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

    promise()  = default;
    ~promise() = default;
    CLASS_NO_COPY_MOVE(promise);

    void set_continuation(handle_type handle) {
        m_continuation = handle;
    }

    auto get_return_object() -> task_type {
        auto handle = std::coroutine_handle<promise>::from_promise(*this);
        return task<T>(handle);
    }
    auto initial_suspend() -> std::suspend_always {
        return {};
    }
    auto final_suspend() noexcept -> final_awaiter {
        return {};
    }

    void return_value(const T &value) {
        m_value.emplace(value);
    }
    void return_value(T &&value) {
        m_value.emplace(std::move(value));
    }
    auto result() -> T {
        if (m_except != nullptr)
            std::rethrow_exception(m_except);
        if (m_value.has_value())
            return std::move(*m_value);
        else
            throw std::runtime_error{
                "The return value was never set, did you execute the coroutine?"};
    }

    void unhandled_exception() {
        m_except = std::current_exception();
    }
};

template <typename T>
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
    task(const task &)            = delete;
    task &operator=(const task &) = delete;
    // task(task &&other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}

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
} // namespace llco
