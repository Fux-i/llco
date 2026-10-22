#include <atomic>
#include <chrono>
#include <iostream>
#include <llco/llco.hpp>
#include <llco/trace.decl.hpp>
#include <map>
#include <mutex>
#include <span>
#include <thread>

using namespace llco;

namespace {

constexpr unsigned ENGINE_COUNT = 4;
constexpr unsigned TASK_COUNT   = 1000;
constexpr int      HOP_VALUE    = 7;
constexpr auto     POLL         = std::chrono::milliseconds{1};

struct census {
    std::atomic<unsigned>               done{0};
    std::mutex                          mutex;
    std::map<std::thread::id, unsigned> per_thread;
};

task<> work(census *state) {
    {
        const std::scoped_lock lock{state->mutex};
        ++state->per_thread[std::this_thread::get_id()];
    }
    ++state->done;
    co_return;
}

// Reports the thread it starts on and the one it continues on after the hop.
task<> hopping(engine *target, std::atomic<int> *value) {
    std::cout << "  [hop] before: thread " << std::this_thread::get_id() << '\n';
    co_await resume_on(*target);
    std::cout << "  [hop] after : thread " << std::this_thread::get_id() << '\n';
    *value = HOP_VALUE;
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    trace::set_thread_name("main");

    const auto begin = std::chrono::steady_clock::now();
    scheduler  s{ENGINE_COUNT};
    census     state;

    for (unsigned i = 0; i < TASK_COUNT; ++i) {
        if (!s.submit(work(&state)))
            std::cout << "[main] submit " << i << " rejected\n";
    }
    while (state.done.load() < TASK_COUNT)
        std::this_thread::sleep_for(POLL);

    std::atomic<int> hopped{0};
    if (!s.engine_at(0).spawn(hopping(&s.engine_at(1), &hopped)))
        std::cout << "[main] hop task rejected\n";
    while (hopped.load() == 0)
        std::this_thread::sleep_for(POLL);

    s.stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    std::cout << "[main] " << state.done.load() << '/' << TASK_COUNT << " tasks done in "
              << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";

    for (const auto &[tid, count] : state.per_thread)
        std::cout << "[main]   thread " << tid << " ran " << count << " task(s)\n";
    for (unsigned i = 0; i < s.size(); ++i)
        std::cout << "[main] engine " << i << ": loops " << s.engine_at(i).loop_count()
                  << ", doorbells " << s.engine_at(i).wakeup_count() << ", queued "
                  << s.engine_at(i).load() << '\n';

    if (args.size() > 1) {
        const bool written = trace::flush(args[1]);
        std::cout << "[main] wrote " << args[1] << ": " << written << '\n';
    }
}
