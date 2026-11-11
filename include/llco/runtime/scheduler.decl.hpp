#pragma once

#include <llco/attr.hpp>
#include <llco/runtime/engine.decl.hpp>

#include <atomic>
#include <memory>
#include <span>
#include <mutex>
#include <thread>
#include <vector>

namespace llco {

// The process-wide scheduler: N engines and the round-robin placement policy (ADR-0010).
// Nothing runs until init() or the first use of a never-initialized scheduler.
class scheduler {
  private:
    std::vector<std::unique_ptr<engine>> m_engines;
    std::atomic<unsigned>                m_next{0};
    std::atomic<bool>                    m_running{false};
    std::atomic<bool>                    m_started{false};
    std::once_flag                       m_default_pool;

    scheduler() = default;
    ~scheduler();
    CLASS_NO_COPY_MOVE(scheduler);

    static auto instance() -> scheduler &;

    void start(unsigned count, std::span<const unsigned> cpus);
    void halt();

    // Builds the default pool for a scheduler nobody initialized, once even under a first-use race.
    void ensure_started();

  public:
    // (Re)builds the pool, stopping whatever was already running; count 0 means a single engine.
    // cpus pins engine i to cpus[i % size], which isolated cores need (ADR-0013); empty leaves
    // every engine unpinned and the scheduler hands placement to the kernel.
    static void init(unsigned count = std::thread::hardware_concurrency(),
                     std::span<const unsigned> cpus = {});

    // Stops every engine, each draining what was already queued; submit afterwards returns false.
    // The engines stay readable for their counters until the next init().
    static void stop();

    // engines in the pool; also after stop()
    static auto size() -> unsigned;

    // index must be below size(); tests and demos reach the rings here
    static auto engine_at(unsigned index) -> engine &;

    // picks an engine and hands the frame over; false when stopped or every queue is full
    template <class T>
    [[LLCO_DISCARD_HINT]]
    static auto submit(task<T> &&t) -> bool;

    // pins the frame to one engine, without the round-robin lap; false when stopped or that
    // engine is full, so index must be below size()
    template <class T>
    [[LLCO_DISCARD_HINT]]
    static auto submit_at(unsigned index, task<T> &&t) -> bool;
};

} // namespace llco

#include <llco/runtime/scheduler.impl.hpp>