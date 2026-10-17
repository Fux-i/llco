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

int main() {
    auto task = top();
    std::cout << "[main]   task.resume()\n";
    task.resume();
    const int result = task.result();
    std::cout << "[main]   task.resume() returned: top() == " << result << '\n';
    return result == 24 ? 0 : 1;
}
