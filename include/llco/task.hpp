#pragma once

#include "attr.hpp"
#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace llco {

template <typename T>
class [[LLCO_AWAIT_HINT]] task;

struct promise_base {
    using handle_type       = std::coroutine_handle<>;
    using exception_handler = std::function<void(std::exception_ptr)>;

  protected:
    handle_type              m_continuation;
    std::exception_ptr       m_exception;
    const exception_handler *m_exception_handler{};
    bool                     m_detached{};

  public:
    friend struct final_awaiter;
    // transfer the workflow back to caller's continuation
    struct final_awaiter {
        auto await_ready() const noexcept -> bool {
            return false;
        }
        template <typename P>
        auto await_suspend(std::coroutine_handle<P> caller) const noexcept -> handle_type {
            auto &promise = caller.promise();
            // detached promise must have no continuation
            if (promise.m_detached) {
                if (promise.m_exception != nullptr)
                    (*promise.m_exception_handler)(promise.m_exception);
                caller.destroy();
                return std::noop_coroutine();
            }
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

    // hand the frame over to the engine: it self-destroys at final suspend
    void set_detached(const exception_handler &handler) noexcept {
        m_detached          = true;
        m_exception_handler = &handler;
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

    [[LLCO_DISCARD_HINT]]
    auto handle() const noexcept -> handle_type {
        return m_handle;
    }
    void resume() const {
        m_handle.resume();
    }
    // gives up the frame; the caller owns it from now on
    [[LLCO_DISCARD_HINT]]
    auto release() noexcept -> handle_type {
        return std::exchange(m_handle, nullptr);
    }
    [[LLCO_DISCARD_HINT]]
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
