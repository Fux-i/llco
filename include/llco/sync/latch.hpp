#pragma once

#include <llco/attr.hpp>
#include <llco/sync/event.hpp>

#include <atomic>
#include <cstddef>

namespace llco {

// Counts down to zero, then broadcasts once to every waiter. count_down runs at most count times.
class latch {
  private:
    std::atomic<std::size_t> m_count;
    event<void>              m_done;

  public:
    explicit latch(std::size_t count) noexcept : m_count(count) {
        if (count == 0)
            m_done.set();
    }
    CLASS_NO_COPY_MOVE(latch);

    void count_down() noexcept {
        if (m_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
            m_done.set();
    }

    auto wait() -> event<void>::awaiter {
        return m_done.wait();
    }
};

} // namespace llco
