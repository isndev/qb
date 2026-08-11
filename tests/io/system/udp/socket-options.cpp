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
    EXPECT_GE(static_cast<std::size_t>(rcvbuf), requested) << "set_buffer_size did not raise SO_RCVBUF to at least the requested size";
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

// The default-interface (empty iface) multicast branch: a VALID group with an empty
// interface takes the INADDR_ANY (v4) / resolve_iface_index("")==0 (v6) path — distinct
// from the malformed-group / malformed-iface rejections the other cases drive.
//
// The v6 half is where two kernels genuinely disagree, and the disagreement is in the
// KERNEL, not in qb: join/leave_multicast_group are a faithful passthrough here
// (`resolve_iface_index("") == 0`, socket.cpp:56, and the same `ipv6_mreq` reaches the same
// `setsockopt`). Measured with raw `setsockopt` and no qb code in the loop:
//
//   Linux 6.12    join(idx=0) = 0    leave(idx=0) = 0
//                 Index 0 on LEAVE is a WILDCARD — it also drops a membership that was
//                 created with an explicit index, and after it succeeds a leave naming the
//                 explicit index fails. So join("")/leave("") is symmetric.
//
//   Darwin 25.6   join(idx=0) = 0    leave(idx=0) = -1 EADDRNOTAVAIL   (0 successes in 10)
//                 Index 0 is resolved by a route lookup on JOIN and the membership is
//                 recorded against the CONCRETE interface that lookup picked (en0 here, the
//                 default route's interface — not lo0, despite `ff00::/8 -> ::1`). On LEAVE
//                 index 0 is a literal match key, so it matches nothing — not even a
//                 membership that was itself created with index 0.
//
// The Darwin membership is not lost, only un-addressable as "0": a leave naming the resolved
// index releases it (asserted below). So `join("") == 0` does NOT imply `leave("") == 0`, and
// asserting that implication is what made this test fail on any macOS host with a default
// IPv6 multicast route. It shipped in 2.6.0 and never fired in CI, because a runner has no
// such route: the join returns -1 there and the strict branch was simply never reached.
//
// What this test asserts now, per platform:
//   * everywhere  — an empty interface is exactly equivalent to an explicit "0";
//                   the leave's return value must AGREE with whether the membership is
//                   still held (the rejoin oracle below), so a failure must be an honest
//                   failure and a success must be a real release;
//                   and where the host refuses the join, the leave must refuse too — the
//                   run is never vacuous.
//   * Darwin      — a successful index-0 join is followed by a FAILING index-0 leave, and
//                   the membership is still releasable by its resolved interface index.
//   * Linux       — a successful index-0 join is followed by a SUCCEEDING index-0 leave.
//   * elsewhere   — (Windows/BSD: unmeasured) no hard-coded expectation for the leave's
//                   value, but the equivalence and the rejoin oracle still bind.
TEST(UDPSocket, MulticastJoinLeaveWithDefaultInterface) {
    {
        qb::io::udp::socket socket;
        ASSERT_EQ(socket.bind_v4(0, "127.0.0.1"), 0);

        const int joined = socket.join_multicast_group("239.0.0.1", ""); // -> imr_interface = INADDR_ANY
        EXPECT_TRUE(joined == 0 || joined == -1);
        const int left = socket.leave_multicast_group("239.0.0.1", "");
        EXPECT_TRUE(left == 0 || left == -1);
        if (joined == 0) {
            // v4 is symmetric on every kernel measured: IP_DROP_MEMBERSHIP with
            // INADDR_ANY releases what IP_ADD_MEMBERSHIP with INADDR_ANY created.
            EXPECT_EQ(left, 0) << "leaving a group that was successfully joined must succeed";
        } else {
            // Non-vacuous on a host that refuses the join: with no membership on the
            // socket there is nothing to drop, so the leave must fail too.
            EXPECT_EQ(left, -1) << "leaving a group that was never joined must fail";
        }
    }

    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available; the v4 default-interface leg already ran";
    }

    qb::io::udp::socket socket6;
    ASSERT_EQ(socket6.bind_v6(0, "::1"), 0);

    const int joined6 = socket6.join_multicast_group("ff02::1", ""); // -> resolve_iface_index("") == 0 (any)
    EXPECT_TRUE(joined6 == 0 || joined6 == -1);

    // Platform-independent, and asserted even where the host refuses the join: the empty
    // interface must take exactly the index-0 path an explicit "0" takes. This is what keeps
    // the empty-iface arm covered on a runner whose join returns -1 — without it the whole
    // v6 leg would assert nothing there. A second socket is used because the two joins would
    // otherwise collide (a repeat join on the SAME socket is EADDRINUSE); distinct sockets
    // may each hold the same membership.
    {
        qb::io::udp::socket probe6;
        ASSERT_EQ(probe6.bind_v6(0, "::1"), 0);
        EXPECT_EQ(probe6.join_multicast_group("ff02::1", "0"), joined6) << "an empty interface must behave exactly like an explicit index 0";
    }

    const int left6 = socket6.leave_multicast_group("ff02::1", "");
    EXPECT_TRUE(left6 == 0 || left6 == -1);

    if (joined6 != 0) {
        // Non-vacuous where the join was refused: nothing was joined, so nothing can be left.
        EXPECT_EQ(left6, -1) << "leaving a v6 group that was never joined must fail";
        return;
    }

    // The oracle. A repeat join is refused (EADDRINUSE) exactly while the membership is
    // still held, so it reads the kernel's actual state independently of what the leave
    // claimed. Verified on both kernels: a leave that reports success is followed by a
    // successful rejoin, a leave that reports failure by a refused one, and a failed leave
    // does not perturb the membership. This binds on every platform, including those whose
    // index-0 leave semantics have not been measured.
    const int rejoined6 = socket6.join_multicast_group("ff02::1", "");
    if (left6 == 0) {
        EXPECT_EQ(rejoined6, 0) << "a leave that reported success must really have released the membership";
        EXPECT_EQ(socket6.leave_multicast_group("ff02::1", ""), 0);
    } else {
        EXPECT_EQ(rejoined6, -1) << "a leave that reported failure must really have left the membership held";
    }

#if defined(__APPLE__)
    // Darwin's documented asymmetry, pinned to the measured value rather than widened. If a
    // future Darwin resolves index 0 on the leave path the way it does on the join path,
    // this fires — which is the point: the divergence would be gone and this branch, the
    // sweep below and the note above should go with it.
    EXPECT_EQ(left6, -1) << "Darwin: IPV6_LEAVE_GROUP with ipv6mr_interface == 0 never matches";

    // The membership is un-addressable as "0", NOT unreleasable: naming the interface the
    // join resolved to does release it. Asserting this is what distinguishes a kernel that
    // mislays the membership (a real leak, worth reporting) from one that merely indexes it
    // differently. Guarded on the membership still being held, so that a future Darwin whose
    // index-0 leave works reports only the one assertion above and not a cascade.
    if (left6 != 0) {
        bool released_by_index = false;
        for (unsigned int idx = 1; idx <= 64 && !released_by_index; ++idx) {
            released_by_index = socket6.leave_multicast_group("ff02::1", std::to_string(idx)) == 0;
        }
        EXPECT_TRUE(released_by_index) << "Darwin: the index-0 membership must still be releasable via its resolved interface index";
    }
#elif defined(__linux__)
    EXPECT_EQ(left6, 0) << "Linux: index 0 on IPV6_LEAVE_GROUP is a wildcard and must match";
#endif
}

// bind(uri) with a URI whose address family is neither AF_INET/AF_INET6/AF_UNIX falls
// through the switch to the -1 error return.
TEST(UDPSocket, BindWithUnknownAddressFamilyUriFails) {
    qb::io::udp::socket socket;
    const qb::io::uri   unknown_af("", AF_UNSPEC); // af() == AF_UNSPEC -> switch fallthrough
    EXPECT_EQ(socket.bind(unknown_af), -1);
    EXPECT_FALSE(socket.is_open());
}
