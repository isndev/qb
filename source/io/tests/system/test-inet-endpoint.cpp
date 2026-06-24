/**
 * @file source/io/tests/system/test-inet-endpoint.cpp
 * @brief Unit tests for qb::io::inet::endpoint (pure address parse/format, no live I/O).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <string>
// Pulls in qb/io/config.h, which provides the socket vocabulary (AF_UNSPEC,
// AF_INET, INADDR_LOOPBACK, ...) portably — WinSock2 on Windows, <arpa/inet.h>
// & friends on POSIX. This is a pure parse/format test (no live I/O or raw
// syscalls), so it stays cross-platform without the POSIX-only headers.
#include <qb/io/tcp/socket.h>

using qb::io::endpoint;

TEST(InetEndpoint, DefaultIsInvalid) {
    endpoint e;
    EXPECT_FALSE(static_cast<bool>(e)); // operator bool() == is_valid (af != AF_UNSPEC)
    EXPECT_EQ(e.af(), AF_UNSPEC);
}

TEST(InetEndpoint, IPv4ParseAndFormat) {
    endpoint e("127.0.0.1", 8080);
    EXPECT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_EQ(e.port(), 8080);
    EXPECT_EQ(e.ip(), "127.0.0.1");
    EXPECT_EQ(e.to_string(), "127.0.0.1:8080");
}

TEST(InetEndpoint, IPv6ParseAndFormat) {
    endpoint e("::1", 443);
    EXPECT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET6);
    EXPECT_EQ(e.port(), 443);
    EXPECT_EQ(e.ip(), "::1");
    EXPECT_EQ(e.to_string(), "[::1]:443"); // IPv6 bracketed; last char must survive (regression guard)
}

TEST(InetEndpoint, IPv6ToStringKeepsLastChar) {
    // Regression: to_string() used to write ']' one byte early, clobbering the final hextet.
    endpoint e("2001:db8::abcd", 8080);
    EXPECT_EQ(e.to_string(), "[2001:db8::abcd]:8080");
}

TEST(InetEndpoint, PortSetterGetter) {
    endpoint e("10.0.0.5", 1);
    e.port(static_cast<unsigned short>(9999));
    EXPECT_EQ(e.port(), 9999);
    EXPECT_EQ(e.ip(), "10.0.0.5"); // address unchanged
}

TEST(InetEndpoint, IpSetterRoundTripV4AndV6) {
    endpoint e4;
    e4.ip("192.168.1.42");
    EXPECT_EQ(e4.af(), AF_INET);
    EXPECT_EQ(e4.ip(), "192.168.1.42");

    endpoint e6;
    e6.ip("2001:db8::1");
    EXPECT_EQ(e6.af(), AF_INET6);
    EXPECT_EQ(e6.ip(), "2001:db8::1");
}

TEST(InetEndpoint, CopyConstruct) {
    endpoint a("172.16.0.1", 22);
    endpoint b(a);
    EXPECT_EQ(b.af(), a.af());
    EXPECT_EQ(b.port(), a.port());
    EXPECT_EQ(b.ip(), a.ip());
    EXPECT_EQ(b.to_string(), a.to_string());
}

TEST(InetEndpoint, AfSetter) {
    endpoint e;
    e.af(AF_INET);
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_TRUE(static_cast<bool>(e)); // valid once a family is set
}

TEST(InetEndpoint, Uint32CtorIsInetWithPort) {
    // endpoint(uint32_t) takes a HOST-order IPv4 address (addr_v4 applies htonl internally).
    endpoint e(static_cast<uint32_t>(INADDR_LOOPBACK), static_cast<unsigned short>(53));
    EXPECT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_EQ(e.port(), 53);
    EXPECT_EQ(e.ip(), "127.0.0.1");
}
