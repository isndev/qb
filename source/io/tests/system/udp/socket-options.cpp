/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/udp/socket-options.cpp
 * @brief `qb::io::udp::socket` — bind, ownership, options, multicast and timed reads over loopback.
 *
 * This is the raw-datagram-socket half of the former test-udp-socket.cpp. It drives
 * `qb::io::udp::socket` directly: closed-socket guard behaviour, ownership transfer from a generic
 * `qb::io::socket`, `bind_v4`/`bind_v6`/`bind(uri)`, the option setters (buffer size, broadcast,
 * multicast TTL/loopback, group join/leave), family-mismatch rejection, and the
 * `try_read`/`read_timeout` blocking-mode-preservation + real loopback round-trip with peer
 * reporting. No event loop is required — these are direct socket syscalls over 127.0.0.1 / ::1.
 *
 * Hardening over the original (per the restructure spec §2/§7):
 *   - IPv6 multicast is gated behind a real capability probe (`ipv6_multicast_available()`): where the
 *     platform supports it the join/leave success path is asserted *exactly* (`EXPECT_EQ(..., 0)`)
 *     instead of the old lenient `EXPECT_LE(..., 0)` that passed on benign failure too;
 *   - `set_buffer_size` is verified to actually take effect via an `SO_RCVBUF` readback (the kernel
 *     may round the request up, so the assertion is "grew to at least the request", never an exact
 *     equality that would be inherently brittle);
 *   - `read_timeout` peer reporting is additionally covered on IPv6 (gated on `::1` availability).
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

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <qb/io/system/sys__socket.h>
#include <qb/io/udp/socket.h>

using namespace std::chrono_literals;

namespace {

// Loopback interface name differs per OS (used for the named-interface multicast legs).
#if defined(__APPLE__)
constexpr const char *kLoopbackInterface = "lo0";
#else
constexpr const char *kLoopbackInterface = "lo";
#endif

// Probe whether `::1` can be bound at all — hosts/CI images without IPv6
// loopback should skip the v6 legs cleanly.
bool
ipv6_loopback_available() {
    qb::io::udp::socket probe;
    return probe.bind_v6(0, "::1") == 0;
}

// Probe whether IPv6 multicast group join actually succeeds on this host. Some
// CI containers have `::1` but no multicast-capable interface; in that case the
// strict success assertions are skipped rather than spuriously failing.
bool
ipv6_multicast_available() {
    qb::io::udp::socket probe;
    if (probe.bind_v6(0, "::1") != 0) {
        return false;
    }
    const int joined = probe.join_multicast_group("ff02::1", "1");
    if (joined == 0) {
        probe.leave_multicast_group("ff02::1", "1");
        return true;
    }
    return false;
}

} // namespace

TEST(UDPSocket, ClosedSocketOperationsFailCleanly) {
    qb::io::udp::socket socket;
    char                buffer[8] = {};
    qb::io::endpoint    peer;

    EXPECT_FALSE(socket.is_open());
    EXPECT_FALSE(socket.is_bound());
    EXPECT_EQ(socket.address_family(), -1);
    EXPECT_EQ(socket.read_timeout(buffer, sizeof(buffer), peer, 1ms), -1);
    EXPECT_EQ(socket.try_read(buffer, sizeof(buffer), peer), -1);
    EXPECT_EQ(socket.set_buffer_size(4096), -1);
    EXPECT_EQ(socket.set_broadcast(true), -1);
    EXPECT_EQ(socket.join_multicast_group("224.0.0.251"), -1);
    EXPECT_EQ(socket.leave_multicast_group("224.0.0.251"), -1);
    EXPECT_EQ(socket.set_multicast_ttl(1), -1);
    EXPECT_EQ(socket.set_multicast_loopback(true), -1);
    EXPECT_EQ(socket.disconnect(), -1);
}

TEST(UDPSocket, MoveFromGenericSocketTransfersOwnership) {
    qb::io::socket raw;
    ASSERT_TRUE(raw.open(AF_INET, SOCK_DGRAM, 0));
    const auto raw_handle = raw.native_handle();

    qb::io::udp::socket udp_socket(std::move(raw));
    EXPECT_FALSE(raw.is_open());
    EXPECT_TRUE(udp_socket.is_open());
    EXPECT_EQ(udp_socket.native_handle(), raw_handle);

    qb::io::socket replacement;
    ASSERT_TRUE(replacement.open(AF_INET, SOCK_DGRAM, 0));
    const auto replacement_handle = replacement.native_handle();
    udp_socket                    = std::move(replacement);

    EXPECT_FALSE(replacement.is_open());
    EXPECT_TRUE(udp_socket.is_open());
    EXPECT_EQ(udp_socket.native_handle(), replacement_handle);
}

TEST(UDPSocket, BindAndOptionsExposeExpectedState) {
    qb::io::udp::socket socket;

    ASSERT_EQ(socket.bind_v4(0, "127.0.0.1"), 0);
    EXPECT_TRUE(socket.is_open());
    EXPECT_TRUE(socket.is_bound());
    EXPECT_EQ(socket.address_family(), AF_INET);
    EXPECT_EQ(socket.local_endpoint().ip(), "127.0.0.1");
    EXPECT_NE(socket.local_endpoint().port(), 0);

    EXPECT_EQ(socket.set_buffer_size(4096), 0);
    EXPECT_EQ(socket.set_broadcast(false), 0);
    EXPECT_EQ(socket.set_broadcast(true), 0);
    EXPECT_EQ(socket.set_multicast_ttl(0), 0);
    EXPECT_EQ(socket.set_multicast_ttl(512), 0);
    EXPECT_EQ(socket.set_multicast_loopback(false), 0);
    EXPECT_EQ(socket.set_multicast_loopback(true), 0);

    EXPECT_EQ(socket.join_multicast_group("not-an-ip"), -1);
    EXPECT_EQ(socket.join_multicast_group("224.0.0.251", "not-an-ip"), -1);
    EXPECT_EQ(socket.leave_multicast_group("not-an-ip"), -1);
    EXPECT_EQ(socket.leave_multicast_group("224.0.0.251", "not-an-ip"), -1);
}

// set_buffer_size must change SO_RCVBUF, not just return success. The kernel is
// free to round the request up (and commonly doubles it for bookkeeping), so we
// assert it grew to at least the requested size rather than an exact value.
TEST(UDPSocket, SetBufferSizeGrowsReceiveBuffer) {
    qb::io::udp::socket socket;
    ASSERT_EQ(socket.bind_v4(0, "127.0.0.1"), 0);

    constexpr std::size_t requested = 64 * 1024;
    ASSERT_EQ(socket.set_buffer_size(requested), 0);

    int rcvbuf = 0;
    ASSERT_EQ(socket.get_optval(SOL_SOCKET, SO_RCVBUF, rcvbuf), 0);
    EXPECT_GE(static_cast<std::size_t>(rcvbuf), requested)
        << "set_buffer_size did not raise SO_RCVBUF to at least the requested size";
}

TEST(UDPSocket, BindRejectsFamilyMismatchForAlreadyOpenSocket) {
    qb::io::udp::socket socket;
    ASSERT_TRUE(socket.init(AF_INET));
    EXPECT_EQ(socket.bind_v6(0, "::1"), -1);
}

TEST(UDPSocket, TryReadAndTimeoutRestoreBlockingMode) {
    qb::io::udp::socket socket;
    ASSERT_EQ(socket.bind_v4(0, "127.0.0.1"), 0);
    ASSERT_EQ(socket.test_nonblocking(), 0);

    char             buffer[16] = {};
    qb::io::endpoint peer;

    EXPECT_LE(socket.try_read(buffer, sizeof(buffer), peer), 0);
    EXPECT_EQ(socket.test_nonblocking(), 0);

    EXPECT_EQ(socket.read_timeout(buffer, sizeof(buffer), peer, 1ms), -ETIMEDOUT);
    EXPECT_EQ(socket.test_nonblocking(), 0);
}

TEST(UDPSocket, ReadTimeoutReceivesDatagramAndReportsPeer) {
    qb::io::udp::socket receiver;
    qb::io::udp::socket sender;

    ASSERT_EQ(receiver.bind_v4(0, "127.0.0.1"), 0);
    ASSERT_TRUE(sender.init());

    const auto port = receiver.local_endpoint().port();
    ASSERT_NE(port, 0);

    constexpr std::string_view payload = "qb udp timeout";
    qb::io::endpoint           dest;
    dest.as_in("127.0.0.1", port);
    ASSERT_EQ(sender.write(payload.data(), payload.size(), dest), static_cast<int>(payload.size()));

    char             buffer[64] = {};
    qb::io::endpoint peer;
    const int        read = receiver.read_timeout(buffer, sizeof(buffer), peer, 500ms);

    ASSERT_EQ(read, static_cast<int>(payload.size()));
    EXPECT_EQ(std::string_view(buffer, static_cast<std::size_t>(read)), payload);
    EXPECT_EQ(peer.ip(), "127.0.0.1");
    EXPECT_NE(peer.port(), 0);
}

TEST(UDPSocket, ReadTimeoutReceivesDatagramAndReportsPeerIpv6) {
    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available on this host";
    }

    qb::io::udp::socket receiver;
    qb::io::udp::socket sender;

    ASSERT_EQ(receiver.bind_v6(0, "::1"), 0);
    ASSERT_TRUE(sender.init(AF_INET6));

    const auto port = receiver.local_endpoint().port();
    ASSERT_NE(port, 0);

    constexpr std::string_view payload = "qb udp v6 timeout";
    qb::io::endpoint           dest;
    dest.as_in("::1", port);
    ASSERT_EQ(sender.write(payload.data(), payload.size(), dest), static_cast<int>(payload.size()));

    char             buffer[64] = {};
    qb::io::endpoint peer;
    const int        read = receiver.read_timeout(buffer, sizeof(buffer), peer, 500ms);

    ASSERT_EQ(read, static_cast<int>(payload.size()));
    EXPECT_EQ(std::string_view(buffer, static_cast<std::size_t>(read)), payload);
    EXPECT_EQ(peer.af(), AF_INET6);
    EXPECT_NE(peer.port(), 0);
}

TEST(UDPSocket, UriBindSupportsIpv4AndIpv6) {
    qb::io::udp::socket socket;
    ASSERT_EQ(socket.bind(qb::io::uri{"udp://127.0.0.1:0"}), 0);
    EXPECT_TRUE(socket.is_bound());
    EXPECT_EQ(socket.local_endpoint().ip(), "127.0.0.1");

    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available; v4 leg already asserted";
    }
    qb::io::udp::socket ipv6_socket;
    ASSERT_EQ(ipv6_socket.bind(qb::io::uri{"udp://[::1]:0"}), 0);
    EXPECT_TRUE(ipv6_socket.is_bound());
    EXPECT_EQ(ipv6_socket.address_family(), AF_INET6);
}

TEST(UDPSocket, Ipv6MulticastRejectsBadInputsAndAcceptsValidGroups) {
    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available on this host";
    }

    qb::io::udp::socket socket;
    ASSERT_EQ(socket.bind_v6(0, "::1"), 0);

    // Malformed group / interface must always be rejected, independent of
    // multicast capability.
    EXPECT_EQ(socket.join_multicast_group("not-an-ip", "1"), -1);
    EXPECT_EQ(socket.leave_multicast_group("not-an-ip", "1"), -1);

    // TTL / loopback option setters are accepted regardless.
    EXPECT_EQ(socket.set_multicast_ttl(-5), 0);
    EXPECT_EQ(socket.set_multicast_ttl(500), 0);
    EXPECT_EQ(socket.set_multicast_loopback(false), 0);
    EXPECT_EQ(socket.set_multicast_loopback(true), 0);

    if (!ipv6_multicast_available()) {
        GTEST_SKIP() << "IPv6 multicast group join is not supported on this host's loopback";
    }

    // Capability is present: the success path must succeed *exactly*, not merely
    // "not throw" (the old EXPECT_LE accepted benign failure too).
    EXPECT_EQ(socket.join_multicast_group("ff02::1", "1"), 0);
    EXPECT_EQ(socket.leave_multicast_group("ff02::1", "1"), 0);

#if !defined(_WIN32)
    EXPECT_EQ(socket.join_multicast_group("ff02::1", kLoopbackInterface), 0);
    EXPECT_EQ(socket.leave_multicast_group("ff02::1", kLoopbackInterface), 0);
#endif
}
