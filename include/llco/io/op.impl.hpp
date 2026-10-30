#pragma once

#include <llco/io/op.decl.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <system_error>

#include <sys/socket.h>

namespace llco::io {

inline rw_awaiter::rw_awaiter(ring &owner, int fd, std::span<std::byte> buffer,
                              std::uint64_t offset, Kind which) noexcept
    : m_ring{&owner}, m_buffer{buffer.data()}, m_len{static_cast<unsigned>(buffer.size())},
      m_offset{offset}, m_fd{fd}, m_kind{which} {}

inline auto rw_awaiter::await_ready() const noexcept -> bool {
    return false;
}

inline void rw_awaiter::await_suspend(std::coroutine_handle<> handle) {
    auto *sqe = m_ring->next_sqe();
    switch (m_kind) {
    case Kind::Read:
        ::io_uring_prep_read(sqe, m_fd, m_buffer, m_len, m_offset);
        break;
    case Kind::Write:
        ::io_uring_prep_write(sqe, m_fd, m_buffer, m_len, m_offset);
        break;
    case Kind::Recv:
        ::io_uring_prep_recv(sqe, m_fd, m_buffer, m_len, 0);
        break;
    case Kind::Send:
        ::io_uring_prep_send(sqe, m_fd, m_buffer, m_len, MSG_NOSIGNAL);
        break;
    }
    ::io_uring_sqe_set_data(sqe, static_cast<completion *>(this));
    armed(*m_ring, handle);
}

inline auto rw_awaiter::await_resume() const noexcept -> std::expected<std::size_t, std::error_code> {
    if (m_result < 0)
        return std::unexpected{std::error_code{-m_result, std::system_category()}};
    return static_cast<std::size_t>(m_result);
}

inline auto read(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset)
    -> rw_awaiter {
    return {owner, fd, buffer, offset, rw_awaiter::Kind::Read};
}

inline auto write(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset)
    -> rw_awaiter {
    return {owner, fd, buffer, offset, rw_awaiter::Kind::Write};
}

inline auto recv(ring &owner, int fd, std::span<std::byte> buffer) -> rw_awaiter {
    return {owner, fd, buffer, 0, rw_awaiter::Kind::Recv};
}

inline auto send(ring &owner, int fd, std::span<std::byte> buffer) -> rw_awaiter {
    return {owner, fd, buffer, 0, rw_awaiter::Kind::Send};
}

inline auto recv(ring &owner, int fd, std::span<char> buffer) -> rw_awaiter {
    return recv(owner, fd, std::as_writable_bytes(buffer));
}

inline auto send(ring &owner, int fd, std::string_view buffer) -> rw_awaiter {
    const auto *raw = static_cast<const void *>(buffer.data());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the kernel only reads for send
    auto *bytes = const_cast<std::byte *>(static_cast<const std::byte *>(raw));
    return send(owner, fd, std::span{bytes, buffer.size()});
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

inline auto accept_awaiter::await_resume() const noexcept -> std::expected<int, std::error_code> {
    if (m_result < 0)
        return std::unexpected{std::error_code{-m_result, std::system_category()}};
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

inline auto connect_awaiter::await_resume() const noexcept -> std::expected<void, std::error_code> {
    if (m_result < 0)
        return std::unexpected{std::error_code{-m_result, std::system_category()}};
    return {};
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

inline auto cancel_awaiter::await_resume() const noexcept -> std::expected<bool, std::error_code> {
    if (m_result == -ENOENT)
        return false;
    if (m_result < 0)
        return std::unexpected{std::error_code{-m_result, std::system_category()}};
    return true;
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
