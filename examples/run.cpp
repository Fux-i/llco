#include <llco/config.hpp>
#include <llco/llco.hpp>
#include <llco/trace/trace.decl.hpp>
#include <print>
#include <thread>
#include <utility>
#include <vector>

using namespace llco;

namespace {

constexpr int BATCH = 2;

task<> report(int id) {
    std::println("[task {}] thread {}", id, std::this_thread::get_id());
    co_return;
}

} // namespace

int main() {
#if LLCO_TRACE
    trace::set_thread_name("main");
#endif

    run(report(1)); // submits and returns

    // A coroutine lambda must spell out its return type: co_return has no deduced one.
    run([] -> task<> {
        std::println("[lambda] thread {}", std::this_thread::get_id());
        co_return;
    });

    sync_run(report(2), report(3)); // submits and blocks until all of them finish

    std::vector<task<>> batch;
    for (int id = 4; id < 4 + BATCH; ++id)
        batch.push_back(report(id));
    sync_run(std::move(batch)); // a rvalue range of tasks

#if LLCO_TRACE
    trace::flush("trace.json");
#endif
}
