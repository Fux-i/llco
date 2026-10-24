#include <gtest/gtest.h>
#include <llco/net/socket.decl.hpp>

#include <unistd.h>

using namespace llco;

TEST(net, listen_tcp_reports_its_port) {
    const int listener = net::listen_tcp(0);
    const auto port    = net::local_port(listener);
    EXPECT_NE(port, 0) << "the kernel picked port 0";
    ::close(listener);
}
