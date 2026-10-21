#pragma once

#include "attr.hpp"
#include "queue.decl.hpp"
#include "task.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <stop_token>
#include <thread>

namespace llco {

// last resort for exceptions escaping detached tasks
inline void default_exception_handler(std::exception_ptr e);

// Single-threaded context; manual (caller-driven) until start() runs a loop thread.
class engine {
  private:
    queue<std::coroutine_handle<>>          m_queue;
    std::function<void(std::exception_ptr)> m_exception_handler = default_exception_handler;
    std::jthread                            m_thread;
    std::atomic<int>                        m_event_fd{-1};
    std::size_t                             m_loops{};

    void notify() noexcept;
    void wait() noexcept;
    void loop(const std::stop_token &token);

  public:
    ~engine();

    // runs a loop thread; the engine stays manual until then
    void start();

    // stops the loop thread, running what was already queued
    void stop();

    // loop iterations so far; read it after stop()
    auto loop_count() const noexcept -> std::size_t;

    using handle_type       = std::coroutine_handle<>;
    using exception_handler = promise_base::exception_handler;

    // false when full (ADR-0004)
    [[LLCO_DISCARD_HINT]]
    auto try_add(handle_type handle) noexcept -> bool;

    // false when empty
    auto run_once() -> bool;

    // returns how many handles were resumed
    [[LLCO_DISCARD_HINT]]
    auto drain() -> std::size_t;

    // transfers ownership; false when full (the frame is destroyed)
    template <class T>
    [[LLCO_DISCARD_HINT]]
    auto spawn(task<T> &&t) noexcept -> bool;

    // replaces the handler for exceptions escaping detached tasks; it must not throw
    void set_exception_handler(exception_handler handler);
};

} // namespace llco

#include "engine.impl.hpp"
