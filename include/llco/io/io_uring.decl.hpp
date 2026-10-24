#pragma once

#include <llco/attr.hpp>
#include <llco/config.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <span>

#include <liburing.h>

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

    // Prepares one SQE; nothing reaches the kernel before submit().
    [[LLCO_DISCARD_HINT]]
    auto sqe() -> ::io_uring_sqe *;

    void submit();

    // Completions already waiting in the CQ; read-only, unlike reap().
    [[LLCO_DISCARD_HINT]]
    auto pending() const noexcept -> unsigned;

    // SQEs filled but not handed to the kernel yet; blocking while this is non-zero strands them.
    [[LLCO_DISCARD_HINT]]
    auto sq_pending() const noexcept -> unsigned;

    // Hands every reported completion back to the kernel; returns how many were seen.
    auto reap() -> std::size_t;

    // Completions reaped so far; readable from any thread.
    auto completions() const noexcept -> std::size_t;

    // Ops armed but not home yet, submitted or not; readable from any thread.
    auto running_io() const noexcept -> unsigned;

    friend class rw_awaiter;
};

// One in-flight kernel op; it lives in the awaiting frame until reap() hands it a result.
class [[LLCO_AWAIT_HINT]] rw_awaiter {
  private:
    ring                   *m_ring;
    std::coroutine_handle<> m_handle{};
    std::byte              *m_buffer;
    unsigned                m_len;
    std::uint64_t           m_offset;
    int                     m_fd;
    bool                    m_write;
    int                     m_result{};

  public:
    rw_awaiter(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset,
               bool write) noexcept;
    CLASS_NO_COPY_MOVE(rw_awaiter);

    auto await_ready() const noexcept -> bool;

    // Arms the op but leaves the submit to the loop, which flushes the batch after drain().
    void await_suspend(std::coroutine_handle<> handle);

    // Bytes transferred; a negative result is rethrown as std::system_error.
    auto await_resume() -> std::size_t;

  private:
    // Runs on the ring thread inside reap(), before the awaiting frame continues.
    void complete(int result) noexcept;

    friend class ring;
};

// Reads into buffer; it must be co_awaited on the ring's owner thread.
auto read(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset = 0) -> rw_awaiter;

// Writes from buffer; it must be co_awaited on the ring's owner thread.
auto write(ring &owner, int fd, std::span<std::byte> buffer, std::uint64_t offset = 0) -> rw_awaiter;

// Deadline in the form a timeout op expects; it must outlive the op.
inline auto to_kernel_time(std::chrono::nanoseconds timeout) noexcept -> __kernel_timespec;

} // namespace llco::io

#include <llco/io/io_uring.impl.hpp>
