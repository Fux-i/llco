#pragma once

#include "config.hpp"
#include "engine.decl.hpp"

#if LLCO_TRACE
#include "trace.decl.hpp"
#endif

#include <cstdio>
#include <print>
#include <utility>

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

inline auto engine::try_add(handle_type handle) noexcept -> bool {
    return m_queue.try_push(handle);
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
