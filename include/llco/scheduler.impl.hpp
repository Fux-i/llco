#pragma once

#include "scheduler.decl.hpp"

namespace llco {

inline scheduler::scheduler(unsigned count) {
    if (count == 0)
        count = 1;
    m_engines.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        m_engines.push_back(std::make_unique<engine>());
        m_engines.back()->start();
    }
}

inline scheduler::~scheduler() {
    stop();
}

inline void scheduler::stop() {
    for (auto &e : m_engines)
        e->stop();
}

inline auto scheduler::size() const noexcept -> unsigned {
    return static_cast<unsigned>(m_engines.size());
}

inline auto scheduler::engine_at(unsigned index) -> engine & {
    return *m_engines[index];
}

template <class T>
auto scheduler::submit(task<T> &&t) noexcept -> bool {
    auto       handle = t.release();
    const auto count  = size();
    const auto first  = m_next.fetch_add(1, std::memory_order_relaxed) % count;
    // One frame, one lap: a full engine passes it on (ADR-0004).
    for (unsigned lap = 0; lap < count; ++lap) {
        if (m_engines[(first + lap) % count]->try_adopt(handle))
            return true;
    }
    handle.destroy();
    return false;
}

} // namespace llco
