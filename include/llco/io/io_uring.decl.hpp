#pragma once

#include "llco/attr.hpp"
#include "llco/config.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>

#include <liburing.h>

namespace llco::io {

// One io_uring instance, confined to the engine thread that created it (liburing is not
// thread-safe).
class ring {
  private:
    ::io_uring               m_ring{};
    std::atomic<std::size_t> m_completions;

  public:
    explicit ring(unsigned entries = RING_ENTRIES);
    ~ring();
    CLASS_NO_COPY_MOVE(ring);

    // The kernel writes efd whenever it posts completions, so it shares the engine's blocking
    // point.
    void register_eventfd(int efd);

    // A ring takes one registration at a time; release it before the fd closes.
    void unregister_eventfd();

    // Prepares one SQE; nothing reaches the kernel before submit().
    [[LLCO_DISCARD_HINT]]
    auto sqe() -> ::io_uring_sqe *;

    void submit();

    // Hands every reported completion back to the kernel; returns how many were seen.
    auto reap() -> std::size_t;

    // Completions reaped so far; readable from any thread.
    auto completions() const noexcept -> std::size_t;
};

// Deadline in the form a timeout op expects; it must outlive the op.
inline auto to_kernel_time(std::chrono::nanoseconds timeout) noexcept -> __kernel_timespec;

} // namespace llco::io

#include "io_uring.impl.hpp"