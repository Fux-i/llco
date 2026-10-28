#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <llco/llco.hpp>
#include <memory>
#include <thread>
#include <vector>

using namespace llco;

namespace {

constexpr int TARGET_VALUE = 7;

task<> work(std::atomic<int> *done) {
    ++*done;
    co_return;
}

task<int> resuming(engine *target) {
    co_await resume_on(*target);
    co_return TARGET_VALUE;
}

} // namespace

int main() {
    constexpr unsigned engine_count = 4;
    constexpr unsigned task_count   = 1000;
    constexpr int      idle_ms      = 200;

    std::vector<std::unique_ptr<engine>> engines; // engine carries an 8192-slot queue
    for (unsigned i = 0; i < engine_count; ++i) {
        engines.push_back(std::make_unique<engine>());
        engines.back()->start();
    }

    const auto cpu_before = std::clock();
    std::this_thread::sleep_for(std::chrono::milliseconds{idle_ms});
    const auto idle_cpu_ms = (std::clock() - cpu_before) * 1000 / CLOCKS_PER_SEC;
    std::cout << "[main] idle " << idle_ms << " ms burned " << idle_cpu_ms << " ms of CPU\n";

    std::atomic<int> done{0};
    const auto       begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < task_count; ++i) {
        if (!engines[i % engine_count]->spawn(work(&done)))
            std::cout << "[main] engine " << i % engine_count << " rejected a task\n";
    }
    std::cout << "[main] sync_wait across engines -> " << sync_wait(resuming(engines[0].get()))
              << '\n';

    for (auto &e : engines)
        e->stop(); // graceful: runs whatever is still queued

    const auto elapsed = std::chrono::steady_clock::now() - begin;
    std::cout << "[main] " << done.load() << '/' << task_count << " tasks done in "
              << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
    for (unsigned i = 0; i < engine_count; ++i)
        std::cout << "[main] engine " << i << " loop iterations: " << engines[i]->loop_count()
                  << ", wakeups: " << engines[i]->wakeup_count() << '\n';
}