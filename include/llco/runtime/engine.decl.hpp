#pragma once

#include <llco/attr.hpp>
#include <llco/core/task.hpp>
#include <llco/io/io_uring.decl.hpp>
#include <llco/runtime/queue.decl.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <thread>

#if LLCO_TRACE
#include <string>
#include <string_view>
#endif

namespace llco {

// last resort for exceptions escaping detached tasks
inline void default_exception_handler(std::exception_ptr e);

// Single-threaded context; manual (caller-driven) until start() runs a loop thread.
class engine {
  private:
    queue<std::coroutine_handle<>>          m_queue;
    std::function<void(std::exception_ptr)> m_exception_handler = default_exception_handler;
    std::size_t                             m_loops{};
    std::atomic<std::size_t>                m_wakeups;
    std::jthread                            m_thread;
    std::optional<io::ring>                 m_ring;
    std::atomic<int>                        m_event_fd{-1};
    std::atomic<bool>                       m_sleeping{false};
#if LLCO_TRACE
    std::string m_lane;
#endif

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

    // doorbell writes so far; read it after stop()
    auto wakeup_count() const noexcept -> std::size_t;

    // handles queued but not yet resumed; a sample, not a precise load
    auto load() const noexcept -> unsigned;

    // created on the first start(), reused across restarts; liburing is not thread-safe, so touch
    // it from the engine thread only
    auto ring() -> io::ring &;

#if LLCO_TRACE
    // names the loop thread's lane in the trace; call it before start()
    void set_lane(std::string_view lane);
#endif

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

    // hands a released frame over; false leaves ownership with the caller (unlike spawn)
    template <class P>
    [[LLCO_DISCARD_HINT]]
    auto try_adopt(std::coroutine_handle<P> handle) noexcept -> bool;

    // replaces the handler for exceptions escaping detached tasks; it must not throw
    void set_exception_handler(exception_handler handler);
};

// the engine whose loop is running this thread; nullptr on manual engines and caller threads
auto current_engine() noexcept -> engine *;

} // namespace llco

#include <llco/runtime/engine.impl.hpp>
