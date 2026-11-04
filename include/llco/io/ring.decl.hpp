#pragma once

#include <llco/attr.hpp>
#include <llco/config.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>

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

    // Ops outstanding: prepared or submitted, not completed yet; readable from any thread.
    auto running_io() const noexcept -> unsigned;

    friend class completion;
};

// What reap() hands a kernel result to: the frame to resume and the result itself.
class completion {
  protected:
    std::coroutine_handle<> m_handle = nullptr;
    int                     m_result{};

    // Counts the op on its ring and remembers the frame; reap() does the reverse.
    void track(ring &owner, std::coroutine_handle<> caller) noexcept;

    friend class ring;
};

} // namespace llco::io

#include <llco/io/ring.impl.hpp>
