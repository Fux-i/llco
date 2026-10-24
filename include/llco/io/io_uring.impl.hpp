#pragma once

#include <llco/io/io_uring.decl.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <system_error>
#include <utility>

#include <sys/socket.h>

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

inline rw_awaiter::rw_awaiter(ring &owner, int fd, std::span<std::byte> buffer,
                              std::uint64_t offset, kind which) noexcept
    : m_ring{&owner}, m_buffer{buffer.data()}, m_len{static_cast<unsigned>(buffer.size())},
      m_offset{offset}, m_fd{fd}, m_kind{which} {}

inline auto rw_awaiter::await_ready() const noexcept -> bool {
    return false;
}

inline void completion::armed(ring &owner, std::coroutine_handle<> caller) noexcept {
    m_handle = caller;
    owner.m_running_io.fetch_add(1, std::memory_order_relaxed);
}

inline void rw_awaiter::await_suspend(std::coroutine_handle<> handle) {
    auto *sqe = m_ring->next_sqe();
    switch (m_kind) {
    case kind::read:
        ::io_uring_prep_read(sqe, m_fd, m_buffer, m_len, m_offset);
        break;
    case kind::write:
        ::io_uring_prep_write(sqe, m_fd, m_buffer, m_len, m_offset);
        break;
    case kind::recv:
        ::io_uring_prep_recv(sqe, m_fd, m_buffer, m_len, 0);
        break;
    case kind::send:
        ::io_uring_prep_send(sqe, m_fd, m_buffer, m_len, MSG_NOSIGNAL);
        break;
    }
    ::io_uring_sqe_set_data(sqe, static_cast<completion *>(this));
    armed(*m_ring, handle);
}

inline auto rw_awaiter::await_resume() const -> std::size_t {
    if (m_result < 0)
        throw std::system_error{-m_result, std::system_category(), "io_uring op"};
    return static_cast<std::size_t>(m_result);
}

inline auto read(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset)
    -> rw_awaiter {
    return {owner, fd, buffer, offset, rw_awaiter::kind::read};
}

inline auto write(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset)
    -> rw_awaiter {
    return {owner, fd, buffer, offset, rw_awaiter::kind::write};
}

inline auto recv(ring &owner, int fd, std::span<std::byte> buffer) -> rw_awaiter {
    return {owner, fd, buffer, 0, rw_awaiter::kind::recv};
}

inline auto send(ring &owner, int fd, std::span<std::byte> buffer) -> rw_awaiter {
    return {owner, fd, buffer, 0, rw_awaiter::kind::send};
}

inline accept_awaiter::accept_awaiter(ring &owner, int listener) noexcept
    : m_ring{&owner}, m_listener{listener} {}

inline auto accept_awaiter::await_ready() const noexcept -> bool {
    return false;
}

inline void accept_awaiter::await_suspend(std::coroutine_handle<> handle) {
    auto *sqe = m_ring->next_sqe();
    ::io_uring_prep_accept(sqe, m_listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    ::io_uring_sqe_set_data(sqe, static_cast<completion *>(this));
    armed(*m_ring, handle);
}

inline auto accept_awaiter::await_resume() const -> int {
    if (m_result < 0)
        throw std::system_error{-m_result, std::system_category(), "io_uring accept"};
    return m_result;
}

inline connect_awaiter::connect_awaiter(ring &owner, int fd, const ::sockaddr_in &address) noexcept
    : m_ring{&owner}, m_fd{fd}, m_address{&address} {}

inline auto connect_awaiter::await_ready() const noexcept -> bool {
    return false;
}

inline void connect_awaiter::await_suspend(std::coroutine_handle<> handle) {
    auto *sqe = m_ring->next_sqe();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_in is a sockaddr
    ::io_uring_prep_connect(sqe, m_fd, reinterpret_cast<const ::sockaddr *>(m_address),
                            sizeof *m_address);
    ::io_uring_sqe_set_data(sqe, static_cast<completion *>(this));
    armed(*m_ring, handle);
}

inline void connect_awaiter::await_resume() const {
    if (m_result < 0)
        throw std::system_error{-m_result, std::system_category(), "io_uring connect"};
}

inline cancel_awaiter::cancel_awaiter(ring &owner, int fd) noexcept : m_ring{&owner}, m_fd{fd} {}

inline auto cancel_awaiter::await_ready() const noexcept -> bool {
    return false;
}

inline void cancel_awaiter::await_suspend(std::coroutine_handle<> handle) {
    auto *sqe = m_ring->next_sqe();
    ::io_uring_prep_cancel_fd(sqe, m_fd, 0);
    ::io_uring_sqe_set_data(sqe, static_cast<completion *>(this));
    armed(*m_ring, handle);
}

inline auto cancel_awaiter::await_resume() const -> bool {
    if (m_result == 0)
        return true;
    if (m_result == -ENOENT)
        return false;
    throw std::system_error{-m_result, std::system_category(), "io_uring cancel"};
}

inline auto accept(ring &owner, int listener) -> accept_awaiter {
    return {owner, listener};
}

inline auto connect(ring &owner, int fd, const ::sockaddr_in &address) -> connect_awaiter {
    return {owner, fd, address};
}

inline auto cancel(ring &owner, int fd) -> cancel_awaiter {
    return {owner, fd};
}

inline auto to_kernel_time(std::chrono::nanoseconds timeout) noexcept -> __kernel_timespec {
    const auto secs  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs);
    return __kernel_timespec{.tv_sec = secs.count(), .tv_nsec = nanos.count()};
}

} // namespace llco::io
