#include <llco/engine.decl.hpp>
#include <llco/task.hpp>

#include <iostream>

using namespace llco;

namespace {

task<int> leaf(int value) {
    std::cout << "  [leaf]   start, value = " << value << '\n';
    co_return value * 2;
}

task<int> middle(int input) {
    std::cout << "  [middle] before co_await leaf\n";
    const int output = co_await leaf(input + 1);
    std::cout << "  [middle] resumed with " << output << '\n';
    co_return output + 1;
}

task<int> top() {
    std::cout << "  [top]    before co_await middle\n";
    const int value = co_await middle(10);
    std::cout << "  [top]    resumed with " << value << '\n';
    co_return value + 1;
}

} // namespace

int main() {
    auto root = top();
    std::cout << "[main] task is lazy: nothing has run yet\n";

    engine<> e;
    std::cout << "[main] try_add -> " << e.try_add(root.handle()) << '\n';

    std::cout << "[main] run_once() resumes one handle, and the whole chain runs inline\n";
    e.run_once();

    std::cout << "[main] drain() -> " << e.drain() << " handle(s) left\n";
    std::cout << "[main] result == " << root.result() << '\n';
}
