#pragma once

#include "config.hpp"

#include <atomic_queue/atomic_queue.h>

namespace llco {

// AtomicQueue2, not AtomicQueue: the latter marks empty slots with a nil element.
template <class T, unsigned Cap = queue_capacity>
using queue_backend = atomic_queue::AtomicQueue2<T, Cap>;

// Bounded MPMC queue; Cap is rounded up to a power of two by the backend.
template <class T, unsigned Cap = queue_capacity>
class queue {
  private:
    queue_backend<T, Cap> m_impl;

  public:
    [[nodiscard]]
    auto try_push(const T &value) noexcept -> bool;
    [[nodiscard]]
    auto try_pop(T &value) noexcept -> bool;
};

} // namespace llco

#include "queue.impl.hpp"
