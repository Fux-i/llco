#include <chrono>
#include <cstdint>
#include <llco/llco.hpp>
#include <print>
#include <thread>

#include <liburing.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto TIMEOUT       = 200ms;
constexpr auto WAIT_LIMIT    = 2 * TIMEOUT;
constexpr auto POLL_INTERVAL = 1ms;

// Arm one timeout on the engine ring: submitting must happen on the engine thread.
task<> arm_timeout(engine *e, __kernel_timespec *deadline) {
    ::io_uring_prep_timeout(e->ring().sqe(), deadline, 0, 0);
    e->ring().submit();
    co_return;
}

} // namespace

int main() {
    // The kernel reads the deadline until the CQE lands, so it outlives the coroutine.
    auto deadline = io::to_kernel_time(TIMEOUT);

    engine e;
    e.start();
    const auto begin = std::chrono::steady_clock::now();

    if (!e.spawn(arm_timeout(&e, &deadline)))
        return 1;
    std::println("[submit] one {} ms timeout, then the loop blocks on read(eventfd)",
                 TIMEOUT.count());

    const auto limit = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (e.ring().completions() == 0 && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(POLL_INTERVAL);

    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::println("[wake] woke after {:.0f} ms, completions: {}", elapsed, e.ring().completions());

    e.stop();
    std::println("[stop] {} loop iterations: idle start, doorbell, completion, stop",
                 e.loop_count());
}
