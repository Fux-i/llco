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
    run(report(1)); // submits and returns: main keeps going

    sync_run(report(2), report(3)); // submits and blocks until both finish

    // A coroutine lambda has to spell out its return type: co_return cannot
    // appear in a function with a deduced return type.
    sync_run([] -> task<> {
        std::println("[lambda] thread {}", std::this_thread::get_id());
        co_return;
    });

    // A container of tasks is accepted as an rvalue only: the range is consumed.
    std::vector<task<>> batch;
    batch.reserve(2);
    for (int id = 4; id < 6; ++id) // NOLINT(cppcoreguidelines-avoid-magic-numbers): demo labels
        batch.push_back(report(id));
    sync_run(std::move(batch));
}