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
#include <qb/io/transport/udp.h>
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

// =============================================================================
// transport::udp read()/write() error + buffer-limit branches.
//
// The companion transport-datagram.cpp drives the *happy-path* proxy accounting
// (out-buffer build, multi-datagram write, identity hash). These cases close the
// remaining error / capacity branches of `qb::io::transport::udp::read()` and
// `write()` that the happy path never reaches: the `_in_buffer`-over-capacity
// early-out, the negative-`recvfrom` cleanup, the zero-length datagram on the
// constrained (small-buffer) slow path, and a failed `sendto` in `write()`.
// These run on a single thread over real loopback datagram sockets — no peer
// thread, no sleeps-as-sync; every wait is a bounded read poll that fails loud
// on stall rather than blocking forever.
// =============================================================================

namespace {

// Bounded poll of `transport::udp::read()` until a real datagram arrives.
// IMPORTANT: a non-blocking transport::udp::read() with no pending datagram returns a NEGATIVE
// would-block (EAGAIN propagated from recvfrom — see read() free_back/return-ret branch and the
// ReadOnEmptyNonBlockingSocketYieldsNegativeAndKeepsBufferEmpty test), NOT 0. So we must keep
// spinning while ret <= 0 (negative would-block AND zero-length) and stop only on ret > 0 — a real
// payload — mirroring receive_datagram() above. The previous `if (ret != 0) return ret;` bailed on
// the first would-block (-1) before the loopback datagram was even delivered, so first==-1.
// Returns the first datagram's byte count (>0), or -1 on timeout.
int
poll_transport_read(qb::io::transport::udp &receiver, std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const int ret = receiver.read();
        if (ret > 0)
            return ret;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return -1;
}

// Send one IPv4 loopback datagram of `payload` to `port` via a raw udp::socket.
void
send_one(unsigned short port, std::string_view payload) {
    qb::io::udp::socket sender;
    ASSERT_TRUE(sender.init());
    ASSERT_EQ(sender.write(payload.data(), payload.size(), qb::io::endpoint().as_in("127.0.0.1", port)),
              static_cast<int>(payload.size()));
}

} // namespace

// transport::udp::is_secure() — the connectionless transport is plaintext; this
// is consulted at compile time by generic stream/protocol code, so prove it is a
// usable constant expression as well as a correct runtime value.
TEST(UDPTransportBranches, IsSecureIsCompileTimeFalse) {
    static_assert(!qb::io::transport::udp::is_secure(), "udp transport must be plaintext");
    EXPECT_FALSE(qb::io::transport::udp::is_secure());
}

// read() early-out: once the accumulated `_in_buffer` already exceeds the
// (shrunk) max-read-buffer cap, the next read() must refuse with
// ErrBufferLimitExceeded WITHOUT touching the socket — the over-capacity guard
// at the top of read(). We first land a real datagram to populate _in_buffer,
// then tighten the cap below what is already buffered.
TEST(UDPTransportBranches, ReadRefusesWhenBufferAlreadyOverCap) {
    qb::io::transport::udp receiver;
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    receiver.transport().set_nonblocking(true);
    const auto port = receiver.transport().local_endpoint().port();
    ASSERT_NE(port, 0);

    constexpr std::string_view payload = "buffered-bytes";
    send_one(port, payload);

    const int first = poll_transport_read(receiver);
    ASSERT_EQ(first, static_cast<int>(payload.size())) << "datagram never arrived";
    ASSERT_EQ(receiver.in().size(), payload.size());

    // Now the buffer holds `payload.size()` bytes; cap it strictly below that.
    receiver.set_max_read_buffer_size(payload.size() - 1);
    EXPECT_EQ(receiver.read(), qb::io::ErrBufferLimitExceeded)
        << "read() must refuse when the input buffer already exceeds the cap";
    // The refusal is a pure early-out: the buffered bytes are left untouched.
    EXPECT_EQ(receiver.in().size(), payload.size());
}

// read() negative-recvfrom branch (fast path): a non-blocking receiver with no
// pending datagram has recvfrom return < 0 (would-block), which must roll the
// speculative MaxDatagramSize reservation back so the input buffer stays empty.
TEST(UDPTransportBranches, ReadOnEmptyNonBlockingSocketYieldsNegativeAndKeepsBufferEmpty) {
    qb::io::transport::udp receiver;
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    receiver.transport().set_nonblocking(true);

    // Nothing was ever sent: read() takes the fast path (huge default cap) and
    // recvfrom would-blocks, returning < 0 and freeing the whole reservation.
    EXPECT_LT(receiver.read(), 0) << "empty non-blocking read must report a negative would-block";
    EXPECT_EQ(receiver.in().size(), 0u) << "failed read must not leak the speculative reservation";
}

// read() zero-length datagram on the CONSTRAINED slow path: with a small read
// cap (< MaxDatagramSize) read() uses the stack-buffer slow path; a real
// zero-length datagram returns 0 there and still re-targets the reply
// destination to the sender (so a subsequent reply would go back to it).
TEST(UDPTransportBranches, ZeroLengthDatagramOnSmallBufferUpdatesReplyTarget) {
    qb::io::transport::udp receiver;
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    receiver.transport().set_nonblocking(true);
    // Force the slow path: remaining capacity < MaxDatagramSize.
    receiver.set_max_read_buffer_size(128);
    const auto port = receiver.transport().local_endpoint().port();
    ASSERT_NE(port, 0);

    // Send a real zero-length datagram from a known sender port.
    qb::io::udp::socket sender;
    ASSERT_TRUE(sender.init());
    ASSERT_EQ(sender.bind_v4(0, "127.0.0.1"), 0);
    const auto sender_port = sender.local_endpoint().port();
    ASSERT_NE(sender_port, 0);
    ASSERT_EQ(sender.write("", 0, qb::io::endpoint().as_in("127.0.0.1", port)), 0);

    // A zero-length datagram reads back as exactly 0 bytes, which is
    // indistinguishable from "nothing pending" by return value alone — so we
    // poll until the read re-points the reply target at our known sender port,
    // proving a real datagram (not a would-block) was consumed on the slow path.
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    int        got      = -1;
    bool       arrived  = false;
    do {
        got = receiver.read();
        if (got == 0 && receiver.getSource().port() == sender_port) {
            arrived = true;
            break;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);

    ASSERT_TRUE(arrived) << "zero-length datagram never arrived on the slow path";
    EXPECT_EQ(got, 0) << "zero-length datagram must read back as 0 bytes on the slow path";
    EXPECT_EQ(receiver.in().size(), 0u) << "zero-length datagram adds no payload bytes";
    // The reply target was re-pointed at the sender by setDestination(_remote_source).
    EXPECT_EQ(receiver.getSource().port(), sender_port);
    EXPECT_EQ(receiver.getSource().ip(), "127.0.0.1");
}

// write() error propagation: a queued (valid-size) datagram whose underlying
// sendto fails must surface the negative socket error to the caller rather than
// silently consuming the message. We close the transport socket after queuing so
// the sendto inside write() fails.
TEST(UDPTransportBranches, WriteReturnsErrorWhenSendtoFails) {
    qb::io::transport::udp sender;
    ASSERT_TRUE(sender.transport().init());

    // Queue a valid, in-range datagram to a real loopback target.
    qb::io::transport::udp::identity dest{qb::io::endpoint().as_in("127.0.0.1", 9)};
    ASSERT_NE(sender.publish_to(dest, "payload", 7), nullptr);
    ASSERT_EQ(sender.pendingWrite() > 0, true);

    // Break the underlying socket so the sendto inside write() fails.
    sender.transport().close();

    EXPECT_LT(sender.write(), 0) << "write() must propagate the sendto failure as a negative result";
}
