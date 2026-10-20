#pragma once

#include "engine.decl.hpp"

namespace llco {

template <class Q>
auto engine<Q>::try_add(handle_type handle) noexcept -> bool {
    return m_queue.try_push(handle);
}

template <class Q>
auto engine<Q>::run_once() -> bool {
    handle_type handle;
    if (!m_queue.try_pop(handle))
        return false;
    handle.resume();
    return true;
}

template <class Q>
auto engine<Q>::drain() -> std::size_t {
    std::size_t count = 0;
    while (run_once())
        ++count;
    return count;
}

} // namespace llco
