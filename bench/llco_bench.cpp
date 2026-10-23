#include "timer.hpp"

#include <llco/llco.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace llco;

#ifdef __clang__
constexpr auto COMPILER = "clang " __clang_version__;
#elifdef __GNUC__
constexpr auto COMPILER = "gcc " __VERSION__;
#else
constexpr auto COMPILER = "unknown";
#endif

#ifdef NDEBUG
constexpr auto BUILD = "release";
#else
constexpr auto BUILD = "debug";
#endif

constexpr unsigned    ROUNDS          = 5;
constexpr int         SETTLE_MS       = 1;
constexpr std::size_t QUICK_ADDS      = 512;
constexpr std::size_t QUICK_WAKEUPS   = 20;
constexpr std::size_t QUICK_SPAWNS    = 5'000;
constexpr std::size_t QUICK_WAITS     = 200;
constexpr std::size_t QUICK_HOPS      = 100;
constexpr std::size_t FULL_WAKEUPS    = 200;
constexpr std::size_t FULL_SPAWNS     = 100'000;
constexpr std::size_t FULL_WAITS      = 2'000;
constexpr std::size_t FULL_HOPS       = 1'000;
constexpr double      MEDIAN          = 0.5;
constexpr double      P10             = 0.1;
constexpr double      P90             = 0.9;

// Iterations per metric; --quick keeps the smoke test under a second.
struct scale {
    unsigned    rounds;
    std::size_t adds;
    std::size_t wakeups;
    std::size_t spawns;
    std::size_t waits;
    std::size_t hops;
};

auto parse_scale(std::span<char *const> args) -> scale {
    const bool quick = args.size() > 1 && std::string_view{args[1]} == "--quick";
    if (quick)
        return scale{.rounds  = 1,
                     .adds    = QUICK_ADDS,
                     .wakeups = QUICK_WAKEUPS,
                     .spawns  = QUICK_SPAWNS,
                     .waits   = QUICK_WAITS,
                     .hops    = QUICK_HOPS};
    return scale{.rounds  = ROUNDS,
                 .adds    = QUEUE_CAPACITY - 1,
                 .wakeups = FULL_WAKEUPS,
                 .spawns  = FULL_SPAWNS,
                 .waits   = FULL_WAITS,
                 .hops    = FULL_HOPS};
}

auto cpu_model() -> std::string {
    std::ifstream input{"/proc/cpuinfo"};
    for (std::string line; std::getline(input, line);)
        if (line.starts_with("model name"))
            return line.substr(line.find(": ") + 2);
    return "unknown";
}

struct stats {
    double median;
    double p10;
    double p90;
};

auto percentile(const std::vector<double> &sorted, double fraction) -> double {
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

auto summarize(std::vector<double> samples) -> stats {
    std::ranges::sort(samples);
    return stats{.median = percentile(samples, MEDIAN),
                 .p10    = percentile(samples, P10),
                 .p90    = percentile(samples, P90)};
}

// One row per metric: a per-operation latency in ns plus the derived rate.
void report(std::string_view metric, const std::vector<double> &round_ns, std::size_t measurements) {
    const auto result = summarize(round_ns);
    std::println("{},{},{:.2f},{:.2f},{:.2f},{:.0f}", metric, measurements, result.median,
                 result.p10, result.p90, bench::NS_PER_SECOND / result.median);
}

task<> noop() {
    co_return;
}

// Stamps the moment the engine thread starts running it.
task<> wake_probe(std::atomic<double> *stamp) {
    stamp->store(bench::steady_ns(), std::memory_order_relaxed);
    co_return;
}

task<> counter_probe(std::atomic<std::size_t> *done) {
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// One sample per hop: queue it onto the target, then measure the trip back to a resume.
task<> hop_probe(engine *target, std::span<std::uint64_t> ticks, std::atomic<bool> *done) {
    for (auto &slot : ticks) {
        const auto begin = bench::tsc();
        co_await resume_on(*target);
        slot = bench::tsc() - begin;
    }
    done->store(true, std::memory_order_release);
    co_return;
}

// Queue push only: a manual engine never runs the frames and has no fd to ring.
auto measure_add(const scale &plan) -> std::vector<double> {
    engine                               e;
    std::vector<std::coroutine_handle<>> frames;
    std::vector<double>                  rounds;
    frames.reserve(plan.adds);
    rounds.reserve(plan.rounds);

    for (unsigned round = 0; round < plan.rounds; ++round) {
        frames.clear();
        for (std::size_t i = 0; i < plan.adds; ++i)
            frames.push_back(noop().release());

        const auto begin = bench::tsc();
        for (auto frame : frames)
            (void)e.try_add(frame);
        const auto end = bench::tsc();

        rounds.push_back(bench::to_ns(end - begin) / static_cast<double>(plan.adds));
        (void)e.drain();
        for (auto frame : frames)
            frame.destroy();
    }
    return rounds;
}

// Submit to a sleeping engine; the body stamps itself so the delta covers doorbell and resume.
auto measure_wakeup(const scale &plan) -> std::vector<double> {
    engine e;
    e.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{SETTLE_MS});
    std::vector<double> rounds;
    rounds.reserve(plan.rounds);

    for (unsigned round = 0; round < plan.rounds; ++round) {
        double total = 0.0;
        for (std::size_t i = 0; i < plan.wakeups; ++i) {
            // Every sample must start from a sleeping loop, otherwise it measures the awake path.
            std::this_thread::sleep_for(std::chrono::milliseconds{SETTLE_MS});
            std::atomic<double> stamp{0.0};
            const auto          begin = bench::steady_ns();
            (void)e.spawn(wake_probe(&stamp));
            while (stamp.load(std::memory_order_relaxed) == 0.0) {
            }
            total += stamp.load(std::memory_order_relaxed) - begin;
        }
        rounds.push_back(total / static_cast<double>(plan.wakeups));
    }
    e.stop();
    return rounds;
}

// Submit and run to completion; a full queue retries, which is part of the cost.
auto measure_spawn(const scale &plan) -> std::vector<double> {
    engine                   e;
    std::atomic<std::size_t> done{0};
    std::vector<double>      rounds;
    rounds.reserve(plan.rounds);
    e.start();

    for (unsigned round = 0; round < plan.rounds; ++round) {
        done.store(0, std::memory_order_relaxed);
        const auto begin = bench::tsc();
        for (std::size_t i = 0; i < plan.spawns; ++i) {
            while (!e.spawn(counter_probe(&done))) {
            }
        }
        while (done.load(std::memory_order_acquire) < plan.spawns) {
        }
        const auto end = bench::tsc();
        rounds.push_back(bench::to_ns(end - begin) / static_cast<double>(plan.spawns));
    }
    e.stop();
    return rounds;
}

// Whole round trip on the calling thread: build the frame, resume it, block, read the result.
auto measure_sync_wait(const scale &plan) -> std::vector<double> {
    std::vector<double> rounds;
    rounds.reserve(plan.rounds);

    for (unsigned round = 0; round < plan.rounds; ++round) {
        const auto begin = bench::tsc();
        for (std::size_t i = 0; i < plan.waits; ++i)
            sync_wait(noop());
        const auto end = bench::tsc();
        rounds.push_back(bench::to_ns(end - begin) / static_cast<double>(plan.waits));
    }
    return rounds;
}

// A coroutine hopping back onto its own engine: one queue round trip and one resume per sample.
auto measure_hop(const scale &plan) -> std::vector<double> {
    engine                    e;
    std::vector<std::uint64_t> ticks(plan.hops);
    std::atomic<bool>          done{false};
    std::vector<double>        rounds;
    rounds.reserve(plan.rounds);
    e.start();

    for (unsigned round = 0; round < plan.rounds; ++round) {
        done.store(false, std::memory_order_relaxed);
        (void)e.spawn(hop_probe(&e, ticks, &done));
        while (!done.load(std::memory_order_acquire)) {
        }
        std::uint64_t total = 0;
        for (auto tick : ticks)
            total += tick;
        rounds.push_back(bench::to_ns(total) / static_cast<double>(plan.hops));
    }
    e.stop();
    return rounds;
}

} // namespace

auto main(int argc, char **argv) -> int {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const auto      plan = parse_scale(args);

    std::println("# llco bench: A/B baseline for this host only, do not compare across machines");
    std::println("# cpu={}", cpu_model());
    std::println("# compiler={}", COMPILER);
    std::println("# build={}", BUILD);
    std::println("# tsc_hz={:.0f}", bench::tsc_hz());
    std::println("# rounds={}", plan.rounds);
    std::println("metric,n,median_ns,p10_ns,p90_ns,ops_per_sec");

    const auto add = measure_add(plan);
    report("add_latency", add, static_cast<std::size_t>(plan.rounds) * plan.adds);

    const auto wakeup = measure_wakeup(plan);
    report("wakeup_latency", wakeup, static_cast<std::size_t>(plan.rounds) * plan.wakeups);

    const auto spawn = measure_spawn(plan);
    report("spawn_task", spawn, static_cast<std::size_t>(plan.rounds) * plan.spawns);

    const auto wait = measure_sync_wait(plan);
    report("sync_wait_round_trip", wait, static_cast<std::size_t>(plan.rounds) * plan.waits);

    const auto hop = measure_hop(plan);
    report("hop_cost", hop, static_cast<std::size_t>(plan.rounds) * plan.hops);
    return 0;
}
