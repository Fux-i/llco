#pragma once

#include <llco/attr.hpp>
#include <llco/runtime/engine.decl.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace llco {

// N engines plus the placement policy: round robin over their queues (ADR-0005).
class scheduler {
  private:
    std::vector<std::unique_ptr<engine>> m_engines;
    std::atomic<unsigned>                m_next{0};

  public:
    // starts every engine; count 0 means a single engine
    explicit scheduler(unsigned count = std::thread::hardware_concurrency());
    ~scheduler();
    CLASS_NO_COPY_MOVE(scheduler);

    // stops every engine, each draining what was already queued; submitting afterwards is a bug
    void stop();

    auto size() const noexcept -> unsigned;

    auto engine_at(unsigned index) -> engine &;

    // picks an engine and hands the frame over; false when every queue is full
    template <class T>
    [[LLCO_DISCARD_HINT]]
    auto submit(task<T> &&t) noexcept -> bool;
};

} // namespace llco

#include <llco/runtime/scheduler.impl.hpp>
