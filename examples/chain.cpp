#include <llco/config.hpp>
#include <llco/llco.hpp>
#include <llco/trace/trace.decl.hpp>
#include <print>

using namespace llco;

namespace {

constexpr int INPUT = 10;

task<int> leaf(int value) {
    co_return value * 2;
}

task<int> middle(int value) {
    co_return co_await leaf(value + 1) + 1;
}

task<int> top() {
    co_return co_await middle(INPUT) + 1;
}

} // namespace

int main() {
#if LLCO_TRACE
    trace::set_thread_name("main");
#endif

    // sync_wait starts the root on this thread; every nested co_await runs inline on it.
    std::println("[main] top() = {}", sync_wait(top()));

#if LLCO_TRACE
    trace::flush("trace.json");
#endif
}
