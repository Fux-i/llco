#pragma once

#include <cstdint>

#include <netinet/in.h>

namespace llco::net {

// Binds and listens on 127.0.0.1; port 0 lets the kernel pick one.
auto listen_tcp(std::uint16_t port) -> int;

// The port a socket is bound to.
auto local_port(int fd) -> std::uint16_t;

// An unconnected loopback socket, non-blocking and close-on-exec.
auto tcp_socket() -> int;

// The loopback address of a listener, for io::connect.
auto loopback(std::uint16_t port) -> ::sockaddr_in;

} // namespace llco::net

#include <llco/net/socket.impl.hpp>
