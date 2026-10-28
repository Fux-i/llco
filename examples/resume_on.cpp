#include <iostream>
#include <llco/llco.hpp>

using namespace llco;

namespace {

task<> resumer(engine *target, int *steps) {
    std::cout << "  [body] before the resume_on\n";
    *steps = 1;
    co_await resume_on(*target);
    std::cout << "  [body] after the resume_on\n";
    *steps = 2;
}

} // namespace

int main() {
    engine e;
    int    steps = 0;

    const bool queued = e.spawn(resumer(&e, &steps));
    std::cout << "[main] spawn -> " << queued << '\n';

    const bool started = e.run_once();
    std::cout << "[main] run_once() -> " << started << '\n';
    std::cout << "[main] the body is parked in the queue: steps = " << steps << '\n';

    const auto drained = e.drain();
    std::cout << "[main] drain() -> " << drained << " handle(s) left\n";
    std::cout << "[main] resumed by the queue: steps = " << steps << '\n';
}