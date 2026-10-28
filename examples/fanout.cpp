#include <array>
#include <chrono>
#include <cstddef>
#include <llco/llco.hpp>
#include <print>
#include <span>
#include <thread>

using namespace llco;

namespace {

constexpr std::size_t CHILDREN = 8;
constexpr unsigned    ENGINES  = 8;
constexpr auto        WORK     = std::chrono::milliseconds{20};

task<> child(std::size_t id, std::span<int> results, latch *done) {
    std::this_thread::sleep_for(WORK); // stand-in for real work
    results[id] = static_cast<int>(id * id);
    done->count_down();
    co_return;
}

task<> parent(std::span<int> results, latch *done) {
    for (std::size_t i = 0; i < results.size(); ++i)
        if (!scheduler::submit(child(i, results, done)))
            std::println("[parent] submit {} rejected", i);
    co_await done->wait();
    co_return;
}

} // namespace

int main() {
    scheduler::init(ENGINES);
    std::array<int, CHILDREN> results{};
    latch                     done{CHILDREN};

    const auto begin = std::chrono::steady_clock::now();
    sync_wait(parent(results, &done));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    scheduler::stop();

    int sum = 0;
    for (const int value : results)
        sum += value;

    std::println("[main] {} children x {} ms; fan-out + fan-in took {:.1f} ms (serial: {} ms)",
                 CHILDREN, WORK.count(), std::chrono::duration<double, std::milli>(elapsed).count(),
                 CHILDREN * WORK.count());
    std::print("[main] results:");
    for (const int value : results)
        std::print(" {}", value);
    std::println("; sum = {}", sum);
}
