#pragma once

#include <llco/io/ring.decl.hpp>

#include <cstddef>
#include <system_error>

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

inline auto ring::next_sqe() -> ::io_uring_sqe * {
    auto *sqe = ::io_uring_get_sqe(&m_ring);
    while (sqe == nullptr) { // the queue is full: hand the batch over and take a new slot
        submit();
        sqe = ::io_uring_get_sqe(&m_ring);
    }
    return sqe;
}

inline void ring::submit() {
    const int rc = ::io_uring_submit(&m_ring);
    if (rc < 0)
        throw std::system_error{-rc, std::system_category(), "io_uring_submit"};
}

inline auto ring::pending() const noexcept -> unsigned {
    return ::io_uring_cq_ready(&m_ring);
}

inline auto ring::sq_pending() const noexcept -> unsigned {
    return ::io_uring_sq_ready(&m_ring);
}

inline auto ring::reap() -> std::size_t {
    ::io_uring_cqe *cqe  = nullptr;
    std::size_t     seen = 0;
    while (::io_uring_peek_cqe(&m_ring, &cqe) == 0) {
        const int result = cqe->res;
        auto     *op     = static_cast<completion *>(::io_uring_cqe_get_data(cqe));
        ::io_uring_cqe_seen(&m_ring, cqe); // before resuming: the frame may be gone afterwards
        if (op != nullptr) {
            op->m_result = result;
            m_running_io.fetch_sub(1, std::memory_order_relaxed);
            op->m_handle.resume();
        }
        ++seen;
    }
    m_completions.fetch_add(seen, std::memory_order_relaxed);
    return seen;
}

inline auto ring::completions() const noexcept -> std::size_t {
    return m_completions.load(std::memory_order_relaxed);
}

inline auto ring::running_io() const noexcept -> unsigned {
    return m_running_io.load(std::memory_order_relaxed);
}

inline void completion::track(ring &owner, std::coroutine_handle<> caller) noexcept {
    m_handle = caller;
    owner.m_running_io.fetch_add(1, std::memory_order_relaxed);
}

} // namespace llco::io
