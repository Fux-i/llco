#pragma once

#include <llco/config.hpp>
#include <llco/runtime/engine.decl.hpp>

#if LLCO_TRACE
#include <llco/trace/trace.decl.hpp>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <print>
#include <system_error>
#include <thread>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace llco {

namespace detail {

// Set for the whole loop thread; sync primitives use it to post wakeups back to their engine.
inline auto engine_slot() noexcept -> engine *& {
    static thread_local engine *slot = nullptr;
    return slot;
}

// Gives the calling thread an engine slot for the duration of a scope.
class slot_guard {
  private:
    engine *m_previous;

  public:
    explicit slot_guard(engine *current) noexcept : m_previous{engine_slot()} {
        engine_slot() = current;
    }
    ~slot_guard() { engine_slot() = m_previous; }
    CLASS_NO_COPY_MOVE(slot_guard);
};

} // namespace detail

inline auto current_engine() noexcept -> engine * {
    return detail::engine_slot();
}

inline void default_exception_handler(std::exception_ptr e) {
    try {
        std::rethrow_exception(std::move(e));
    } catch (const std::exception &ex) {
        std::println(stderr, "llco: unhandled exception in detached task: {}", ex.what());
    } catch (...) {
        std::println(stderr, "llco: unhandled exception in detached task");
    }
    std::terminate();
}

inline engine::~engine() {
    stop();
}

inline void engine::start() {
    if (!m_ring)
        m_ring.emplace();
    m_event_fd = ::eventfd(0, EFD_CLOEXEC);
    if (m_event_fd < 0)
        throw std::system_error{errno, std::system_category(), "eventfd"};
    ring().register_eventfd(m_event_fd);
    m_thread = std::jthread{[this](const std::stop_token &token) { loop(token); }};
}

inline void engine::request_stop() {
    if (m_thread.joinable())
        m_thread.request_stop();
}

inline void engine::set_before_idle(std::function<void()> hook) {
    m_before_idle = std::move(hook);
}

inline void engine::stop() {
    if (m_thread.joinable()) {
        request_stop();
        m_thread.join(); // the loop runs its own ops out before it returns
        // Tasks pushed after the loop's last look still have to run; their ops are submitted by
        // this thread, which stays alive until they complete. They reach current_engine() too, so
        // the drain runs with the slot the loop thread used to hold.
        const detail::slot_guard slot{this};
        for (;;) {
            (void)drain();
            ring().submit();
            if (ring().running_io() == 0 && m_queue.empty())
                break;
            if (ring().reap() == 0)
                wait();
        }
    }
    if (m_event_fd >= 0) {
        ring().unregister_eventfd(); // the ring outlives this start/stop cycle
        ::close(m_event_fd);
        m_event_fd = -1;
    }
}

inline auto engine::loop_count() const noexcept -> std::size_t {
    return m_loops;
}

inline auto engine::wakeup_count() const noexcept -> std::size_t {
    return m_wakeups.load(std::memory_order_relaxed);
}

inline auto engine::load() const noexcept -> unsigned {
    return m_queue.size();
}

inline auto engine::ring() -> io::ring & {
    return *m_ring; // NOLINT(bugprone-unchecked-optional-access): start() fills it
}

#if LLCO_TRACE
inline void engine::set_lane(std::string_view lane) {
    m_lane = lane;
}
#endif

inline void engine::notify() noexcept {
    const int fd = m_event_fd.load(std::memory_order_relaxed);
    if (fd < 0)
        return;
    // A running loop will see the work itself; only a blocked one needs the ring.
    if (!m_idle.exchange(false))
        return;
    m_wakeups.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t one = 1;
    [[maybe_unused]]
    auto written = ::write(fd, &one, sizeof one); // failing means it is already running
}

inline void engine::wait() noexcept {
    const int     fd = m_event_fd.load(std::memory_order_relaxed);
    std::uint64_t counter{};
    while (::read(fd, &counter, sizeof counter) < 0 && errno == EINTR) {
    }
}

inline void engine::loop(const std::stop_token &token) {
    detail::engine_slot() = this;
#if LLCO_TRACE
    if (!m_lane.empty())
        trace::set_thread_name(m_lane);
#endif
    const std::stop_callback wake{token, [this] { notify(); }};
    for (;;) {
        ++m_loops;
        (void)drain();
        ring().submit(); // one enter per batch; nothing pending costs no syscall
        {
#if LLCO_TRACE
            const trace::scope slice{"reap"}; // one slice per completion batch
#endif
            // completions that landed inside submit(), or since the last wakeup
            (void)ring().reap();
        }
        // Stop only once the kernel holds nothing of ours: the ops were submitted by this
        // thread, and it gets them cancelled the moment it exits.
        if (token.stop_requested() && m_queue.empty() && ring().running_io() == 0)
            return;
        if (m_before_idle)
            m_before_idle();
        // Mark the loop idle first, then look: a push that landed before this look is still
        // in the queue, and one that lands after it wakes the loop. Reversing these two is a
        // lost wakeup. The stop token needs the same look: its callback went through the idle
        // check too and may have decided not to wake the loop, which then sleeps forever.
        m_idle = true;
        if (m_queue.empty() && ring().pending() == 0 && ring().sq_pending() == 0 &&
            !token.stop_requested())
            wait();
        m_idle = false;
    }
}

inline auto engine::try_add(handle_type handle) noexcept -> bool {
    if (!m_queue.try_push(handle))
        return false;
    notify();
    return true;
}

inline auto engine::run_once() -> bool {
    handle_type handle;
    if (!m_queue.try_pop(handle))
        return false;
#if LLCO_TRACE
    const trace::scope slice{"resume"};
#endif
    handle.resume();
    return true;
}

inline auto engine::drain() -> std::size_t {
    std::size_t count = 0;
    while (run_once())
        ++count;
    return count;
}

template <class P>
auto engine::try_adopt(std::coroutine_handle<P> handle) noexcept -> bool {
    handle.promise().set_detached(m_exception_handler);
    return try_add(handle);
}

template <class T>
auto engine::spawn(task<T> &&t) noexcept -> bool {
    auto handle = t.release();
    if (try_adopt(handle))
        return true;
    handle.destroy();
    return false;
}

inline void engine::set_exception_handler(exception_handler handler) {
    m_exception_handler = std::move(handler);
}

} // namespace llco