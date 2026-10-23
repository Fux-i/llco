#include "timer.hpp"

#include <llco/llco.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
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

constexpr unsigned    ROUNDS     = 5;
constexpr std::size_t QUICK_ADDS = 512;
constexpr double      MEDIAN     = 0.5;
constexpr double      P10        = 0.1;
constexpr double      P90        = 0.9;

// Iterations per metric; --quick keeps the smoke test under a second.
struct scale {
    unsigned    rounds;
    std::size_t adds;
};

auto parse_scale(std::span<char *const> args) -> scale {
    const bool quick = args.size() > 1 && std::string_view{args[1]} == "--quick";
    return quick ? scale{.rounds = 1, .adds = QUICK_ADDS}
                 : scale{.rounds = ROUNDS, .adds = QUEUE_CAPACITY - 1};
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
    return 0;
}
