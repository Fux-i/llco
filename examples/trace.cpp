#include <llco/config.hpp>
#include <llco/llco.hpp>
#include <llco/trace/trace.decl.hpp>
#include <print>

using namespace llco;

namespace {

task<> stage(const char *name) {
    const trace::scope slice{name};
    std::println("[stage] {}", name);
    co_return;
}

task<> pipeline() {
    const trace::scope slice{"pipeline"};
    co_await stage("load");
    co_await stage("compute");
    co_return;
}

} // namespace

int main() {
#if LLCO_TRACE
    trace::set_thread_name("main");
#endif
    scheduler::init(2);

    {
        const trace::scope submit{"submit"};
        sync_run(pipeline()); // these scopes nest inside the engine's own "resume" slice
    }
#if LLCO_TRACE
    trace::flush("trace.json");
#endif
}
