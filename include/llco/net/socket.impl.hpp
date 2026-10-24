#pragma once

#include <llco/net/socket.decl.hpp>

#include <cerrno>
#include <system_error>

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

} // namespace llco::net
