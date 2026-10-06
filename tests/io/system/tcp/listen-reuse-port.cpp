/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/listen-reuse-port.cpp
 * @brief `SO_REUSEPORT` asked for by name: `socket::reuse_port`, `tcp::listen_options::reuse_port` (Huly QB-78).
 *
 * The contract, one assertion per clause:
 *   - `reuse_address` sets `SO_REUSEADDR` and no longer `SO_REUSEPORT` (it set both, so any caller of it shared
 *     its port without saying so);
 *   - `reuse_port` sets `SO_REUSEPORT` where the system has it, and refuses with `ENOPROTOOPT` on Windows;
 *   - two listeners share a port only when BOTH asked for it: one that did not ask is refused the port, and
 *     a listener that asks cannot join one that did not; on Windows a listen that asks fails with
 *     `ENOPROTOOPT` and leaves its socket closed;
 *   - on Linux the kernel balances the accept across the listeners that share the port -- the point of the
 *     option, measured with 64 connections over two listeners (one of them getting none has a probability of
 *     2^-63 under the kernel's 4-tuple hash). macOS and the BSDs share the bind without balancing: skipped there.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/system/sys__socket.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

namespace {

#if defined(SO_REUSEPORT) && !defined(_WIN32)
constexpr bool kHasReusePort = true;
#else
constexpr bool kHasReusePort = false;
#endif

int
read_option(qb::io::socket &s, int level, int name) {
    int value = -1;
    EXPECT_EQ(s.get_optval(level, name, value), 0);
    return value;
}

uint16_t
port_of(const qb::io::tcp::listener &l) {
    return l.local_endpoint().port();
}

} // namespace

TEST(ListenReusePort, ReuseAddressNoLongerSharesThePort) {
    qb::io::socket s;
    ASSERT_TRUE(s.open(AF_INET, SOCK_STREAM, 0));
    s.reuse_address(true);
    EXPECT_NE(read_option(s, SOL_SOCKET, SO_REUSEADDR), 0);
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    EXPECT_EQ(read_option(s, SOL_SOCKET, SO_REUSEPORT), 0) << "reuse_address must not share the port any more";
#endif
}

TEST(ListenReusePort, ReusePortIsSetByNameOrRefusedHonestly) {
    qb::io::socket s;
    ASSERT_TRUE(s.open(AF_INET, SOCK_STREAM, 0));
    if constexpr (kHasReusePort) {
        EXPECT_TRUE(s.reuse_port(true));
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        EXPECT_NE(read_option(s, SOL_SOCKET, SO_REUSEPORT), 0);
        EXPECT_TRUE(s.reuse_port(false));
        EXPECT_EQ(read_option(s, SOL_SOCKET, SO_REUSEPORT), 0);
#endif
    } else {
        EXPECT_FALSE(s.reuse_port(true));
        EXPECT_EQ(qb::io::socket::get_last_errno(), ENOPROTOOPT);
    }
}

TEST(ListenReusePort, TwoListenersShareAPortOnlyWhenBothAskedForIt) {
    const qb::io::tcp::listen_options share{.reuse_port = true};

    qb::io::tcp::listener first;
    if constexpr (!kHasReusePort) {
        EXPECT_NE(first.listen_v4(0, "127.0.0.1", share), 0);
        EXPECT_EQ(qb::io::socket::get_last_errno(), ENOPROTOOPT);
        EXPECT_FALSE(first.is_open()) << "a refused listen leaves no socket behind";
        GTEST_SKIP() << "no SO_REUSEPORT on this system: a listen that asks for it fails, as asserted";
    }
    ASSERT_EQ(first.listen_v4(0, "127.0.0.1", share), 0);
    const auto port = port_of(first);
    ASSERT_NE(port, 0);

    qb::io::tcp::listener second;
    EXPECT_EQ(second.listen_v4(port, "127.0.0.1", share), 0) << "both asked: the port is shared";

    qb::io::tcp::listener intruder;
    EXPECT_NE(intruder.listen_v4(port, "127.0.0.1"), 0) << "one that did not ask is refused the port";

    qb::io::tcp::listener plain;
    ASSERT_EQ(plain.listen_v4(0, "127.0.0.1"), 0);
    qb::io::tcp::listener joiner;
    EXPECT_NE(joiner.listen_v4(port_of(plain), "127.0.0.1", share), 0) << "asking cannot join a listener that did not";
}

TEST(ListenReusePort, TheKernelBalancesTheAcceptAcrossSharedListeners) {
#if !defined(__linux__)
    GTEST_SKIP() << "only Linux balances the accept across listeners sharing a port";
#else
    const qb::io::tcp::listen_options share{.reuse_port = true};
    qb::io::tcp::listener             a;
    ASSERT_EQ(a.listen_v4(0, "127.0.0.1", share), 0);
    const auto            port = port_of(a);
    qb::io::tcp::listener b;
    ASSERT_EQ(b.listen_v4(port, "127.0.0.1", share), 0);

    constexpr int                    kConnections = 64;
    std::vector<qb::io::tcp::socket> clients(kConnections);
    for (auto &c : clients)
        ASSERT_EQ(c.connect(qb::io::endpoint{"127.0.0.1", port}), 0); // completes into a listener's backlog

    a.set_nonblocking(true);
    b.set_nonblocking(true);
    auto drain = [](qb::io::tcp::listener &l) {
        int n = 0;
        for (;;) {
            auto s = l.accept();
            if (!s.is_open())
                return n;
            ++n;
        }
    };
    const int on_a = drain(a);
    const int on_b = drain(b);
    EXPECT_EQ(on_a + on_b, kConnections);
    EXPECT_GT(on_a, 0) << "the kernel gave listener A none of " << kConnections;
    EXPECT_GT(on_b, 0) << "the kernel gave listener B none of " << kConnections;
#endif
}
