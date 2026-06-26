/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/udp/transport-datagram.cpp
 * @brief `qb::io::transport::udp` — out/in proxy buffers, publish accounting and buffer limits.
 *
 * This is the transport-proxy half of the former test-udp-socket.cpp. It drives
 * `qb::io::transport::udp` over real loopback datagram sockets: out-buffer accumulation and
 * per-datagram `write()` accounting, `publish_to`/`setDestination`, zero-length datagrams updating
 * the reply destination, ordered multi-datagram drain, read-into-partial-buffer append, overflow
 * rollback (`EMSGSIZE`), destination-reset re-allocation, identity equality/hash, oversized-publish
 * rejection, and the inbound `ErrBufferLimitExceeded` path. It also covers the minimal
 * `qb::io::async::udp::{server,client}` wrappers constructing and tearing down cleanly.
 *
 * It additionally absorbs test-io.cpp's `UDPTransport.SmallReadLimitAcceptsSmallDatagram` (which was
 * a fixed-port + busy-poll clone of the same publish→read round-trip) as
 * `SmallReadLimitAcceptsSmallDatagram`, now on an ephemeral port via the shared deadline helper.
 *
 * De-flake (per the restructure spec §2): the original fixed-budget busy-wait helpers
 * (`wait_for_datagram` / `wait_for_zero_length_datagram` — 100 × 2ms polls that surfaced a delayed
 * loopback delivery as a confusing `ASSERT_EQ(count)` mismatch) are replaced by `read_datagram()` /
 * `read_zero_length_datagram()`: a deadline-bounded poll that returns the read result and, on
 * timeout, produces a *descriptive* gtest failure naming exactly what never arrived.
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
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/udp/client.h>
#include <qb/io/async/udp/server.h>
#include <qb/io/system/sys__socket.h>
#include <qb/io/transport/udp.h>
#include <qb/io/udp/socket.h>

using namespace std::chrono_literals;

namespace {

// Deadline-bounded read of one datagram. Returns the first non-zero `read()`
// result (bytes, or a negative qb error code), or 0 on a deadline timeout. The
// caller asserts on the returned value, so a stalled delivery surfaces as a
// precise failure rather than a busy-loop that "looks" like a zero-length read.
int
read_datagram(qb::io::transport::udp &receiver, std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const int ret = receiver.read();
        if (ret != 0) {
            return ret;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return 0;
}

// Deadline-bounded read of a *zero-length* datagram: a 0-byte payload reads back
// as `read()==0` but with a real source endpoint. Distinguishes "arrived empty"
// (source port set) from "nothing arrived yet" (source port still 0).
int
read_zero_length_datagram(qb::io::transport::udp &receiver, std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const int ret = receiver.read();
        if (ret != 0) {
            return ret; // a real (non-empty) datagram or an error
        }
        if (receiver.getSource().port() != 0) {
            return 0; // empty datagram delivered, source recorded
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return -1; // never delivered
}

struct MinimalAsyncUdpServer : qb::io::async::udp::server<MinimalAsyncUdpServer> {};
struct MinimalAsyncUdpClient : qb::io::async::udp::client<MinimalAsyncUdpClient> {};

} // namespace

TEST(UDPTransport, ProxyOutBuildsAndSendsOneDatagram) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);

    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", receiver.transport().local_endpoint().port())};
    sender.setDestination(dest);

    auto &out = sender.out();
    out << std::string("hello");
    out << std::string(" ");
    out << std::string("udp");
    EXPECT_GT(out.size(), 9u);

    ASSERT_EQ(sender.write(), 9);
    ASSERT_EQ(read_datagram(receiver), 9) << "receiver never got the 9-byte datagram";
    EXPECT_EQ(std::string_view(receiver.in().begin(), receiver.pendingRead()), "hello udp");
    EXPECT_EQ(receiver.getSource().ip(), "127.0.0.1");
}

TEST(UDPTransport, ZeroLengthDatagramUpdatesReplyDestination) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);

    qb::io::transport::udp::identity receiver_dest{qb::io::endpoint("127.0.0.1", receiver.transport().local_endpoint().port())};

    ASSERT_NE(sender.publish_to(receiver_dest, "", 0), nullptr);
    ASSERT_EQ(sender.write(), 0);

    ASSERT_EQ(read_zero_length_datagram(receiver), 0) << "receiver never got the zero-length datagram";
    EXPECT_EQ(receiver.pendingRead(), 0u);
    EXPECT_EQ(receiver.getSource().ip(), "127.0.0.1");
    EXPECT_NE(receiver.getSource().port(), 0);

    receiver.out() << std::string("reply");
    ASSERT_EQ(receiver.write(), 5);

    ASSERT_EQ(read_datagram(sender), 5) << "sender never got the 5-byte reply";
    EXPECT_EQ(std::string_view(sender.in().begin(), sender.pendingRead()), "reply");
}

TEST(UDPTransport, AsyncUdpWrappersConstructAndStopCleanly) {
    qb::io::async::init();
    {
        MinimalAsyncUdpServer server;
        MinimalAsyncUdpClient client;
        EXPECT_FALSE(server.transport().is_open());
        EXPECT_FALSE(client.transport().is_open());
    }
    qb::io::async::listener::current.clear();
}

TEST(UDPTransport, WriteConsumesMultipleQueuedDatagramsInOrder) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);

    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", receiver.transport().local_endpoint().port())};

    ASSERT_NE(sender.publish_to(dest, "one", 3), nullptr);
    ASSERT_NE(sender.publish_to(dest, "two", 3), nullptr);
    ASSERT_EQ(sender.write(), 3);
    ASSERT_GT(sender.pendingWrite(), 0u);
    ASSERT_EQ(sender.write(), 3);
    EXPECT_EQ(sender.pendingWrite(), 0u);

    ASSERT_EQ(read_datagram(receiver), 3) << "first queued datagram never arrived";
    EXPECT_EQ(std::string_view(receiver.in().begin(), receiver.pendingRead()), "one");
    receiver.flush(3);

    ASSERT_EQ(read_datagram(receiver), 3) << "second queued datagram never arrived";
    EXPECT_EQ(std::string_view(receiver.in().begin(), receiver.pendingRead()), "two");
}

TEST(UDPTransport, ReadAppendsIntoPartiallyFilledInputBuffer) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    receiver.set_max_read_buffer_size(16);
    receiver.in() << std::string("head");

    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", receiver.transport().local_endpoint().port())};
    ASSERT_NE(sender.publish_to(dest, "tail", 4), nullptr);
    ASSERT_EQ(sender.write(), 4);

    ASSERT_EQ(read_datagram(receiver), 4) << "tail datagram never arrived";
    ASSERT_EQ(receiver.pendingRead(), 8u);
    EXPECT_EQ(std::string_view(receiver.in().begin(), receiver.pendingRead()), "headtail");
}

TEST(UDPTransport, ProxyOutOverflowRollsBackLastAppend) {
    qb::io::transport::udp           transport;
    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", 9)};

    transport.setDestination(dest);
    auto &out = transport.out();
    out << std::string("small");
    const auto before = transport.pendingWrite();

    transport.set_max_write_buffer_size(before + 4);
    out << std::string("too-large");

    EXPECT_EQ(transport.pendingWrite(), before);
    EXPECT_EQ(qb::io::socket::get_last_errno(), EMSGSIZE);
}

TEST(UDPTransport, ProxyOutAllocatesNewDatagramAfterDestinationReset) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver_a;
    qb::io::transport::udp receiver_b;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver_a.transport().init());
    ASSERT_TRUE(receiver_b.transport().init());
    ASSERT_EQ(receiver_a.transport().bind_v4(0, "127.0.0.1"), 0);
    ASSERT_EQ(receiver_b.transport().bind_v4(0, "127.0.0.1"), 0);

    qb::io::transport::udp::identity dest_a{qb::io::endpoint("127.0.0.1", receiver_a.transport().local_endpoint().port())};
    qb::io::transport::udp::identity dest_b{qb::io::endpoint("127.0.0.1", receiver_b.transport().local_endpoint().port())};

    sender.setDestination(dest_a);
    auto &saved_out = sender.out();
    saved_out << std::string("first");
    sender.setDestination(dest_b);
    saved_out << std::string("second");

    ASSERT_EQ(sender.write(), 5);
    ASSERT_EQ(sender.write(), 6);

    ASSERT_EQ(read_datagram(receiver_a), 5) << "receiver_a never got 'first'";
    EXPECT_EQ(std::string_view(receiver_a.in().begin(), receiver_a.pendingRead()), "first");

    ASSERT_EQ(read_datagram(receiver_b), 6) << "receiver_b never got 'second'";
    EXPECT_EQ(std::string_view(receiver_b.in().begin(), receiver_b.pendingRead()), "second");
}

TEST(UDPTransport, EmptyWriteAndIdentityComparisonAreStable) {
    qb::io::transport::udp transport;
    EXPECT_EQ(transport.write(), 0);

    qb::io::transport::udp::identity first{qb::io::endpoint("127.0.0.1", 1234)};
    qb::io::transport::udp::identity same{qb::io::endpoint("127.0.0.1", 1234)};
    qb::io::transport::udp::identity other{qb::io::endpoint("127.0.0.1", 4321)};

    EXPECT_EQ(first, same);
    EXPECT_NE(first, other);
    EXPECT_EQ(qb::io::transport::udp::identity::hasher{}(first), qb::io::transport::udp::identity::hasher{}(same));
}

// Absorbed from test-io.cpp UDPTransport.IdentityEquality / IdentityHashConsistency /
// IdentityInUnorderedSet — the identity value type's full equality + hashing contract,
// including its use as an unordered_set key (deduplicating equal identities).
TEST(UDPTransport, IdentityEqualityHashingAndSetMembership) {
    using identity = qb::io::transport::udp::identity;

    identity id_a{qb::io::endpoint("127.0.0.1", 5000)};
    identity id_a_dup{qb::io::endpoint("127.0.0.1", 5000)};
    identity id_other_port{qb::io::endpoint("127.0.0.1", 5001)};
    identity id_other_host{qb::io::endpoint("10.0.0.1", 5000)};

    EXPECT_EQ(id_a, id_a_dup);
    EXPECT_NE(id_a, id_other_port);
    EXPECT_NE(id_a, id_other_host);

    identity::hasher hash;
    EXPECT_EQ(hash(id_a), hash(id_a_dup));
    EXPECT_NE(hash(id_a), hash(id_other_host));

    std::unordered_set<identity, identity::hasher> id_set;
    id_set.insert(id_a);
    id_set.insert(id_other_port);
    id_set.insert(id_a_dup); // equal to id_a -> no new entry
    EXPECT_EQ(id_set.size(), 2u);
}

TEST(UDPTransport, PublishRejectsOversizedOrBufferLimitedDatagrams) {
    qb::io::transport::udp           transport;
    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", 9)};

    std::string oversized(qb::io::udp::socket::MaxDatagramSize + 1, 'x');
    EXPECT_EQ(transport.publish_to(dest, oversized.data(), oversized.size()), nullptr);

    transport.set_max_write_buffer_size(8);
    constexpr char payload[] = "fits-size-but-not-header";
    EXPECT_EQ(transport.publish_to(dest, payload, std::strlen(payload)), nullptr);
}

// Absorbed from test-io.cpp UDPTransport.SmallReadLimitAcceptsSmallDatagram (was
// fixed port 64328 + 50×10ms busy poll): a small datagram is accepted under a
// small read-buffer limit. Now ephemeral-port + deadline helper.
TEST(UDPTransport, SmallReadLimitAcceptsSmallDatagram) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    receiver.set_max_read_buffer_size(16);

    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", receiver.transport().local_endpoint().port())};
    sender.setDestination(dest);

    constexpr char payload[] = "hello";
    ASSERT_NE(sender.publish(payload, 5), nullptr);
    ASSERT_EQ(sender.write(), 5);

    ASSERT_EQ(read_datagram(receiver), 5) << "small datagram never arrived under a 16-byte read limit";
    EXPECT_EQ(receiver.pendingRead(), 5u);
    EXPECT_EQ(std::string_view(receiver.in().begin(), receiver.pendingRead()), "hello");
}

TEST(UDPTransport, ReadReportsBufferLimitForOversizedDatagram) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    receiver.set_max_read_buffer_size(4);

    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", receiver.transport().local_endpoint().port())};
    constexpr char                   payload[] = "too-large";
    ASSERT_NE(sender.publish_to(dest, payload, sizeof(payload) - 1), nullptr);
    ASSERT_EQ(sender.write(), static_cast<int>(sizeof(payload) - 1));

    EXPECT_EQ(read_datagram(receiver), qb::io::ErrBufferLimitExceeded);
    EXPECT_EQ(receiver.pendingRead(), 0u);
}
