#pragma once

#include <llco/attr.hpp>
#include <llco/io/ring.decl.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>

#include <netinet/in.h>

namespace llco::io {

// One in-flight kernel op; it lives in the awaiting frame until reap() hands it a result.
class [[LLCO_AWAIT_HINT]] rw_awaiter : public completion {
  public:
    enum class Kind : std::uint8_t { Read, Write, Recv, Send };

  private:
    ring         *m_ring;
    std::byte    *m_buffer;
    unsigned      m_len;
    std::uint64_t m_offset;
    int           m_fd;
    Kind          m_kind;

  public:
    rw_awaiter(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset,
               Kind which) noexcept;
    CLASS_NO_COPY_MOVE(rw_awaiter);

    auto await_ready() const noexcept -> bool;

    // Arms the op but leaves the submit to the loop, which flushes the batch after drain().
    void await_suspend(std::coroutine_handle<> handle);

    // Bytes transferred; a kernel error comes back as an error_code, not a throw.
    auto await_resume() const noexcept -> std::expected<std::size_t, std::error_code>;
};

// One accepted connection; await_resume hands back the new socket.
class [[LLCO_AWAIT_HINT]] accept_awaiter : public completion {
  private:
    ring *m_ring;
    int   m_listener;

  public:
    accept_awaiter(ring &owner, int listener) noexcept;
    CLASS_NO_COPY_MOVE(accept_awaiter);

    auto await_ready() const noexcept -> bool;

    // Arms the accept but leaves the submit to the loop, like every other op.
    void await_suspend(std::coroutine_handle<> handle);

    // The new socket; a kernel error comes back as an error_code, not a throw.
    auto await_resume() const noexcept -> std::expected<int, std::error_code>;
};

// One outbound connection; the address must outlive the awaiter.
class [[LLCO_AWAIT_HINT]] connect_awaiter : public completion {
  private:
    ring                *m_ring;
    int                  m_fd;
    const ::sockaddr_in *m_address;

  public:
    connect_awaiter(ring &owner, int fd, const ::sockaddr_in &address) noexcept;
    CLASS_NO_COPY_MOVE(connect_awaiter);

    auto await_ready() const noexcept -> bool;

    // Arms the connect but leaves the submit to the loop.
    void await_suspend(std::coroutine_handle<> handle);

    // Completes once the connection is up; a kernel error comes back as an error_code.
    auto await_resume() const noexcept -> std::expected<void, std::error_code>;
};

// One cancellation request for whatever is in flight on a descriptor.
class [[LLCO_AWAIT_HINT]] cancel_awaiter : public completion {
  private:
    ring *m_ring;
    int   m_fd;

  public:
    cancel_awaiter(ring &owner, int fd) noexcept;
    CLASS_NO_COPY_MOVE(cancel_awaiter);

    auto await_ready() const noexcept -> bool;

    // Arms the cancel but leaves the submit to the loop.
    void await_suspend(std::coroutine_handle<> handle);

    // True when something was cancelled, false when nothing was in flight; check has_value() first.
    auto await_resume() const noexcept -> std::expected<bool, std::error_code>;
};

// Reads into buffer; it must be co_awaited on the ring's owner thread.
auto read(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset = 0) -> rw_awaiter;

// Writes from buffer to a file or pipe. On a socket use send: this one can raise SIGPIPE.
auto write(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset = 0) -> rw_awaiter;

// Reads from a socket; a closed peer reads as zero bytes, not as an error.
auto recv(ring &owner, int fd, std::span<std::byte> buffer) -> rw_awaiter;

// The same read for character buffers, so call sites skip the byte casts.
auto recv(ring &owner, int fd, std::span<char> buffer) -> rw_awaiter;

// Writes to a socket; MSG_NOSIGNAL is always set, so a dead peer is EPIPE and never a signal.
auto send(ring &owner, int fd, std::span<std::byte> buffer) -> rw_awaiter;

// Writes a string to a socket; the kernel only reads it, so the const is cast away inside.
auto send(ring &owner, int fd, std::string_view buffer) -> rw_awaiter;

// Accepts one connection on a listening socket; the new socket is non-blocking, close-on-exec.
auto accept(ring &owner, int listener) -> accept_awaiter;

// Connects to a loopback address; IPv4 only, because the address is a sockaddr_in.
auto connect(ring &owner, int fd, const ::sockaddr_in &address) -> connect_awaiter;

// Cancels every op in flight on fd; the cancelled op itself completes with ECANCELED.
auto cancel(ring &owner, int fd) -> cancel_awaiter;

// Deadline in the form a timeout op expects; it must outlive the op.
inline auto to_kernel_time(std::chrono::nanoseconds timeout) noexcept -> __kernel_timespec;

} // namespace llco::io

#include <llco/io/op.impl.hpp>
