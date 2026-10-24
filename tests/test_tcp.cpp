#include <gtest/gtest.h>
#include <llco/core/task.hpp>
#include <llco/io/io_uring.decl.hpp>
#include <llco/net/socket.decl.hpp>
#include <llco/runtime/engine.decl.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using namespace llco;
using namespace std::chrono_literals;

namespace {

constexpr auto BUDGET        = 1s;
constexpr auto POLL_INTERVAL = 1ms;
constexpr auto PAYLOAD       = std::string_view{"hello tcp"};

// Polls until the predicate holds, or the budget runs out.
template <class Predicate>
auto wait_until(Predicate done) -> bool {
    const auto end = std::chrono::steady_clock::now() + BUDGET;
    while (!done() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(POLL_INTERVAL);
    return done();
}

// One echo round trip: accept, read once, write it back, close.
task<> echo_once(engine *e, int listener, std::atomic<bool> *served) {
    const int                fd = co_await io::accept(e->ring(), listener);
    std::array<std::byte, 64> buffer{};
    const auto               bytes = co_await io::recv(e->ring(), fd, buffer);
    (void)co_await io::send(e->ring(), fd, std::span{buffer}.first(bytes));
    served->store(true);
    ::close(fd);
}

task<> echo_client(engine *e, std::uint16_t port, std::span<char> reply,
                   std::atomic<bool> *done) {
    const int fd = net::tcp_socket();
    co_await io::connect(e->ring(), fd, net::loopback(port));
    std::array<char, PAYLOAD.size()> payload{};
    std::ranges::copy(PAYLOAD, payload.begin());
    (void)co_await io::send(e->ring(), fd, std::as_writable_bytes(std::span{payload}));
    (void)co_await io::recv(e->ring(), fd, std::as_writable_bytes(reply));
    done->store(true);
    ::close(fd);
}

task<> connect_refused(engine *e, std::uint16_t port, std::atomic<int> *code,
                       std::atomic<bool> *done) {
    const int fd = net::tcp_socket();
    try {
        co_await io::connect(e->ring(), fd, net::loopback(port));
    } catch (const std::system_error &error) {
        code->store(error.code().value());
    }
    done->store(true);
    ::close(fd);
}

// Reads the payload, echoes it, then keeps reading: the peer's half close must read as EOF.
task<> half_close_server(engine *e, int listener, std::atomic<bool> *eof,
                         std::atomic<bool> *done) {
    const int                 fd = co_await io::accept(e->ring(), listener);
    std::array<std::byte, 64> buffer{};
    const auto                bytes = co_await io::recv(e->ring(), fd, buffer);
    (void)co_await io::send(e->ring(), fd, std::span{buffer}.first(bytes));
    const auto rest = co_await io::recv(e->ring(), fd, buffer);
    eof->store(rest == 0);
    done->store(true);
    ::close(fd);
}

// Sends the payload, shuts down its write side, then drains the echo and the final EOF.
task<> half_close_client(engine *e, std::uint16_t port, std::span<char> reply,
                         std::atomic<bool> *eof, std::atomic<bool> *done) {
    const int fd = net::tcp_socket();
    co_await io::connect(e->ring(), fd, net::loopback(port));
    std::array<char, PAYLOAD.size()> payload{};
    std::ranges::copy(PAYLOAD, payload.begin());
    (void)co_await io::send(e->ring(), fd, std::as_writable_bytes(std::span{payload}));
    ::shutdown(fd, SHUT_WR);
    (void)co_await io::recv(e->ring(), fd, std::as_writable_bytes(reply));
    std::array<std::byte, 8> spare{};
    const auto               rest = co_await io::recv(e->ring(), fd, spare);
    eof->store(rest == 0);
    done->store(true);
    ::close(fd);
}

} // namespace

TEST(net, listen_tcp_reports_its_port) {
    const int listener = net::listen_tcp(0);
    const auto port    = net::local_port(listener);
    EXPECT_NE(port, 0) << "the kernel picked port 0";
    ::close(listener);
}

TEST(tcp, connect_and_echo) {
    const int  listener = net::listen_tcp(0);
    const auto port     = net::local_port(listener);

    std::array<char, PAYLOAD.size()> reply{};
    std::atomic<bool>                served{false};
    std::atomic<bool>                echoed{false};

    engine e;
    e.start();
    ASSERT_TRUE(e.spawn(echo_once(&e, listener, &served)));
    ASSERT_TRUE(e.spawn(echo_client(&e, port, reply, &echoed)));
    ASSERT_TRUE(wait_until([&] { return served.load() && echoed.load(); }))
        << "the round trip never finished";
    e.stop();
    ::close(listener);

    const std::string_view back{reply.data(), reply.size()};
    EXPECT_EQ(back, PAYLOAD);
    EXPECT_EQ(e.ring().running_io(), 0U);
}

TEST(tcp, connect_refused) {
    const int  listener = net::listen_tcp(0);
    const auto port     = net::local_port(listener);
    ::close(listener); // nobody is listening on that port now

    std::atomic<int>  code{0};
    std::atomic<bool> done{false};

    engine e;
    e.start();
    ASSERT_TRUE(e.spawn(connect_refused(&e, port, &code, &done)));
    ASSERT_TRUE(wait_until([&] { return done.load(); })) << "the failed connect never came back";
    e.stop();

    EXPECT_EQ(code.load(), ECONNREFUSED);
}

TEST(tcp, half_close_ends_the_stream) {
    const int  listener = net::listen_tcp(0);
    const auto port     = net::local_port(listener);

    std::array<char, PAYLOAD.size()> reply{};
    std::atomic<bool>                server_eof{false};
    std::atomic<bool>                client_eof{false};
    std::atomic<bool>                served{false};
    std::atomic<bool>                echoed{false};

    engine e;
    e.start();
    ASSERT_TRUE(e.spawn(half_close_server(&e, listener, &server_eof, &served)));
    ASSERT_TRUE(e.spawn(half_close_client(&e, port, reply, &client_eof, &echoed)));
    ASSERT_TRUE(wait_until([&] { return served.load() && echoed.load(); }))
        << "the half-closed conversation never finished";
    e.stop();
    ::close(listener);

    const std::string_view back{reply.data(), reply.size()};
    EXPECT_EQ(back, PAYLOAD);
    EXPECT_TRUE(server_eof.load()) << "the server did not see EOF after the half close";
    EXPECT_TRUE(client_eof.load()) << "the client did not see EOF after the echo";
}
