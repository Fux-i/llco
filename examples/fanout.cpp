#include <array>
#include <chrono>
#include <llco/config.hpp>
#include <llco/llco.hpp>
#include <llco/trace/trace.decl.hpp>
#include <print>
#include <thread>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr int  CHILDREN = 8;
constexpr auto WORK     = 20ms;

task<> child(int id, std::array<int, CHILDREN> *results, latch *done) {
    const trace::scope slice{"child"};
    std::this_thread::sleep_for(WORK); // stand-in for real work
    results->at(static_cast<std::size_t>(id)) = id * id;
    done->count_down();
    co_return;
}

task<> parent(std::array<int, CHILDREN> *results, latch *done) {
    for (int id = 0; id < CHILDREN; ++id)
        run(child(id, results, done)); // each child lands on the next engine
    co_await done->wait();             // when_all: the latch counts every child down
    co_return;
}

} // namespace

int main() {
#if LLCO_TRACE
    trace::set_thread_name("main");
#endif
    scheduler::init(4);

    std::array<int, CHILDREN> results{};
    latch                     done{CHILDREN};

    const auto begin = std::chrono::steady_clock::now();
    sync_run(parent(&results, &done));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    std::println("[main] {} children x {} ms took {:.0f} ms (serial: {} ms)", CHILDREN,
                 WORK.count(), std::chrono::duration<double, std::milli>(elapsed).count(),
                 CHILDREN * WORK.count());
    std::print("[main] results:");
    for (const int value : results)
        std::print(" {}", value);
    std::println("");

#if LLCO_TRACE
    (void)trace::flush("trace.json");
#endif
}
