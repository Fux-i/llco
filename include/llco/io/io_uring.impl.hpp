#pragma once

#include "io_uring.decl.hpp"

#include <chrono>
#include <cstddef>
#include <system_error>
#include <utility>

namespace llco::io {

inline ring::ring(unsigned entries) {
    const int rc = ::io_uring_queue_init(entries, &m_ring, 0);
    if (rc < 0)
        throw std::system_error{-rc, std::system_category(), "io_uring_queue_init"};
}

inline ring::~ring() {
    ::io_uring_queue_exit(&m_ring);
}

inline void ring::register_eventfd(int efd) {
    const int rc = ::io_uring_register_eventfd(&m_ring, efd);
    if (rc < 0)
        throw std::system_error{-rc, std::system_category(), "io_uring_register_eventfd"};
}

inline void ring::unregister_eventfd() {
    const int rc = ::io_uring_unregister_eventfd(&m_ring);
    if (rc < 0)
        throw std::system_error{-rc, std::system_category(), "io_uring_unregister_eventfd"};
}

inline auto ring::sqe() -> ::io_uring_sqe * {
    return ::io_uring_get_sqe(&m_ring);
}

inline void ring::submit() {
    const int rc = ::io_uring_submit(&m_ring);
    if (rc < 0)
        throw std::system_error{-rc, std::system_category(), "io_uring_submit"};
}

inline auto ring::reap() -> std::size_t {
    ::io_uring_cqe *cqe  = nullptr;
    std::size_t     seen = 0;
    while (::io_uring_peek_cqe(&m_ring, &cqe) == 0) {
        ++seen;
        ::io_uring_cqe_seen(&m_ring, cqe);
    }
    m_completions.fetch_add(seen, std::memory_order_relaxed);
    return seen;
}

inline auto ring::completions() const noexcept -> std::size_t {
    return m_completions.load(std::memory_order_relaxed);
}

inline auto to_kernel_time(std::chrono::nanoseconds timeout) noexcept -> __kernel_timespec {
    const auto secs  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs);
    return __kernel_timespec{.tv_sec = secs.count(), .tv_nsec = nanos.count()};
}

} // namespace llco::io