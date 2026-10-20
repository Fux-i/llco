#pragma once

#include "queue.decl.hpp"

namespace llco {

template <class T, unsigned Cap>
auto queue<T, Cap>::try_push(const T &value) noexcept -> bool {
    return m_impl.try_push(value);
}

template <class T, unsigned Cap>
auto queue<T, Cap>::try_pop(T &value) noexcept -> bool {
    return m_impl.try_pop(value);
}

} // namespace llco
