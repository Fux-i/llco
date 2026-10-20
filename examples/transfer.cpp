#include <iostream>
#include <llco/llco.hpp>

using namespace llco;

task<int> leaf(int value) {
    std::cout << "[leaf]   start, value = " << value << '\n';
    co_return value * 2;
}

task<int> middle(int input) {
    std::cout << "[middle] before co_await leaf\n";
    const int output = co_await leaf(input + 1);
    std::cout << "[middle] resumed with " << output << '\n';
    co_return output + 1;
}

task<int> top() {
    std::cout << "[top]    before co_await middle\n";
    const int value = co_await middle(10);
    std::cout << "[top]    resumed with " << value << '\n';
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
    auto t = top();
    std::cout << "[main]   t.resume()\n";
    t.resume();
    std::cout << "[main]   t.result() == " << t.result() << '\n';

    auto chain = run();
    std::cout << "[main] chain.resume()\n";
    chain.resume();
    std::cout << "[main] chain done\n";
}
