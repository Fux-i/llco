#include <array>
#include <chrono>
#include <cstddef>
#include <llco/config.hpp>
#include <llco/io/op.decl.hpp>
#include <llco/llco.hpp>
#include <llco/trace/trace.decl.hpp>
#include <print>
#include <semaphore>
#include <span>
#include <string_view>
#include <thread>

#include <unistd.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view PAYLOAD     = "hello";
constexpr auto             WRITE_DELAY = 50ms;

// Reads one chunk on the engine thread; the loop flushes the SQE after drain().
task<> read_once(int fd, std::span<char> buffer, std::binary_semaphore *done) {
    const auto bytes =
        co_await io::read(current_engine()->ring(), fd, std::as_writable_bytes(buffer));
    std::println("[read] {} bytes: {}", *bytes, std::string_view{buffer.data(), *bytes});
    done->release();
    co_return;
}

} // namespace

int main() {
#if LLCO_TRACE
    trace::set_thread_name("main");
#endif
    scheduler::init(1);

    std::array<int, 2> fds{};
    [[maybe_unused]]
    const int                        piped = ::pipe(fds.data());
    std::array<char, PAYLOAD.size()> buffer{};
    std::binary_semaphore            done{0};

    run(read_once(fds[0], buffer, &done));    // prepares the read on engine 0
    std::this_thread::sleep_for(WRITE_DELAY); // the engine is parked in read(eventfd) by now
    [[maybe_unused]]
    const auto written = ::write(fds[1], PAYLOAD.data(), PAYLOAD.size()); // needs data

    done.acquire();
#if LLCO_TRACE
    (void)trace::flush("trace.json");
#endif
}
