/**
 * @file qb/source/io/tests/system/test-tcp-socket.cpp
 * @brief System tests for qb TCP sockets and the low-level socket wrapper.
 *
 * These tests exercise deterministic loopback behaviour for the blocking,
 * non-blocking and timeout-oriented socket APIs that back the higher-level TCP
 * transports. They intentionally use dynamic ports to avoid CI collisions.
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

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

template <typename Server, typename Client>
void
with_tcp_pair(Server &&server, Client &&client) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    ASSERT_TRUE(listener.is_open());

    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] {
        qb::io::tcp::socket accepted;
        ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
        server(std::move(accepted));
    });

    client(port);
    server_thread.join();
}

void
accept_low_level_connections(qb::io::socket &listener, int expected_count) {
    listener.set_nonblocking(true);
    int        accepted_count = 0;
    const auto deadline       = std::chrono::steady_clock::now() + 3s;

    while (accepted_count < expected_count && std::chrono::steady_clock::now() < deadline) {
        ::socket_type accepted_handle = qb::io::inet::invalid_socket;
        const int     ret             = listener.accept_n(accepted_handle);
        if (ret == 0) {
            qb::io::socket accepted(accepted_handle);
            ++accepted_count;
            continue;
        }
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(accepted_count, expected_count);
}

void
accept_tcp_connections(qb::io::tcp::listener &listener, int expected_count) {
    for (int i = 0; i < expected_count; ++i) {
        qb::io::tcp::socket accepted;
        ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
        EXPECT_TRUE(accepted.is_open());
        accepted.disconnect();
    }
}

unsigned short
reserve_free_tcp_port() {
    qb::io::socket probe;
    EXPECT_EQ(probe.pserve("127.0.0.1", 0), 0);
    const auto port = probe.local_endpoint().port();
    probe.close();
    return port;
}

} // namespace

TEST(TCPSocket, InitBindAndUriContracts) {
    qb::io::tcp::socket socket;
    EXPECT_EQ(socket.init(), 0);
    EXPECT_TRUE(socket.is_open());
    EXPECT_EQ(socket.init(), 0);

    const auto ipv6_loopback = qb::io::endpoint().as_in("::1", 0);
    EXPECT_EQ(socket.bind(ipv6_loopback), -1);

    qb::io::tcp::socket bound;
    EXPECT_EQ(bound.bind(qb::io::endpoint().as_in("127.0.0.1", 0)), 0);
    EXPECT_TRUE(bound.is_open());
    EXPECT_EQ(bound.local_endpoint().af(), AF_INET);

    qb::io::tcp::socket uri_bound;
    EXPECT_EQ(uri_bound.bind(qb::io::uri("tcp://127.0.0.1:0")), 0);
    EXPECT_TRUE(uri_bound.is_open());
    EXPECT_NE(uri_bound.local_endpoint().port(), 0);

    qb::io::tcp::socket invalid_uri_bound;
    EXPECT_EQ(invalid_uri_bound.bind(qb::io::uri("file:/tmp/qb.sock")), -1);
}

TEST(TCPSocket, UriAndTimeoutConnectVariantsReachLoopbackServer) {
    with_tcp_pair(
        [](qb::io::tcp::socket accepted) {
            accepted.set_nonblocking(false);
            char buffer[16] = {};
            EXPECT_EQ(accepted.read(buffer, sizeof("ping")), static_cast<int>(sizeof("ping")));
            EXPECT_STREQ(buffer, "ping");
            EXPECT_GE(accepted.write("pong", sizeof("pong")), static_cast<int>(sizeof("pong")));
        },
        [](unsigned short port) {
            qb::io::tcp::socket client;
            const auto          uri = qb::io::uri("tcp://127.0.0.1:" + std::to_string(port));

            ASSERT_EQ(client.connect(uri, 1s), qb::io::SocketStatus::Done);
            EXPECT_TRUE(client.is_open());
            EXPECT_EQ(client.peer_endpoint().port(), port);
            EXPECT_GE(client.write("ping", sizeof("ping")), static_cast<int>(sizeof("ping")));

            char buffer[16] = {};
            EXPECT_EQ(client.read(buffer, sizeof("pong")), static_cast<int>(sizeof("pong")));
            EXPECT_STREQ(buffer, "pong");
            client.disconnect();
        });
}

TEST(TCPSocket, BlockingConnectVariantsReachLoopbackServer) {
    constexpr int expected_connections = 3;

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] {
        for (int i = 0; i < expected_connections; ++i) {
            qb::io::tcp::socket accepted;
            ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
            EXPECT_TRUE(accepted.is_open());
            accepted.disconnect();
        }
    });

    qb::io::tcp::socket endpoint_client;
    EXPECT_EQ(endpoint_client.connect(qb::io::endpoint().as_in("127.0.0.1", port)), 0);
    endpoint_client.disconnect();

    qb::io::tcp::socket uri_client;
    EXPECT_EQ(uri_client.connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port))), 0);
    uri_client.disconnect();

    qb::io::tcp::socket v4_client;
    EXPECT_EQ(v4_client.connect_v4("127.0.0.1", port), 0);
    v4_client.disconnect();

    server_thread.join();
}

TEST(TCPSocket, NonBlockingUriConnectReportsProgressOrImmediateSuccess) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);

    qb::io::tcp::socket client;
    const auto          uri = qb::io::uri("tcp://127.0.0.1:" + std::to_string(listener.local_endpoint().port()));

    const int ret = client.n_connect(uri);
    const int err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected n_connect result=" << ret << " errno=" << err;
    EXPECT_TRUE(client.is_open());
    client.close();
}

TEST(TCPSocket, NonBlockingEndpointAndV4ConnectReportProgressOrImmediateSuccess) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();

    qb::io::tcp::socket endpoint_client;
    const int           endpoint_ret = endpoint_client.n_connect(qb::io::endpoint("127.0.0.1", port));
    const int           endpoint_err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(endpoint_ret == 0 || endpoint_err == EINPROGRESS || qb::io::socket::not_send_error(endpoint_err))
        << "unexpected n_connect endpoint result=" << endpoint_ret << " errno=" << endpoint_err;
    endpoint_client.close();

    qb::io::tcp::socket v4_client;
    const int           v4_ret = v4_client.n_connect_v4("127.0.0.1", port);
    const int           v4_err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(v4_ret == 0 || v4_err == EINPROGRESS || qb::io::socket::not_send_error(v4_err))
        << "unexpected n_connect_v4 result=" << v4_ret << " errno=" << v4_err;
    v4_client.close();
}

TEST(TCPSocket, IPv6ConnectVariantsReachLoopbackServer) {
    constexpr int expected_connections = 3;

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v6(0, "::1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_tcp_connections(listener, expected_connections); });

    qb::io::tcp::socket endpoint_client;
    EXPECT_EQ(endpoint_client.connect(qb::io::endpoint().as_in("::1", port)), 0);
    endpoint_client.disconnect();

    qb::io::tcp::socket uri_timeout_client;
    const auto          uri = qb::io::uri("tcp://[::1]:" + std::to_string(port));
    EXPECT_EQ(uri_timeout_client.connect(uri, 1s), qb::io::SocketStatus::Done);
    uri_timeout_client.disconnect();

    qb::io::tcp::socket v6_client;
    EXPECT_EQ(v6_client.connect_v6("::1", port), 0);
    v6_client.disconnect();

    server_thread.join();

    qb::io::tcp::socket n_v6_client;
    const int           ret = n_v6_client.n_connect_v6("::1", port);
    const int           err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected n_connect_v6 result=" << ret << " errno=" << err;
    n_v6_client.disconnect();
}

TEST(TCPSocket, AlreadyOpenSocketRejectsMismatchedEndpointFamilies) {
    const auto ipv6_loopback = qb::io::endpoint().as_in("::1", 1);

    qb::io::tcp::socket blocking_client;
    ASSERT_EQ(blocking_client.init(AF_INET), 0);
    EXPECT_EQ(blocking_client.connect(ipv6_loopback), -1);

    qb::io::tcp::socket timeout_client;
    ASSERT_EQ(timeout_client.init(AF_INET), 0);
    EXPECT_EQ(timeout_client.connect(ipv6_loopback, 1ms), -1);

    qb::io::tcp::socket nonblocking_client;
    ASSERT_EQ(nonblocking_client.init(AF_INET), 0);
    EXPECT_EQ(nonblocking_client.n_connect(ipv6_loopback), -1);
}

#ifndef _WIN32

TEST(TCPSocket, UnixUriBindListenAndConnectVariantsReachLocalSocket) {
    constexpr int expected_connections = 3;
    const auto    path                 = std::string("/tmp/qb-tcp-socket-uri-") + std::to_string(::getpid()) + ".sock";
    const auto    uri                  = qb::io::uri("unix://" + path);

    std::remove(path.c_str());

    qb::io::tcp::socket bound;
    ASSERT_EQ(bound.bind(uri), 0);
    bound.close();
    std::remove(path.c_str());

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen(uri), qb::io::SocketStatus::Done);
    EXPECT_TRUE(listener.is_open());

    std::thread server_thread([&] { accept_tcp_connections(listener, expected_connections); });

    qb::io::tcp::socket uri_client;
    EXPECT_EQ(uri_client.connect(uri), 0);
    uri_client.disconnect();

    qb::io::tcp::socket uri_timeout_client;
    EXPECT_EQ(uri_timeout_client.connect(uri, 1s), qb::io::SocketStatus::Done);
    uri_timeout_client.disconnect();

    qb::io::tcp::socket direct_client;
    EXPECT_EQ(direct_client.connect_un(path), 0);
    direct_client.disconnect();

    server_thread.join();

    qb::io::tcp::socket nonblocking_client;
    const int           ret = nonblocking_client.n_connect(uri);
    const int           err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected unix n_connect result=" << ret << " errno=" << err;
    nonblocking_client.disconnect();

    listener.disconnect();
    std::remove(path.c_str());
}

#endif

TEST(TCPSocket, BaseSocketMoveConstructionAndAssignmentTransferOwnership) {
    qb::io::socket base_constructed(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(base_constructed.is_open());
    const auto constructed_handle = base_constructed.native_handle();

    qb::io::tcp::socket constructed(std::move(base_constructed));
    EXPECT_TRUE(constructed.is_open());
    EXPECT_EQ(constructed.native_handle(), constructed_handle);
    EXPECT_EQ(base_constructed.native_handle(), qb::io::inet::invalid_socket);

    qb::io::socket base_assigned(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(base_assigned.is_open());
    const auto assigned_handle = base_assigned.native_handle();

    qb::io::tcp::socket assigned;
    assigned = std::move(base_assigned);
    EXPECT_TRUE(assigned.is_open());
    EXPECT_EQ(assigned.native_handle(), assigned_handle);
    EXPECT_EQ(base_assigned.native_handle(), qb::io::inet::invalid_socket);
}

TEST(TCPSocket, ReadReportsPeerCloseWithoutStaleErrno) {
    with_tcp_pair([](qb::io::tcp::socket accepted) { accepted.disconnect(); },
                  [](unsigned short port) {
                      qb::io::tcp::socket client;
                      ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
                      qb::io::socket::set_last_errno(EINVAL);

                      char buffer[8] = {};
                      EXPECT_EQ(client.read(buffer, sizeof(buffer)), -1);
                      EXPECT_EQ(qb::io::socket::get_last_errno(), 0);
                      client.close();
                  });
}

TEST(TCPSocket, LowLevelPortableConnectAndTransferHelpers) {
    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] {
        qb::io::socket accepted;
        accepted = listener.accept().release_handle();
        ASSERT_TRUE(accepted.is_open());

        char buffer[32] = {};
        EXPECT_EQ(accepted.recv_n(buffer, static_cast<int>(sizeof("hello")), 1s), static_cast<int>(sizeof("hello")));
        EXPECT_STREQ(buffer, "hello");
        EXPECT_EQ(accepted.send_n("world", static_cast<int>(sizeof("world")), 1s), static_cast<int>(sizeof("world")));
    });

    qb::io::socket client;
    ASSERT_EQ(client.pconnect("127.0.0.1", port), 0);
    EXPECT_TRUE(client.is_open());
    EXPECT_EQ(client.peer_endpoint().port(), port);
    EXPECT_GE(client.tcp_rtt(), 0u);
    EXPECT_EQ(client.send_n("hello", static_cast<int>(sizeof("hello")), 1s), static_cast<int>(sizeof("hello")));

    char buffer[32] = {};
    EXPECT_EQ(client.recv_n(buffer, static_cast<int>(sizeof("world")), 1s), static_cast<int>(sizeof("world")));
    EXPECT_STREQ(buffer, "world");

    server_thread.join();
}

TEST(TCPSocket, LowLevelPortableConnectVariantsReachLoopbackServer) {
    constexpr int expected_connections = 4;

    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_low_level_connections(listener, expected_connections); });

    qb::io::socket xp_client;
    EXPECT_EQ(xp_client.xpconnect("127.0.0.1", port), 0);
    xp_client.close();

    qb::io::socket xp_timeout_client;
    EXPECT_EQ(xp_timeout_client.xpconnect_n("127.0.0.1", port, 1s), 0);
    xp_timeout_client.close();

    qb::io::socket p_timeout_client;
    EXPECT_EQ(p_timeout_client.pconnect_n("127.0.0.1", port, 1s), 0);
    p_timeout_client.close();

    qb::io::socket endpoint_timeout_client;
    EXPECT_EQ(endpoint_timeout_client.pconnect_n(qb::io::endpoint("127.0.0.1", port), 1s), 0);
    endpoint_timeout_client.close();

    qb::io::socket nonblocking_client;
    const int      ret = nonblocking_client.pconnect_n(qb::io::endpoint("127.0.0.1", port));
    const int      err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected pconnect_n result=" << ret << " errno=" << err;
    nonblocking_client.close();

    server_thread.join();
}

TEST(TCPSocket, LowLevelPortableConnectVariantsCanBindExplicitLocalPorts) {
    constexpr int expected_connections = 2;

    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_low_level_connections(listener, expected_connections); });

    qb::io::socket blocking_client;
    const auto     blocking_local_port = reserve_free_tcp_port();
    ASSERT_NE(blocking_local_port, 0);
    EXPECT_EQ(blocking_client.pconnect(qb::io::endpoint("127.0.0.1", port), blocking_local_port), 0);
    EXPECT_EQ(blocking_client.local_endpoint().port(), blocking_local_port);
    blocking_client.close();

    qb::io::socket timeout_client;
    const auto     timeout_local_port = reserve_free_tcp_port();
    ASSERT_NE(timeout_local_port, 0);
    EXPECT_EQ(timeout_client.pconnect_n(qb::io::endpoint("127.0.0.1", port), 1s, timeout_local_port), 0);
    EXPECT_EQ(timeout_client.local_endpoint().port(), timeout_local_port);
    timeout_client.close();

    server_thread.join();

    qb::io::socket nonblocking_client;
    const auto     nonblocking_local_port = reserve_free_tcp_port();
    ASSERT_NE(nonblocking_local_port, 0);
    const int ret = nonblocking_client.pconnect_n(qb::io::endpoint("127.0.0.1", port), nonblocking_local_port);
    const int err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected pconnect_n local-port result=" << ret << " errno=" << err;
    EXPECT_EQ(nonblocking_client.local_endpoint().port(), nonblocking_local_port);
    nonblocking_client.close();
}

TEST(TCPSocket, LowLevelPortableConnectVariantsFailWhenLocalPortIsOccupied) {
    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto remote_port = listener.local_endpoint().port();
    ASSERT_NE(remote_port, 0);

    qb::io::socket occupied;
    ASSERT_EQ(occupied.pserve("127.0.0.1", 0), 0);
    const auto occupied_port = occupied.local_endpoint().port();
    ASSERT_NE(occupied_port, 0);

    qb::io::socket blocking_client;
    EXPECT_EQ(blocking_client.pconnect(qb::io::endpoint("127.0.0.1", remote_port), occupied_port), -1);

    qb::io::socket timeout_client;
    EXPECT_EQ(timeout_client.pconnect_n(qb::io::endpoint("127.0.0.1", remote_port), 1ms, occupied_port), -1);

    qb::io::socket nonblocking_client;
    EXPECT_EQ(nonblocking_client.pconnect_n(qb::io::endpoint("127.0.0.1", remote_port), occupied_port), -1);
}

TEST(TCPSocket, LowLevelResolutionAndInterfaceDiscovery) {
    std::vector<qb::io::endpoint> endpoints;
    EXPECT_EQ(qb::io::socket::resolve(endpoints, "localhost", 4242), 0);
    EXPECT_FALSE(endpoints.empty());

    std::vector<qb::io::endpoint> v4_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_v4(v4_endpoints, "127.0.0.1", 4242), 0);
    ASSERT_FALSE(v4_endpoints.empty());
    EXPECT_TRUE(std::all_of(v4_endpoints.begin(), v4_endpoints.end(), [](const auto &ep) { return ep.af() == AF_INET && ep.port() == 4242; }));

    std::vector<qb::io::endpoint> v6_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_v6(v6_endpoints, "::1", 4242), 0);
    ASSERT_FALSE(v6_endpoints.empty());
    EXPECT_TRUE(std::all_of(v6_endpoints.begin(), v6_endpoints.end(), [](const auto &ep) { return ep.af() == AF_INET6 && ep.port() == 4242; }));

    std::vector<qb::io::endpoint> mapped_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_v4to6(mapped_endpoints, "127.0.0.1", 4242), 0);
    EXPECT_FALSE(mapped_endpoints.empty());

    std::vector<qb::io::endpoint> to_v6_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_tov6(to_v6_endpoints, "localhost", 4242), 0);
    EXPECT_FALSE(to_v6_endpoints.empty());

    int callback_count = 0;
    EXPECT_EQ(qb::io::socket::resolve_i(
                  [&](const qb::io::endpoint &ep) {
                      EXPECT_TRUE(ep.af() == AF_INET || ep.af() == AF_INET6);
                      ++callback_count;
                      return true;
                  },
                  "localhost", 4242, AF_UNSPEC, AI_ALL),
              0);
    EXPECT_EQ(callback_count, 1);

    std::vector<qb::io::endpoint> missing;
    EXPECT_NE(qb::io::socket::resolve(missing, "invalid.invalid.invalid", 4242), 0);
    EXPECT_TRUE(missing.empty());

    const int ip_flags = qb::io::socket::getipsv();
    EXPECT_GE(ip_flags, qb::io::inet::ip::ipsv_unavailable);

    int local_count = 0;
    qb::io::socket::traverse_local_address([&](const qb::io::endpoint &ep) {
        EXPECT_TRUE(ep.af() == AF_INET || ep.af() == AF_INET6);
        ++local_count;
        return true;
    });
    EXPECT_GE(local_count, 0);
}

TEST(TCPSocket, LowLevelSocketOptionsReadinessAndBindingHelpers) {
    qb::io::socket listener;
    ASSERT_TRUE(listener.open(AF_INET, SOCK_STREAM, 0));
    listener.reuse_address(true);
    listener.exclusive_address(false);
    EXPECT_EQ(listener.bind_any(false), 0);
    EXPECT_TRUE(listener.local_endpoint());
    EXPECT_EQ(listener.listen(), 0);

    EXPECT_EQ(listener.handle_read_ready(1ms), 0);
    EXPECT_EQ(qb::io::socket::get_last_errno(), ETIMEDOUT);
    EXPECT_EQ(listener.handle_write_ready(1ms), 0);

    int accept_conn = 0;
    if (listener.get_optval(SOL_SOCKET, SO_ACCEPTCONN, accept_conn) == 0) {
        EXPECT_NE(accept_conn, 0);
    }

    qb::io::socket raw_socket(static_cast<::socket_type>(listener));
    EXPECT_TRUE(raw_socket.is_open());
    EXPECT_EQ(raw_socket.native_handle(), listener.native_handle());
    EXPECT_EQ(listener.release_handle(), raw_socket.native_handle());
    raw_socket.close();

    qb::io::socket keepalive_socket;
    ASSERT_TRUE(keepalive_socket.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_LE(keepalive_socket.set_keepalive(1, 1, 1, 1), 0);
    EXPECT_GE(qb::io::socket::tcp_rtt(keepalive_socket.native_handle()), 0u);
    qb::io::socket::init_ws32_lib();
}

TEST(TCPSocket, LowLevelNonBlockingAndTimeoutFailuresAreRestored) {
    qb::io::socket closed;
    EXPECT_EQ(qb::io::socket::set_nonblocking(closed.native_handle(), true), -1);
    EXPECT_EQ(closed.shutdown(SHUT_RDWR), -1);

    qb::io::socket client;
    ASSERT_TRUE(client.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_EQ(client.connect_n(qb::io::endpoint().as_in("127.0.0.1", 9), 1ms), -1);
    EXPECT_EQ(client.test_nonblocking(), 0);

    qb::io::socket udp(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(udp.is_open());
    ASSERT_EQ(udp.bind("127.0.0.1", 0), 0);
    const int disconnect_result = udp.disconnect();
    EXPECT_TRUE(disconnect_result == 0 || disconnect_result == -1)
        << "unexpected UDP disconnect result=" << disconnect_result << " errno=" << qb::io::socket::get_last_errno();
}

TEST(TCPSocket, StaticHelpersAndErrorClassifiers) {
    EXPECT_TRUE(qb::io::socket::not_send_error(EWOULDBLOCK));
    EXPECT_TRUE(qb::io::socket::not_send_error(EAGAIN));
    EXPECT_TRUE(qb::io::socket::not_send_error(EINTR));
    EXPECT_TRUE(qb::io::socket::not_send_error(ENOBUFS));
    EXPECT_FALSE(qb::io::socket::not_send_error(ECONNRESET));

    EXPECT_TRUE(qb::io::socket::not_recv_error(EWOULDBLOCK));
    EXPECT_TRUE(qb::io::socket::not_recv_error(EAGAIN));
    EXPECT_TRUE(qb::io::socket::not_recv_error(EINTR));
    EXPECT_FALSE(qb::io::socket::not_recv_error(ECONNRESET));

    EXPECT_NE(qb::io::socket::strerror(EINVAL), nullptr);
    EXPECT_NE(qb::io::socket::gai_strerror(EAI_NONAME), nullptr);

    qb::io::socket::set_last_errno(0);
    EXPECT_EQ(qb::io::socket::get_last_errno(), 0);
}
