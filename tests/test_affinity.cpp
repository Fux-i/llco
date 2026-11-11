#include <gtest/gtest.h>
#include <llco/core/task.hpp>
#include <llco/runtime/scheduler.decl.hpp>

#include <array>
#include <atomic>
#include <coroutine>
#include <sched.h>
#include <thread>
#include <vector>

using namespace llco;

namespace {

// Reports the core the engine thread is running on.
task<> report_cpu(std::atomic<int> *slot) {
    slot->store(::sched_getcpu());
    co_return;
}

// The cores this process may run on; empty when the mask is unreadable.
auto allowed_cpus() -> std::vector<unsigned> {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof set, &set) != 0)
        return {};
    std::vector<unsigned> cpus;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set))
            cpus.push_back(cpu);
    return cpus;
}

// Runs one task on engine 0 and waits for the engine to record its core.
auto core_of_engine_zero(unsigned pinned_to) -> int {
    scheduler::init(1, std::span{&pinned_to, 1});
    std::atomic<int> seen{-1};
    (void)scheduler::submit(report_cpu(&seen));
    while (seen.load() < 0)
        std::this_thread::yield();
    scheduler::stop();
    return seen.load();
}

} // namespace

TEST(affinity, an_engine_runs_on_the_core_it_was_pinned_to) {
    const auto cpus = allowed_cpus();
    ASSERT_FALSE(cpus.empty()) << "no readable cpu affinity mask";

    const auto target = cpus.front();
    EXPECT_EQ(core_of_engine_zero(target), static_cast<int>(target));
}

TEST(affinity, engines_pinned_to_different_cores_report_different_cores) {
    const auto cpus = allowed_cpus();
    if (cpus.size() < 2)
        GTEST_SKIP() << "needs two allowed cpus, this process sees " << cpus.size();

    scheduler::init(2, std::span{cpus.data(), std::size_t{2}});
    std::array<std::atomic<int>, 2> seen{};
    for (auto &slot : seen)
        slot.store(-1);
    (void)scheduler::submit_at(0, report_cpu(&seen.at(0)));
    (void)scheduler::submit_at(1, report_cpu(&seen.at(1)));
    while (seen[0].load() < 0 || seen[1].load() < 0)
        std::this_thread::yield();
    scheduler::stop();

    EXPECT_EQ(seen[0].load(), static_cast<int>(cpus[0]));
    EXPECT_EQ(seen[1].load(), static_cast<int>(cpus[1]));
    EXPECT_NE(seen[0].load(), seen[1].load());
}
