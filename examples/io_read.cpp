#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <llco/io/io_uring.decl.hpp>
#include <llco/llco.hpp>
#include <print>
#include <span>
#include <string_view>
#include <thread>

#include <unistd.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto WRITE_DELAY = 50ms;
constexpr auto WAIT_LIMIT  = 1s;
constexpr auto POLL        = 1ms;
constexpr auto PAYLOAD     = std::string_view{"hello"};

// Reads one chunk on the engine thread; the loop flushes the SQE after drain().
task<> read_once(engine *e, int fd, std::span<char> buffer, std::atomic<std::size_t> *bytes,
                 std::atomic<bool> *done) {
    const auto got = co_await io::read(e->ring(), fd, std::as_writable_bytes(buffer));
    *bytes         = got.value_or(0);
    done->store(true);
}

} // namespace

int main() {
    std::array<int, 2> fds{};
    if (::pipe(fds.data()) != 0)
        return 1;

    std::array<char, PAYLOAD.size()> buffer{};
    std::atomic<std::size_t>         bytes{0};
    std::atomic<bool>                done{false};

    engine e;
    e.start();
    const auto begin = std::chrono::steady_clock::now();
    if (!e.spawn(read_once(&e, fds[0], buffer, &bytes, &done)))
        return 1;
    std::println("[submit] one read is armed; the loop goes back to read(eventfd)");

    std::this_thread::sleep_for(WRITE_DELAY);
    (void)::write(fds[1], PAYLOAD.data(), PAYLOAD.size());

    const auto limit = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (!done.load() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(POLL);

    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    const auto received = std::string_view{buffer.data(), bytes.load()};
    std::println("[read] {} bytes after {:.0f} ms: {}", received.size(), elapsed, received);

    e.stop(); // wait for the op, so running_io() is zero here
    std::println("[stop] {} loop iterations, running io: {}", e.loop_count(),
                 e.ring().running_io());
}
