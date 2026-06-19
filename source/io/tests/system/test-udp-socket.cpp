/**
 * @file qb/source/io/tests/system/test-udp-socket.cpp
 * @brief System tests for qb UDP socket and datagram transport utilities.
 *
 * The tests focus on behaviour that is easy to regress in the low-level UDP
 * layer: ownership transfer, option guards, timeout restoration, multicast
 * validation and datagram buffer accounting.
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
 *
 * @ingroup Tests
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <qb/io/async/udp/client.h>
#include <qb/io/async/udp/server.h>
#include <qb/io/system/sys__socket.h>
#include <qb/io/transport/udp.h>
#include <qb/io/udp/socket.h>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

int
wait_for_datagram(qb::io::transport::udp &receiver) {
    int ret = -1;
    for (int i = 0; i < 100; ++i) {
        ret = receiver.read();
        if (ret != 0) {
            return ret;
        }
        if (ret == 0) {
            std::this_thread::sleep_for(2ms);
        }
    }
    return ret;
}

int
wait_for_zero_length_datagram(qb::io::transport::udp &receiver) {
    int ret = -1;
    for (int i = 0; i < 100; ++i) {
        ret = receiver.read();
        if (ret == 0 && receiver.getSource().port() != 0) {
            return ret;
        }
        if (ret != 0) {
            return ret;
        }
        std::this_thread::sleep_for(2ms);
    }
    return ret;
}

struct MinimalAsyncUdpServer : qb::io::async::udp::server<MinimalAsyncUdpServer> {};
struct MinimalAsyncUdpClient : qb::io::async::udp::client<MinimalAsyncUdpClient> {};

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

TEST(UDPSocket, UriBindSupportsIpv4AndIpv6) {
    qb::io::udp::socket socket;
    ASSERT_EQ(socket.bind(qb::io::uri{"udp://127.0.0.1:0"}), 0);
    EXPECT_TRUE(socket.is_bound());
    EXPECT_EQ(socket.local_endpoint().ip(), "127.0.0.1");

    qb::io::udp::socket ipv6_socket;
    ASSERT_EQ(ipv6_socket.bind(qb::io::uri{"udp://[::1]:0"}), 0);
    EXPECT_TRUE(ipv6_socket.is_bound());
    EXPECT_EQ(ipv6_socket.address_family(), AF_INET6);
}

TEST(UDPSocket, Ipv6MulticastValidationDoesNotThrowForNamedInterfaces) {
    qb::io::udp::socket socket;
    ASSERT_EQ(socket.bind_v6(0, "::1"), 0);

    EXPECT_EQ(socket.join_multicast_group("not-an-ip", "1"), -1);
    EXPECT_EQ(socket.leave_multicast_group("not-an-ip", "1"), -1);

#if !defined(_WIN32)
#if defined(__APPLE__)
    constexpr const char *loopback_interface = "lo0";
#else
    constexpr const char *loopback_interface = "lo";
#endif
    EXPECT_LE(socket.join_multicast_group("ff02::1", loopback_interface), 0);
    EXPECT_LE(socket.leave_multicast_group("ff02::1", loopback_interface), 0);
#endif
    EXPECT_LE(socket.join_multicast_group("ff02::1", "1"), 0);
    EXPECT_LE(socket.leave_multicast_group("ff02::1", "1"), 0);
    EXPECT_EQ(socket.set_multicast_ttl(-5), 0);
    EXPECT_EQ(socket.set_multicast_ttl(500), 0);
    EXPECT_EQ(socket.set_multicast_loopback(false), 0);
    EXPECT_EQ(socket.set_multicast_loopback(true), 0);
}

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
    ASSERT_EQ(wait_for_datagram(receiver), 9);
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

    ASSERT_EQ(wait_for_zero_length_datagram(receiver), 0);
    EXPECT_EQ(receiver.pendingRead(), 0u);
    EXPECT_EQ(receiver.getSource().ip(), "127.0.0.1");
    EXPECT_NE(receiver.getSource().port(), 0);

    receiver.out() << std::string("reply");
    ASSERT_EQ(receiver.write(), 5);

    ASSERT_EQ(wait_for_datagram(sender), 5);
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

    ASSERT_EQ(wait_for_datagram(receiver), 3);
    EXPECT_EQ(std::string_view(receiver.in().begin(), receiver.pendingRead()), "one");
    receiver.flush(3);

    ASSERT_EQ(wait_for_datagram(receiver), 3);
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

    ASSERT_EQ(wait_for_datagram(receiver), 4);
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

    ASSERT_EQ(wait_for_datagram(receiver_a), 5);
    EXPECT_EQ(std::string_view(receiver_a.in().begin(), receiver_a.pendingRead()), "first");

    ASSERT_EQ(wait_for_datagram(receiver_b), 6);
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

TEST(UDPTransport, PublishRejectsOversizedOrBufferLimitedDatagrams) {
    qb::io::transport::udp           transport;
    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", 9)};

    std::string oversized(qb::io::udp::socket::MaxDatagramSize + 1, 'x');
    EXPECT_EQ(transport.publish_to(dest, oversized.data(), oversized.size()), nullptr);

    transport.set_max_write_buffer_size(8);
    constexpr char payload[] = "fits-size-but-not-header";
    EXPECT_EQ(transport.publish_to(dest, payload, std::strlen(payload)), nullptr);
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

    EXPECT_EQ(wait_for_datagram(receiver), qb::io::ErrBufferLimitExceeded);
    EXPECT_EQ(receiver.pendingRead(), 0u);
}
