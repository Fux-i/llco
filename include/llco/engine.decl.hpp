#pragma once

#include "queue.decl.hpp"

#include <coroutine>
#include <cstddef>

namespace llco {

// Single-threaded context; manual mode means the caller drives it.
template <class Q = queue<std::coroutine_handle<>>>
class engine {
  private:
    Q m_queue;

  public:
    using handle_type = std::coroutine_handle<>;

    // false when full (ADR-0004)
    [[nodiscard]]
    auto try_add(handle_type handle) noexcept -> bool;

    // false when empty
    auto run_once() -> bool;

    // returns how many handles were resumed
    [[nodiscard]]
    auto drain() -> std::size_t;
};

} // namespace llco

#include "engine.impl.hpp"
