#pragma once

#include <llco/attr.hpp>
#include <llco/config.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>

#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace llco::io {

// One io_uring instance, confined to the engine thread that created it (liburing is not
// thread-safe).
class ring {
  private:
    ::io_uring               m_ring{};
    std::atomic<std::size_t> m_completions;
    std::atomic<unsigned>    m_running_io{0};

  public:
    explicit ring(unsigned entries = RING_ENTRIES);
    ~ring();
    CLASS_NO_COPY_MOVE(ring);

    // The kernel writes efd whenever it posts completions, so it shares the engine's blocking
    // point.
    void register_eventfd(int efd);

    // A ring takes one registration at a time; release it before the fd closes.
    void unregister_eventfd();

    // Prepares one SQE, or nullptr when the queue is full; nothing reaches the kernel before
    // submit(). Raw ops must clear user_data themselves (set_data(sqe, nullptr)).
    [[LLCO_DISCARD_HINT]]
    auto sqe() -> ::io_uring_sqe *;

    // A slot for one op, handing the current batch over when the queue is full.
    auto next_sqe() -> ::io_uring_sqe *;

    void submit();

    // Completions already waiting in the CQ; read-only, unlike reap().
    [[LLCO_DISCARD_HINT]]
    auto pending() const noexcept -> unsigned;

    // SQEs filled but not handed to the kernel yet; blocking while this is non-zero leaves
    // them unsubmitted.
    [[LLCO_DISCARD_HINT]]
    auto sq_pending() const noexcept -> unsigned;

    // Dispatches every completion to its awaiter (user_data == nullptr means a raw op) and hands
    // it back to the kernel; returns how many were seen.
    auto reap() -> std::size_t;

    // Completions reaped so far; readable from any thread.
    auto completions() const noexcept -> std::size_t;

    // Ops armed but not completed yet, submitted or not; readable from any thread.
    auto running_io() const noexcept -> unsigned;

    friend class completion;
};

// What reap() hands a kernel result to: the frame to resume and the result itself.
class completion {
  protected:
    std::coroutine_handle<> m_handle = nullptr;
    int                     m_result{};

    // Counts the op on its ring and remembers the frame; reap() does the reverse.
    void armed(ring &owner, std::coroutine_handle<> caller) noexcept;

    friend class ring;
};

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

#include <llco/io/io_uring.impl.hpp>