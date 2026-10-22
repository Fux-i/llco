#pragma once

#include "attr.hpp"
#include "config.hpp"

#include <atomic_queue/atomic_queue.h>
#include <coroutine>

namespace llco {

// AtomicQueue2, not AtomicQueue: the latter marks empty slots with a nil element.
template <class T, unsigned Cap = QUEUE_CAPACITY>
using queue_backend = atomic_queue::AtomicQueue2<T, Cap>;

// Bounded MPMC queue; Cap is rounded up to a power of two by the backend.
template <class T, unsigned Cap = QUEUE_CAPACITY>
class queue {
  private:
    queue_backend<T, Cap> m_impl;

  public:
    [[LLCO_DISCARD_HINT]]
    auto try_push(const T &value) noexcept -> bool;
    [[LLCO_DISCARD_HINT]]
    auto try_pop(T &value) noexcept -> bool;
    // A push still in flight reports empty; its producer rings the bell afterwards.
    [[LLCO_DISCARD_HINT]]
    auto empty() const noexcept -> bool;
    // Elements queued right now; like empty(), a sample rather than a precise count.
    auto size() const noexcept -> unsigned;
};

} // namespace llco

#include "queue.impl.hpp"
