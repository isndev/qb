/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/udp/udp-datagram.cpp
 * @brief Raw `qb::io::udp::socket` datagram round-trips over IPv4 / IPv6 / AF_UNIX loopback.
 *
 * This file gathers the bare-socket datagram loopback round-trips that used to live (as fixed-port,
 * `sleep_for(3s)`, assert-the-empty-non-result clones) in test-io.cpp's `INET_UDP.*` / `UNIX_UDP.*`
 * suites, plus the `UDPDatagram` send→recv case from test-async-io.cpp. Where socket-options.cpp
 * covers the `udp::socket` *API surface* (options, multicast, timeouts) and transport-datagram.cpp
 * covers the `transport::udp` *proxy*, this file is the end-to-end "datagram actually crosses the
 * wire and the payload + peer are correct" probe over all three address families.
 *
 * De-flake (per the restructure spec §2): the original `INET_UDP.NonBlocking` / `UNIX_UDP.NonBlocking`
 * "tests" sent from a non-blocking socket after a 3-second sleep and then asserted the receiver read
 * back the *empty* string (`EXPECT_EQ(std::string(buffer), "")`) — a near-vacuous "nothing arrived in
 * the window we slept" check that is both slow and structurally unfalsifiable. They are replaced by a
 * single, deterministic non-blocking contract: a freshly bound non-blocking socket with no pending
 * datagram returns a would-block (`<= 0`) read *immediately*, and after a real send a deadline-bounded
 * poll receives the exact payload. No fixed ports, no multi-second sleeps.
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
 * @ingroup Tests
 */

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <qb/io/system/sys__socket.h>
#include <qb/io/udp/socket.h>

using namespace std::chrono_literals;

namespace {

// Deadline-bounded blocking-mode-agnostic datagram receive: poll `read()` until a
// datagram arrives or the deadline elapses. Returns the byte count (>0) or -1 on
// timeout, so callers get a precise failure instead of a slept-out empty read.
int
receive_datagram(qb::io::udp::socket &socket, char *buffer, std::size_t len, qb::io::endpoint &peer,
                 std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const int got = socket.read(buffer, len, peer);
        if (got > 0) {
            return got;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return -1;
}

bool
ipv6_loopback_available() {
    qb::io::udp::socket probe;
    return probe.bind_v6(0, "::1") == 0;
}

#ifndef _WIN32
struct unix_socket_file {
    explicit unix_socket_file(std::string_view suffix)
        : path("/tmp/qb-udp-datagram-" + std::to_string(::getpid()) + "-" + std::string(suffix) + ".sock") {
        ::unlink(path.c_str());
    }

    unix_socket_file(const unix_socket_file &)            = delete;
    unix_socket_file &operator=(const unix_socket_file &) = delete;

    ~unix_socket_file() {
        ::unlink(path.c_str());
    }

    std::string path;
};
#endif

} // namespace

// Absorbed from test-io.cpp INET_UDP.Blocking + test-async-io.cpp UDPDatagram:
// a real send→recv over IPv4 loopback delivers the exact payload and the peer ip.
TEST(UDPDatagram, Ipv4LoopbackDeliversPayloadAndPeer) {
    qb::io::udp::socket receiver;
    ASSERT_EQ(receiver.bind_v4(0, "127.0.0.1"), 0);
    const auto port = receiver.local_endpoint().port();
    ASSERT_NE(port, 0);

    qb::io::udp::socket sender;
    ASSERT_TRUE(sender.init());

    constexpr std::string_view message = "Hello, UDP datagram!";
    ASSERT_EQ(sender.write(message.data(), message.size(), qb::io::endpoint().as_in("127.0.0.1", port)),
              static_cast<int>(message.size()));

    char             buffer[512] = {};
    qb::io::endpoint peer;
    const int        got = receive_datagram(receiver, buffer, sizeof(buffer), peer);
    ASSERT_EQ(got, static_cast<int>(message.size())) << "IPv4 datagram never arrived";
    EXPECT_EQ(std::string_view(buffer, static_cast<std::size_t>(got)), message);
    EXPECT_EQ(peer.ip(), "127.0.0.1");
}

// Absorbed from test-io.cpp INET_UDP.NonBlocking — but rewritten from the vacuous
// "slept 3s then read empty" form into a real non-blocking contract: an empty
// non-blocking socket would-blocks immediately, and a real send is then received.
TEST(UDPDatagram, Ipv6LoopbackNonBlockingDeliversPayload) {
    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available on this host";
    }

    qb::io::udp::socket receiver;
    ASSERT_EQ(receiver.bind_v6(0, "::1"), 0);
    receiver.set_nonblocking(true);
    const auto port = receiver.local_endpoint().port();
    ASSERT_NE(port, 0);

    // With nothing pending, a non-blocking read must would-block immediately.
    char             buffer[512] = {};
    qb::io::endpoint empty_peer;
    EXPECT_LE(receiver.read(buffer, sizeof(buffer), empty_peer), 0) << "non-blocking read with no datagram must not block or succeed";

    qb::io::udp::socket sender;
    ASSERT_TRUE(sender.init(AF_INET6));
    sender.set_nonblocking(true);

    constexpr std::string_view message = "Hello v6 datagram!";
    ASSERT_EQ(sender.write(message.data(), message.size(), qb::io::endpoint().as_in("::1", port)),
              static_cast<int>(message.size()));

    qb::io::endpoint peer;
    const int        got = receive_datagram(receiver, buffer, sizeof(buffer), peer);
    ASSERT_EQ(got, static_cast<int>(message.size())) << "IPv6 datagram never arrived";
    EXPECT_EQ(std::string_view(buffer, static_cast<std::size_t>(got)), message);
    EXPECT_EQ(peer.af(), AF_INET6);
}

#ifndef _WIN32

// Absorbed from test-io.cpp UNIX_UDP.Blocking: a datagram over an AF_UNIX socket
// delivers the exact payload (the unix peer has no ip, by design).
TEST(UDPDatagram, UnixLoopbackDeliversPayload) {
    unix_socket_file    socket_path{"blocking"};
    qb::io::udp::socket receiver;
    ASSERT_EQ(receiver.bind_un(socket_path.path), qb::io::SocketStatus::Done);
    ASSERT_TRUE(receiver.is_open());

    qb::io::udp::socket sender;
    ASSERT_TRUE(sender.init(AF_UNIX));

    constexpr std::string_view message = "Hello unix datagram!";
    ASSERT_GT(sender.write(message.data(), message.size(), qb::io::endpoint().as_un(socket_path.path.c_str())), 0);

    char             buffer[512] = {};
    qb::io::endpoint peer;
    const int        got = receive_datagram(receiver, buffer, sizeof(buffer), peer);
    ASSERT_EQ(got, static_cast<int>(message.size())) << "unix datagram never arrived";
    EXPECT_EQ(std::string_view(buffer, static_cast<std::size_t>(got)), message);
}

// Absorbed from test-io.cpp UNIX_UDP.NonBlocking — rewritten from the slept-out
// empty-read form into the real non-blocking contract.
TEST(UDPDatagram, UnixLoopbackNonBlockingDeliversPayload) {
    unix_socket_file    socket_path{"nonblocking"};
    qb::io::udp::socket receiver;
    ASSERT_EQ(receiver.bind_un(socket_path.path), qb::io::SocketStatus::Done);
    receiver.set_nonblocking(true);

    char             buffer[512] = {};
    qb::io::endpoint empty_peer;
    EXPECT_LE(receiver.read(buffer, sizeof(buffer), empty_peer), 0) << "non-blocking read with no datagram must not block or succeed";

    qb::io::udp::socket sender;
    ASSERT_TRUE(sender.init(AF_UNIX));
    sender.set_nonblocking(true);

    constexpr std::string_view message = "Hello unix nb datagram!";
    ASSERT_GT(sender.write(message.data(), message.size(), qb::io::endpoint().as_un(socket_path.path.c_str())), 0);

    qb::io::endpoint peer;
    const int        got = receive_datagram(receiver, buffer, sizeof(buffer), peer);
    ASSERT_EQ(got, static_cast<int>(message.size())) << "unix non-blocking datagram never arrived";
    EXPECT_EQ(std::string_view(buffer, static_cast<std::size_t>(got)), message);
}

#endif
