#pragma once

#include <chrono>
#include <cstdint>
#include <x86intrin.h>

namespace bench {

constexpr double NS_PER_SECOND         = 1e9;
constexpr int    CALIBRATION_WINDOW_MS = 20;

// Invariant TSC: the host reports constant_tsc and nonstop_tsc.
inline auto tsc() noexcept -> std::uint64_t {
    return __rdtsc();
}

// Calibrated against steady_clock once; every run prints the ratio, see docs/bench.md.
inline auto tsc_hz() -> double {
    using namespace std::chrono;
    static const double hz = [] {
        const auto begin_clock = steady_clock::now();
        const auto begin_tsc   = tsc();
        while (steady_clock::now() - begin_clock < milliseconds{CALIBRATION_WINDOW_MS}) {
        }
        const double elapsed = duration<double>(steady_clock::now() - begin_clock).count();
        return static_cast<double>(tsc() - begin_tsc) / elapsed;
    }();
    return hz;
}

// Ticks to nanoseconds; rdtsc costs ~10 ns here, so never per-op on sub-microsecond work.
inline auto to_ns(std::uint64_t ticks) noexcept -> double {
    return static_cast<double>(ticks) * NS_PER_SECOND / tsc_hz();
}

// Cross-thread samples share steady_clock: one clock for every thread, vDSO backed.
inline auto steady_ns() noexcept -> double {
    using namespace std::chrono;
    return duration<double, std::nano>(steady_clock::now().time_since_epoch()).count();
}

} // namespace bench
