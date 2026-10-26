#pragma once

#include <llco/io/io_uring.decl.hpp>

#include <coroutine>
#include <cstdint>
#include <span>
#include <string_view>

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

// One accepted connection; the destructor closes it.
class connection {
  private:
    io::ring *m_ring;
    int       m_fd;

  public:
    connection(io::ring &ring, int fd) noexcept;
    connection(connection &&other) noexcept;
    ~connection();
    connection(const connection &)                     = delete;
    auto operator=(const connection &) -> connection & = delete;
    auto operator=(connection &&) -> connection &      = delete;

    // Waits for what the peer sent; zero bytes mean it stopped writing.
    auto recv(std::span<std::byte> buffer) -> io::rw_awaiter;
    auto recv(std::span<char> buffer) -> io::rw_awaiter;

    // Writes it all out; MSG_NOSIGNAL, so a dead peer is EPIPE and never a signal.
    auto send(std::span<std::byte> buffer) -> io::rw_awaiter;
    auto send(std::string_view buffer) -> io::rw_awaiter;

    // Half-closes: tells the peer we are done writing.
    void shutdown() const;

    auto fd() const noexcept -> int;
};

// One listening socket; the destructor closes it.
class listener {
  public:
    // await_resume hands back a whole connection, not a raw fd.
    class [[LLCO_AWAIT_HINT]] accepting {
      private:
        io::ring          *m_ring;
        io::accept_awaiter m_op;

      public:
        accepting(io::ring &ring, int listener_fd) noexcept;
        auto await_ready() const noexcept -> bool;
        void await_suspend(std::coroutine_handle<> handle);
        auto await_resume() -> connection;
    };

  private:
    io::ring *m_ring;
    int       m_fd;

  public:
    // Binds 127.0.0.1:port (0 asks the kernel) and listens.
    explicit listener(io::ring &ring, std::uint16_t port = 0);
    ~listener();
    CLASS_NO_COPY_MOVE(listener);

    // Waits for one connection; the new socket is owned by the result.
    auto accept() -> accepting;

    // Cancels a pending accept, which completes with ECANCELED.
    auto cancel() -> io::cancel_awaiter;

    auto port() const -> std::uint16_t;
    auto fd() const noexcept -> int;
};

} // namespace llco::net

#include <llco/net/socket.impl.hpp>
