#include "coro.hpp"
#include <iostream>

using namespace llco;

task<int> leaf(int value) {
    std::cout << "[leaf]   start, value = " << value << '\n';
    co_return value * 2;
}

task<int> middle(int input) {
    std::cout << "[middle] before co_await leaf\n";
    const int output = co_await leaf(input + 1);
    std::cout << "[middle] resumed with " << output << ", co_await returned\n";
    co_return output + 1;
}

task<int> top() {
    std::cout << "[top]    before co_await middle\n";
    const int value = co_await middle(10);
    std::cout << "[top]    resumed with " << value << ", co_await returned\n";
    co_return value + 1;
}

task<> log(const char *what) {
    std::cout << "    [log] " << what << '\n';
    co_return;
}

task<> step(const char *name) {
    std::cout << "  [" << name << "] in\n";
    co_await log(name);
    std::cout << "  [" << name << "] out\n";
    co_return;
}

task<> run() {
    std::cout << "[run] start\n";
    co_await step("A");
    co_await step("B");
    std::cout << "[run] end\n";
    co_return;
}

int main() {
    auto task = top();
    std::cout << "[main]   task.resume()\n";
    task.resume();
    const int result = task.result();
    std::cout << "[main]   task.resume() returned: top() == " << result << '\n';

    auto t = run();
    std::cout << "[main] t.resume()\n";
    t.resume();
    std::cout << "[main] resume return\n";
}
