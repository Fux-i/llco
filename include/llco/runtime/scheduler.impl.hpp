#pragma once

#include <llco/runtime/scheduler.decl.hpp>

#include <cassert>
#include <utility>

namespace llco {

inline auto scheduler::instance() -> scheduler & {
    static scheduler sc;
    return sc;
}

inline scheduler::~scheduler() {
    halt();
}

inline void scheduler::start(unsigned count) {
    halt();
    m_started.store(true, std::memory_order_release);
    if (count == 0)
        count = 1;
    m_engines.clear();
    m_next    = 0;
    m_running = true;
    m_engines.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        m_engines.push_back(std::make_unique<engine>());
#if LLCO_TRACE
        m_engines.back()->set_lane("engine-" + std::to_string(i));
#endif
        m_engines.back()->start();
    }
}

inline void scheduler::halt() {
    // stop() on a scheduler nobody started must not be undone by a later lazy start
    m_started.store(true, std::memory_order_release);
    m_running = false;
    for (auto &e : m_engines)
        e->stop();
}

inline void scheduler::ensure_started() {
    if (m_started.load(std::memory_order_acquire))
        return;
    std::call_once(m_default_pool, [this] { start(std::thread::hardware_concurrency()); });
}

inline void scheduler::init(unsigned count) {
    instance().start(count);
}

inline void scheduler::stop() {
    instance().halt();
}

inline auto scheduler::size() -> unsigned {
    auto &s = instance();
    s.ensure_started();
    return static_cast<unsigned>(s.m_engines.size());
}

inline auto scheduler::engine_at(unsigned index) -> engine & {
    auto &s = instance();
    s.ensure_started();
    assert(index < s.m_engines.size());
    return *s.m_engines[index];
}

template <class T>
auto scheduler::submit(task<T> &&t) -> bool {
    auto &s = instance();
    s.ensure_started();
    if (!s.m_running) {
        t.release().destroy();
        return false;
    }
    const auto count  = static_cast<unsigned>(s.m_engines.size());
    auto       handle = t.release();
    const auto first  = s.m_next.fetch_add(1, std::memory_order_relaxed) % count;
    // One frame, one lap: a full engine passes it on (ADR-0004).
    for (unsigned lap = 0; lap < count; ++lap) {
        if (s.m_engines[(first + lap) % count]->try_adopt(handle))
            return true;
    }
    handle.destroy();
    return false;
}

template <class T>
auto scheduler::submit_at(unsigned index, task<T> &&t) -> bool {
    auto &s = instance();
    s.ensure_started();
    if (!s.m_running) {
        t.release().destroy();
        return false;
    }
    auto handle = t.release();
    if (s.m_engines[index]->try_adopt(handle))
        return true;
    handle.destroy();
    return false;
}

} // namespace llco