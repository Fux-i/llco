#include <iostream>
#include <llco/llco.hpp>
#include <llco/trace.decl.hpp>

using namespace llco;

namespace {

task<> work(int id) {
    const trace::scope slice{"work"};
    std::cout << "  [body] work " << id << '\n';
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "trace.json";

    trace::set_thread_name("main");

    engine e;
    {
        const trace::scope submit{"submit"};
        std::cout << "[main] spawn 1 -> " << e.spawn(work(1)) << '\n';
        std::cout << "[main] spawn 2 -> " << e.spawn(work(2)) << '\n';
    }

    const auto drained = e.drain();
    std::cout << "[main] drain() -> " << drained << " handle(s)\n";

    const bool written = trace::flush(path);
    std::cout << "[main] wrote " << path << ": " << written << '\n';
    std::cout << "[main] engine slices follow LLCO_TRACE (this build: " << LLCO_TRACE << ")\n";
}
