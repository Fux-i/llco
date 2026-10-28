#pragma once

#include <llco/net/socket.decl.hpp>

#include <cerrno>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

namespace llco::net {

inline auto loopback(std::uint16_t port) -> ::sockaddr_in {
    return ::sockaddr_in{.sin_family = AF_INET,
                         .sin_port   = htons(port),
                         .sin_addr   = {.s_addr = htonl(INADDR_LOOPBACK)}};
}

inline auto tcp_socket() -> int {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::system_error{errno, std::system_category(), "socket"};
    return fd;
}

inline auto local_port(int fd) -> std::uint16_t {
    ::sockaddr_in address{};
    ::socklen_t   length = sizeof address;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_in is a sockaddr
    if (::getsockname(fd, reinterpret_cast<::sockaddr *>(&address), &length) != 0)
        throw std::system_error{errno, std::system_category(), "getsockname"};
    return ntohs(address.sin_port);
}

inline auto listen_tcp(std::uint16_t port) -> int {
    const int fd = tcp_socket();
    const int reuse = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) != 0) {
        ::close(fd);
        throw std::system_error{errno, std::system_category(), "setsockopt"};
    }
    const auto address = loopback(port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_in is a sockaddr
    if (::bind(fd, reinterpret_cast<const ::sockaddr *>(&address), sizeof address) != 0) {
        ::close(fd);
        throw std::system_error{errno, std::system_category(), "bind"};
    }
    if (::listen(fd, SOMAXCONN) != 0) {
        ::close(fd);
        throw std::system_error{errno, std::system_category(), "listen"};
    }
    return fd;
}

inline connection::connection(io::ring &ring, int fd) noexcept : m_ring{&ring}, m_fd{fd} {}

inline connection::connection(connection &&other) noexcept
    : m_ring{other.m_ring}, m_fd{std::exchange(other.m_fd, -1)} {}

inline connection::~connection() {
    if (m_fd >= 0)
        ::close(m_fd);
}

inline auto connection::recv(std::span<std::byte> buffer) -> io::rw_awaiter {
    return io::recv(*m_ring, m_fd, buffer);
}

inline auto connection::recv(std::span<char> buffer) -> io::rw_awaiter {
    return io::recv(*m_ring, m_fd, buffer);
}

inline auto connection::send(std::span<std::byte> buffer) -> io::rw_awaiter {
    return io::send(*m_ring, m_fd, buffer);
}

inline auto connection::send(std::string_view buffer) -> io::rw_awaiter {
    return io::send(*m_ring, m_fd, buffer);
}

inline void connection::shutdown() const {
    if (m_fd >= 0)
        (void)::shutdown(m_fd, SHUT_WR);
}

inline auto connection::fd() const noexcept -> int {
    return m_fd;
}

inline listener::accepting::accepting(io::ring &ring, int listener_fd) noexcept
    : m_ring{&ring}, m_op{ring, listener_fd} {}

inline auto listener::accepting::await_ready() const noexcept -> bool {
    return m_op.await_ready();
}

inline void listener::accepting::await_suspend(std::coroutine_handle<> handle) {
    m_op.await_suspend(handle);
}

inline auto listener::accepting::await_resume() -> std::expected<connection, std::error_code> {
    auto fd = m_op.await_resume();
    if (!fd)
        return std::unexpected{fd.error()};
    return connection{*m_ring, *fd};
}

inline listener::listener(io::ring &ring, std::uint16_t port) : m_ring{&ring}, m_fd{listen_tcp(port)} {}

inline listener::~listener() {
    if (m_fd >= 0)
        ::close(m_fd);
}

inline auto listener::accept() -> accepting {
    return accepting{*m_ring, m_fd};
}

inline auto listener::accept_fd() -> io::accept_awaiter {
    return io::accept(*m_ring, m_fd);
}

inline auto listener::cancel() -> io::cancel_awaiter {
    return io::cancel(*m_ring, m_fd);
}

inline auto listener::port() const -> std::uint16_t {
    return local_port(m_fd);
}

inline auto listener::fd() const noexcept -> int {
    return m_fd;
}

} // namespace llco::net
