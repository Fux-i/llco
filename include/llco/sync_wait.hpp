#pragma once

#include "task.hpp"

#include <semaphore>

namespace llco {

// blocks the calling thread until the task finishes, then returns its value
template <class T>
auto sync_wait(task<T> &&t) -> T {
    std::binary_semaphore done{0};
    t.handle().promise().set_completion(done);
    t.resume(); // start it here, let it hop wherever it needs to
    done.acquire();
    return t.handle().promise().result();
}

} // namespace llco
