#pragma once

#include "sync_wait.decl.hpp"

#include <semaphore>

namespace llco {

template <class T>
auto sync_wait(task<T> &&t) -> T {
    std::binary_semaphore done{0};
    t.handle().promise().set_completion(done);
    t.resume(); // start it here, let it hop wherever it needs to
    done.acquire();
    return t.handle().promise().result();
}

} // namespace llco
