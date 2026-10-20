#pragma once

#include <concepts>
#include <coroutine>

namespace llco {

// Minimal hand-off target for resume_on: takes a handle, reports whether it was accepted.
template <class E>
concept Executor = requires(E &e, std::coroutine_handle<> handle) {
    { e.try_add(handle) } -> std::same_as<bool>;
};

} // namespace llco
