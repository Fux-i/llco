#pragma once

#include <llco/attr.hpp>
#include <llco/runtime/engine.decl.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace llco {

// The process-wide scheduler: N engines and the round-robin placement policy (ADR-0010).
class scheduler {
  private:
    std::vector<std::unique_ptr<engine>> m_engines;
    std::atomic<unsigned>                m_next{0};
    bool                                 m_running{false};

    explicit scheduler(unsigned count);
    ~scheduler();
    CLASS_NO_COPY_MOVE(scheduler);

    static auto instance() -> scheduler &;

    void start(unsigned count);
    void halt();

  public:
    // (Re)builds the pool, stopping whatever was already running; count 0 means a single engine.
    static void init(unsigned count = std::thread::hardware_concurrency());

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

    // submits every task; throws std::runtime_error on the first rejection (ADR-0004)
    template <class... T>
    static void run(task<T> &&...tasks);
};

} // namespace llco

#include <llco/runtime/scheduler.impl.hpp>