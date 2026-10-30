#include <llco/llco.hpp>
#include <print>
#include <thread>
#include <utility>
#include <vector>

using namespace llco;

namespace {

task<> report(int id) {
    std::println("[task {}] thread {}", id, std::this_thread::get_id());
    co_return;
}

} // namespace

int main() {
    // No scheduler object and no init(): the pool is built on first use, with
    // hardware_concurrency() engines, and its destructor stops it at exit.

    // run() submits and returns.
    run(report(1));

    // A TaskMaker is any callable that hands back a task. A coroutine lambda has
    // to spell out its return type: co_return cannot live in a deduced one.
    run([] -> task<> {
        std::println("[lambda] thread {}", std::this_thread::get_id());
        co_return;
    });

    // sync_run() submits and blocks until every task has finished.
    sync_run(report(2), report(3));

    // A TaskBatch is an rvalue range of tasks, consumed by the call.
    std::vector<task<>> batch;
    batch.reserve(2);
    for (int id = 4; id < 6; ++id) // NOLINT(cppcoreguidelines-avoid-magic-numbers): demo labels
        batch.push_back(report(id));
    sync_run(std::move(batch));
}
