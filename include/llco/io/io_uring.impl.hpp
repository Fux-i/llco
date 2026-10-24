#pragma once

#include <llco/io/io_uring.decl.hpp>

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
        auto     *op     = static_cast<rw_awaiter *>(::io_uring_cqe_get_data(cqe));
        ::io_uring_cqe_seen(&m_ring, cqe); // before resuming: the frame may be gone afterwards
        if (op != nullptr)
            op->complete(result);
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

inline rw_awaiter::rw_awaiter(ring &owner, int fd, std::span<std::byte> buffer,
                              std::uint64_t offset, bool write) noexcept
    : m_ring{&owner}, m_buffer{buffer.data()}, m_len{static_cast<unsigned>(buffer.size())},
      m_offset{offset}, m_fd{fd}, m_write{write} {}

inline auto rw_awaiter::await_ready() const noexcept -> bool {
    return false;
}

inline void rw_awaiter::await_suspend(std::coroutine_handle<> handle) {
    m_handle = handle;
    auto *sqe = m_ring->sqe();
    // A full queue hands the batch over early; the loop still submits what is left.
    while (sqe == nullptr) {
        m_ring->submit();
        sqe = m_ring->sqe();
    }
    if (m_write)
        ::io_uring_prep_write(sqe, m_fd, m_buffer, m_len, m_offset);
    else
        ::io_uring_prep_read(sqe, m_fd, m_buffer, m_len, m_offset);
    ::io_uring_sqe_set_data(sqe, this);
    m_ring->m_running_io.fetch_add(1, std::memory_order_relaxed);
}

inline auto rw_awaiter::await_resume() const -> std::size_t {
    if (m_result < 0)
        throw std::system_error{-m_result, std::system_category(), "io_uring op"};
    return static_cast<std::size_t>(m_result);
}

inline void rw_awaiter::complete(int result) noexcept {
    m_result = result;
    m_ring->m_running_io.fetch_sub(1, std::memory_order_relaxed);
    m_handle.resume();
}

inline auto read(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset)
    -> rw_awaiter {
    return {owner, fd, buffer, offset, false};
}

inline auto write(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset)
    -> rw_awaiter {
    return {owner, fd, buffer, offset, true};
}

inline auto to_kernel_time(std::chrono::nanoseconds timeout) noexcept -> __kernel_timespec {
    const auto secs  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs);
    return __kernel_timespec{.tv_sec = secs.count(), .tv_nsec = nanos.count()};
}

} // namespace llco::io
