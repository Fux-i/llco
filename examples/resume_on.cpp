#include <llco/config.hpp>
#include <llco/llco.hpp>
#include <llco/trace/trace.decl.hpp>
#include <print>
#include <thread>

using namespace llco;

namespace {

task<> resumer() {
    std::println("[body] start on thread {}", std::this_thread::get_id());
    co_await resume_on(scheduler::engine_at(1)); // hand the frame over to the other engine
    std::println("[body] resumed on thread {}", std::this_thread::get_id());
    co_return;
}

} // namespace

int main() {
#if LLCO_TRACE
    trace::set_thread_name("main");
#endif
    scheduler::init(2);

    sync_run(resumer()); // starts on engine 0, continues on engine 1
#if LLCO_TRACE
    trace::flush("trace.json");
#endif
}
