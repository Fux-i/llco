#pragma once

#include "task.hpp"

namespace llco {

// blocks the calling thread until the task finishes, then returns its value
template <class T>
auto sync_wait(task<T> &&t) -> T;

} // namespace llco

#include "sync_wait.impl.hpp"
