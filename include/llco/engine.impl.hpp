#pragma once

#include "config.hpp"
#include "engine.decl.hpp"

#if LLCO_TRACE
#include "trace.decl.hpp"
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

inline void engine::stop() {
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
        (void)drain(); // the loop may stop before it sees the last pushes; run them here
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

inline auto engine::ring() -> io::ring & {
    return *m_ring; // NOLINT(bugprone-unchecked-optional-access): start() fills it
}

inline void engine::notify() noexcept {
    const int fd = m_event_fd.load(std::memory_order_relaxed);
    if (fd < 0)
        return;
    const std::uint64_t one = 1;
    [[maybe_unused]]
    auto written = ::write(fd, &one, sizeof one); // failing means it is awake
}

inline void engine::wait() noexcept {
    const int     fd = m_event_fd.load(std::memory_order_relaxed);
    std::uint64_t counter{};
    while (::read(fd, &counter, sizeof counter) < 0 && errno == EINTR) {
    }
}

inline void engine::loop(const std::stop_token &token) {
    const std::stop_callback wake{token, [this] { notify(); }};
    for (;;) {
        ++m_loops;
        (void)drain();
        (void)ring().reap(); // peek after the work: an op that finished synchronously never rings
        if (token.stop_requested())
            return;
        wait();
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

template <class T>
auto engine::spawn(task<T> &&t) noexcept -> bool {
    auto handle = t.release();
    handle.promise().set_detached(m_exception_handler);
    if (!try_add(handle)) {
        handle.destroy();
        return false;
    }
    return true;
}

inline void engine::set_exception_handler(exception_handler handler) {
    m_exception_handler = std::move(handler);
}

} // namespace llco