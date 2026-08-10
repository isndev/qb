/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the specific terms.
 */

/**
 * @file system/tcp/socket-error-paths.cpp
 * @brief `qb::io::socket` (system/sys__socket.cpp) — the failure branches the loopback suite never hits.
 *
 * tcp-socket-loopback.cpp drives the *success* surface of the portable `qb::io::socket` wrapper over
 * a real socket pair. This test complements it with the negative/error branches that drive the weak
 * BRANCH coverage of sys__socket.cpp without needing a peer or an event loop — every operation here
 * is a single, deterministic syscall on a self-contained descriptor (a few cases bind an ephemeral
 * loopback port to *learn* a refused/occupied port, which is why it sits in the `system` tier):
 *
 *   - `open()` of a bogus address family fails; a double `open()` on an already-open socket is a no-op
 *     that keeps the original descriptor (the `invalid_socket == fd` guard).
 *   - `bind()` to an already-in-use loopback port fails (`EADDRINUSE`-class non-zero).
 *   - `connect()` to a closed loopback port is refused; `connect_n(timeout)` to a black-holed address
 *     times out — both surface as a non-zero/-1 return.
 *   - `getsockopt`/`setsockopt` on a closed descriptor fail; the option accessors round-trip on an open one.
 *   - `resolve*()` of a bogus host returns a non-zero EAI error and an untouched output vector; the
 *     family-specific resolvers reject the wrong literal family.
 *   - `local_endpoint()` / `peer_endpoint()` on an unbound / unconnected socket return an empty
 *     endpoint (the `getsockname`/`getpeername` != 0 branch).
 *   - `accept()`/`accept_n()` on a non-listening or closed socket fail.
 *   - `shutdown()`/`disconnect()`/`set_nonblocking()`/`test_nonblocking()` on a closed fd fail.
 *   - the AF_UNSPEC `disconnect()` path on a fresh (never-connected) datagram socket.
 *
 * Daemon-free `system`: no `qb::Main`, no event loop, no loopback round-trip. A handful of cases bind
 * an ephemeral `:0` loopback port to *learn* a refused/occupied port — a single local bind, not a
 * client/server exchange, so it stays collision-free under `ctest -j`.
 *
 * Signatures exercised (qb/io/system/sys__socket.h):
 *   bool open(int af, int type, int protocol);
 *   int  bind(const endpoint&) const;  int bind(const char*, unsigned short) const;
 *   int  connect(const endpoint&);     static int connect_n(socket_type, const endpoint&, const qb::duration&);
 *   int  listen(int backlog) const;    int accept_n(socket_type&) const;  socket accept() const;
 *   endpoint local_endpoint() const;   endpoint peer_endpoint() const;
 *   static int resolve(std::vector<endpoint>&, const char*, unsigned short, int socktype);
 *   template<_Ty> int get_optval/set_optval(int level, int optname, ...) const;
 *   int shutdown(int how) const;       int disconnect() const;
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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/system/sys__socket.h>

#include "../../shared/loopback_fixture.h"

#if !defined(_WIN32)
// For the SIGPIPE-containment cases at the bottom of this file: they fork a child,
// reset the signal disposition and observe the wait status.
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

// Bind an ephemeral v4 loopback TCP port and return it WITHOUT keeping the socket
// open — used to learn a port that is (briefly) not listening, so a connect to it
// is refused. Pure local bind, no peer.
unsigned short
reserve_then_release_port() {
    qb::io::socket probe;
    EXPECT_EQ(probe.pserve("127.0.0.1", 0), 0);
    const auto port = probe.local_endpoint().port();
    probe.close();
    return port;
}

} // namespace

// ===========================================================================
// open / reopen guards
// ===========================================================================

TEST(SocketErrorPaths, OpenWithBogusAddressFamilyFails) {
    qb::io::socket bad;
    // A nonsense address family must not yield an open descriptor.
    EXPECT_FALSE(bad.open(-1, SOCK_STREAM, 0));
    EXPECT_FALSE(bad.is_open());
    EXPECT_EQ(bad.native_handle(), qb::io::inet::invalid_socket);
}

TEST(SocketErrorPaths, SecondOpenOnAnOpenSocketIsANoOpKeepingTheDescriptor) {
    qb::io::socket sock;
    ASSERT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));
    const auto first_handle = sock.native_handle();

    // open() short-circuits on `invalid_socket == fd`; a second call must not
    // replace (or leak) the live descriptor.
    EXPECT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_EQ(sock.native_handle(), first_handle);
    sock.close();
}

TEST(SocketErrorPaths, ReopenReplacesDescriptorWithAFreshOne) {
    qb::io::socket sock;
    ASSERT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));
    const auto first_handle = sock.native_handle();

    // reopen() closes first, so it always produces a (possibly different) live fd.
    ASSERT_TRUE(sock.reopen(AF_INET, SOCK_DGRAM, 0));
    EXPECT_TRUE(sock.is_open());
    // The previous fd was closed; the new one is independently valid.
    EXPECT_NE(sock.native_handle(), qb::io::inet::invalid_socket);
    (void) first_handle;
    sock.close();
}

// ===========================================================================
// bind failures
// ===========================================================================

TEST(SocketErrorPaths, BindToAnAlreadyOccupiedPortFails) {
    qb::io::socket holder;
    ASSERT_EQ(holder.pserve("127.0.0.1", 0), 0);
    const auto port = holder.local_endpoint().port();
    ASSERT_NE(port, 0);

    // A second socket binding the same loopback address:port must be refused.
    // (holder did NOT set SO_REUSEPORT; pserve sets SO_REUSEADDR which permits
    // rebinding TIME_WAIT ports but not an actively-bound listening port.)
    qb::io::socket second;
    ASSERT_TRUE(second.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_NE(second.bind(qb::io::endpoint("127.0.0.1", port)), 0) << "binding an actively-occupied loopback port must fail";
    second.close();
    holder.close();
}

TEST(SocketErrorPaths, BindOnAClosedSocketFails) {
    qb::io::socket closed;
    EXPECT_NE(closed.bind("127.0.0.1", 0), 0) << "bind on an unopened descriptor must fail";
}

// ===========================================================================
// connect failures
// ===========================================================================

TEST(SocketErrorPaths, ConnectToAClosedPortIsRefused) {
    const auto dead_port = reserve_then_release_port();
    ASSERT_NE(dead_port, 0);

    qb::io::socket client;
    ASSERT_TRUE(client.open(AF_INET, SOCK_STREAM, 0));
    // The kernel has no listener on `dead_port`; the connect is refused.
    EXPECT_NE(client.connect(qb::io::endpoint("127.0.0.1", dead_port)), 0)
        << "blocking connect to a closed loopback port must fail (connection refused)";
    client.close();
}

TEST(SocketErrorPaths, TimedConnectToBlackHoleAddressTimesOut) {
    qb::io::socket client;
    ASSERT_TRUE(client.open(AF_INET, SOCK_STREAM, 0));

    // 192.0.2.0/24 (TEST-NET-1, RFC 5737) is guaranteed non-routable: the SYN is
    // black-holed, so a 1ns budget forces the select()<=0 timeout branch of connect_n.
    const int ret = qb::io::socket::connect_n(client.native_handle(), qb::io::endpoint("192.0.2.1", 9), qb::duration(1));
    EXPECT_EQ(ret, -1) << "timed connect_n to a black-holed address with a 1ns budget must fail";
    client.close();
}

// ===========================================================================
// getsockopt / setsockopt error branches
// ===========================================================================

TEST(SocketErrorPaths, OptionAccessorsFailOnClosedDescriptor) {
    qb::io::socket closed;

    int value = 0;
    EXPECT_NE(closed.get_optval(SOL_SOCKET, SO_REUSEADDR, value), 0) << "get_optval on a closed fd must fail";
    EXPECT_NE(closed.set_optval(SOL_SOCKET, SO_REUSEADDR, 1), 0) << "set_optval on a closed fd must fail";
}

TEST(SocketErrorPaths, GetOptvalReadsBackAKnownOptionOnAnOpenSocket) {
    qb::io::socket sock;
    ASSERT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));

    // The template get_optval succeeds on an open socket and reports the socket's
    // type (SOCK_STREAM) — the success branch the closed-fd case above cannot reach.
    int sock_type = 0;
    ASSERT_EQ(sock.get_optval(SOL_SOCKET, SO_TYPE, sock_type), 0);
    EXPECT_EQ(sock_type, SOCK_STREAM);

    // And a round-trip set_optval/get_optval on an open socket takes effect.
    ASSERT_EQ(sock.set_optval(SOL_SOCKET, SO_REUSEADDR, 1), 0);
    int reuse = 0;
    ASSERT_EQ(sock.get_optval(SOL_SOCKET, SO_REUSEADDR, reuse), 0);
    EXPECT_NE(reuse, 0);
    sock.close();
}

// ===========================================================================
// resolve failures
// ===========================================================================

TEST(SocketErrorPaths, ResolveOfABogusHostFailsAndLeavesOutputUntouched) {
    std::vector<qb::io::endpoint> endpoints;
    // A syntactically-host-shaped but unresolvable name returns a non-zero EAI code.
    EXPECT_NE(qb::io::socket::resolve(endpoints, "no.such.host.invalid.qb", 4242), 0);
    EXPECT_TRUE(endpoints.empty()) << "a failed resolve must not push any endpoint";

    std::vector<qb::io::endpoint> v4;
    EXPECT_NE(qb::io::socket::resolve_v4(v4, "no.such.host.invalid.qb", 4242), 0);
    EXPECT_TRUE(v4.empty());

    std::vector<qb::io::endpoint> v6;
    EXPECT_NE(qb::io::socket::resolve_v6(v6, "no.such.host.invalid.qb", 4242), 0);
    EXPECT_TRUE(v6.empty());
}

TEST(SocketErrorPaths, FamilySpecificResolversRejectTheWrongLiteralFamily) {
    // Asking the IPv6-only resolver to resolve an IPv4 literal yields no AF_INET6
    // endpoint (resolve_v6 passes AF_INET6 with no V4MAPPED flag).
    std::vector<qb::io::endpoint> v6;
    qb::io::socket::resolve_v6(v6, "127.0.0.1", 4242);
    EXPECT_TRUE(std::none_of(v6.begin(), v6.end(), [](const auto &ep) { return ep.af() == AF_INET; }))
        << "resolve_v6 must never yield a raw AF_INET endpoint";
}

// ===========================================================================
// endpoint accessors on un-associated sockets
// ===========================================================================

TEST(SocketErrorPaths, PeerEndpointOnUnconnectedSocketIsEmpty) {
    qb::io::socket sock;
    ASSERT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));

    // getpeername on a never-connected socket fails -> empty endpoint.
    const auto peer = sock.peer_endpoint();
    EXPECT_FALSE(peer) << "peer_endpoint of an unconnected socket must be empty";
    EXPECT_EQ(peer.port(), 0);
    sock.close();
}

TEST(SocketErrorPaths, EndpointAccessorsOnClosedSocketAreEmpty) {
    qb::io::socket closed;
    EXPECT_FALSE(closed.local_endpoint());
    EXPECT_FALSE(closed.peer_endpoint());
}

// ===========================================================================
// accept failures
// ===========================================================================

TEST(SocketErrorPaths, AcceptOnANonListeningSocketFails) {
    qb::io::socket sock;
    ASSERT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));
    ASSERT_EQ(sock.bind("127.0.0.1", 0), 0);
    // No listen() call: the bound-but-not-listening socket cannot accept.
    sock.set_nonblocking(true);

    ::socket_type accepted = qb::io::inet::invalid_socket;
    const int     ret      = sock.accept_n(accepted);
    EXPECT_NE(ret, 0) << "accept_n on a non-listening socket must report an error";
    EXPECT_EQ(accepted, qb::io::inet::invalid_socket);
    sock.close();
}

TEST(SocketErrorPaths, AcceptObjectFormOnClosedSocketYieldsClosedResult) {
    qb::io::socket closed;
    // The object-returning accept() of a closed listener produces a closed socket.
    qb::io::socket result = closed.accept();
    EXPECT_FALSE(result.is_open());
}

// ===========================================================================
// shutdown / disconnect / nonblocking on closed or fresh descriptors
// ===========================================================================

TEST(SocketErrorPaths, ShutdownAndNonblockingFailOnClosedDescriptor) {
    qb::io::socket closed;
    EXPECT_EQ(closed.shutdown(QB_SD_BOTH), -1) << "shutdown of a closed socket must fail";
    EXPECT_EQ(qb::io::socket::set_nonblocking(closed.native_handle(), true), -1) << "set_nonblocking on an invalid fd must fail";
}

TEST(SocketErrorPaths, DisconnectOnAFreshDatagramSocketIsWellDefined) {
    // disconnect() issues connect(AF_UNSPEC); on a never-connected datagram socket
    // it is a well-defined dissolve that the platform may accept (0) or reject (-1),
    // but must not crash or hang.
    qb::io::socket udp(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(udp.is_open());
    const int ret = udp.disconnect();
    EXPECT_TRUE(ret == 0 || ret == -1) << "fresh-socket disconnect returned unexpected " << ret;
    udp.close();
}

// ===========================================================================
// errno classifiers / strerror on the error spectrum
// ===========================================================================

TEST(SocketErrorPaths, ErrorClassifiersDistinguishFatalFromTransient) {
    // Transient (retryable) — these must NOT be treated as a hard failure.
    EXPECT_TRUE(qb::io::socket::not_recv_error(EWOULDBLOCK));
    EXPECT_TRUE(qb::io::socket::not_send_error(EWOULDBLOCK));

    // Fatal connection errors — these MUST be reported as real failures.
    EXPECT_FALSE(qb::io::socket::not_recv_error(ECONNRESET));
    EXPECT_FALSE(qb::io::socket::not_send_error(ECONNREFUSED));
    EXPECT_FALSE(qb::io::socket::not_recv_error(EPIPE));

    // strerror/gai_strerror never return nullptr, even for an out-of-range code.
    EXPECT_NE(qb::io::socket::strerror(ECONNREFUSED), nullptr);
    EXPECT_NE(qb::io::socket::strerror(0), nullptr);
}

// ===========================================================================
// Positive success branches of sys__socket.cpp the loopback suite never drives:
// the string-form connect overloads, the pconnect_n resolve+connect path, the
// pserve bind-failure return, the send_n break, tcp_rtt on an idle socket, and
// the select negative-timeout clamp. Each runs against a bounded local accept
// loop that self-exits after exactly the number of successful client connects
// (no while(!stop) accept that would wedge on macOS).
// ===========================================================================

namespace {

// Accept exactly `count` connections on a listening qb::io::socket, then return.
//
// Bounded, and it has to be. The clients below all connect with NON-FATAL EXPECT_,
// so one that regresses leaves the count short -- and a blocking accept() then parks
// this thread forever while the caller's `server_thread.join()` waits on it, which
// replaces the EXPECT's message with a ctest timeout. The old `break` on a closed
// socket did not cover it either: it needed the listener torn down to wake the
// accept, and on macOS a cross-thread shutdown() does NOT wake a blocked accept().
// A non-blocking listener plus a real deadline is the only form that self-terminates.
void
accept_exactly(qb::io::socket &listener, int count, std::chrono::milliseconds budget = 5s) {
    listener.set_nonblocking(true);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (int i = 0; i < count; ++i) {
        ::socket_type handle = qb::io::inet::invalid_socket;
        while (listener.accept_n(handle) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ADD_FAILURE() << "only " << i << " of " << count << " clients connected within the accept budget";
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        qb::io::socket accepted(handle); // closes on scope exit
    }
}

} // namespace

TEST(SocketErrorPaths, StringFormConnectVariantsReachLoopbackServer) {
    constexpr int expected_connections = 4;

    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_exactly(listener, expected_connections); });

    // Keep every client socket OPEN until the server has accepted all four, so the
    // non-blocking pconnect_n leg is never RST'd before its loopback handshake settles.
    qb::io::socket c1, c2, c3, c4;

    // 1) member connect(addr, port): parses the literal, ::connect on the open fd.
    ASSERT_TRUE(c1.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_EQ(c1.connect("127.0.0.1", port), 0);

    // 2) static connect(s, addr, port): same, driven through the socket_type overload.
    ASSERT_TRUE(c2.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_EQ(qb::io::socket::connect(c2.native_handle(), "127.0.0.1", port), 0);

    // 3) member connect_n(addr, port, timeout): the timed non-blocking connect on a
    //    literal host, which completes against the listener within the budget.
    ASSERT_TRUE(c3.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_EQ(c3.connect_n("127.0.0.1", port, 1s), 0);

    // 4) member pconnect_n(host, port): resolve + reopen + non-blocking connect. On
    //    loopback this either completes (0) or reports in-progress; either way the
    //    kernel finishes the handshake so the server accepts it.
    const int ret4 = c4.pconnect_n("127.0.0.1", port);
    const int err4 = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret4 == 0 || err4 == EINPROGRESS || qb::io::socket::not_send_error(err4)) << "pconnect_n result=" << ret4 << " errno=" << err4;

    server_thread.join();

    c1.close();
    c2.close();
    c3.close();
    c4.close();
}

TEST(SocketErrorPaths, PserveOnAnAlreadyServedPortFails) {
    qb::io::socket first;
    ASSERT_EQ(first.pserve("127.0.0.1", 0), 0);
    const auto port = first.local_endpoint().port();
    ASSERT_NE(port, 0);

    // pserve sets SO_REUSEADDR but NOT SO_REUSEPORT, so a second pserve on the same
    // actively-listening loopback address:port fails at its internal bind() and returns
    // that non-zero code (the `n != 0` early return of pserve).
    qb::io::socket second;
    EXPECT_NE(second.pserve("127.0.0.1", port), 0) << "a second pserve on an actively-served port must fail";
    second.close();
    first.close();
}

TEST(SocketErrorPaths, SendNBreaksInsteadOfBlockingOnBackpressuredClosedPeer) {
    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> peer_closed{false};
    std::thread       server_thread([&] {
        // Bounded accept, and a `peer_closed` that is ALWAYS published: the main thread
        // spins on that flag with no deadline of its own, so a thread that parked in
        // accept() or returned early would wedge it forever.
        if (qb::io::test::wait_acceptable_within(listener.native_handle())) {
            qb::io::socket accepted = listener.accept();
            if (accepted.is_open()) {
                accepted.close(); // peer goes away
            }
        } else {
            ADD_FAILURE() << "no client connected within the accept budget";
        }
        peer_closed = true;
    });

    // Joined on every exit path: several fatal ASSERTs follow, and each would otherwise
    // return from the body leaving this thread joinable — std::terminate, which drops the
    // message it just printed. Safe: the thread's accept is bounded above.
    const qb::io::test::thread_joiner server_joiner{server_thread};

    qb::io::socket client;
    ASSERT_TRUE(client.open(AF_INET, SOCK_STREAM, 0));
    ASSERT_EQ(client.connect(qb::io::endpoint("127.0.0.1", port)), 0);

    // Tiny send buffer; wait until the peer has closed. Bounded: the flag is set on
    // every path out of the server thread above, so a timeout here means that thread
    // never ran at all -- which is a finding, not a reason to spin forever.
    ASSERT_EQ(client.set_optval(SOL_SOCKET, SO_SNDBUF, 4096), 0);
    {
        const auto peer_deadline = std::chrono::steady_clock::now() + 5s;
        while (!peer_closed.load() && std::chrono::steady_clock::now() < peer_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(peer_closed.load()) << "the server thread never reported the peer close";
    }

    // send_n's contract is that it BREAKS OUT within its time budget instead of blocking on a
    // peer that cannot take the data. It is NOT a promise about how many bytes the kernel
    // accepts — that is platform behaviour, and the two platforms disagree:
    //
    //   * POSIX honours SO_SNDBUF as a real cap, so the socket fills after a few KiB, send()
    //     returns EWOULDBLOCK, the 1ns budget is already spent, and send_n returns a SHORT count.
    //   * Winsock does NOT treat SO_SNDBUF as a cap on an individual send: it buffers the
    //     caller's data regardless and surfaces the dead peer on a LATER operation. The whole
    //     256 KiB is accepted by one send() and send_n returns len having never waited at all.
    //
    // Both satisfy the contract. Asserting a short count therefore tested the operating system
    // rather than qb, and failed on Windows for a reason nothing in send_n could cause (measured:
    // the call returns in ~2 ms with sent == len). So the count/promptness half is asserted
    // unconditionally, and the short-count half only where it is a real property.
    //
    // It is genuinely NOT testable on Windows, and not for want of a better setup: a variant with
    // a peer that ACCEPTS and then never reads a byte — the textbook way to close the receive
    // window — was tried, and Winsock still took a whole 8 MiB payload in 6 ms with SO_SNDBUF at
    // 4096. Windows simply pends the caller's buffer rather than refusing it. This case is
    // therefore POSIX-only coverage, and is marked as such instead of being weakened for
    // everyone.
    std::vector<char> payload(256 * 1024, 'x');
    const auto        t0      = std::chrono::steady_clock::now();
    const int         sent    = client.send_n(payload.data(), static_cast<int>(payload.size()), qb::duration(1));
    const auto        elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_GE(sent, 0);
    EXPECT_LE(sent, static_cast<int>(payload.size())) << "send_n must never claim more than it was given";
    EXPECT_LT(elapsed, std::chrono::seconds(5)) << "send_n must honour its time budget, not block on a dead peer";
#if !defined(_WIN32)
    EXPECT_LT(sent, static_cast<int>(payload.size())) << "send_n must break rather than transfer the whole payload to a stalled peer";
#endif

    client.close();
    server_thread.join();
}

TEST(SocketErrorPaths, TcpRttOnAnUnconnectedSocketIsZero) {
    qb::io::socket sock;
    ASSERT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));
    // No connection: the TCP_INFO / TCP_CONNECTION_INFO query yields no RTT, so the
    // accessor returns its 0 fallback rather than a stale value.
    EXPECT_EQ(sock.tcp_rtt(), 0u);
    sock.close();
}

TEST(SocketErrorPaths, HandleReadReadyClampsANegativeTimeout) {
    qb::io::socket sock;
    ASSERT_TRUE(sock.open(AF_INET, SOCK_STREAM, 0));

    // A negative duration would make ::select fail with EINVAL; socket::select clamps it to zero
    // (poll once). What is under test is the CLAMP, and its two observable consequences are that
    // the call does not fail (-1/EINVAL) and that it returns immediately instead of blocking.
    //
    // Deliberately NOT asserted: whether the socket comes back readable. That is platform
    // behaviour, not ours — Linux reports an unconnected TCP socket as select-readable (a read
    // would return an error, which counts as readable) and returns 1, while macOS reports it as
    // not readable and returns 0. The original `EXPECT_LE(ret, 0)` encoded the macOS answer and
    // failed on Linux; it was conflating "the timeout was clamped" with "nothing is readable".
    const auto start   = std::chrono::steady_clock::now();
    const int  ret     = qb::io::socket::handle_read_ready(sock.native_handle(), -1ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(ret, 0) << "a negative timeout must be clamped, not passed through to ::select as EINVAL (-1)";
    EXPECT_LT(elapsed, 1s) << "a clamped (zero) timeout must poll once and return, never block";
    sock.close();
}

// ===========================================================================
// SIGPIPE containment.
//
// Writing to a socket whose peer has closed raises SIGPIPE, whose DEFAULT
// disposition terminates the process. A library must not let that happen: the
// write has to come back as an error the caller can handle.
//
// qb guards it twice, and BOTH guards had a hole:
//
//   1. Per call -- MSG_NOSIGNAL in `flags`. Every send/recv wrapper in
//      sys__socket.h defaults `flags` to MSG_NOSIGNAL; send_n()/recv_n() defaulted
//      theirs to 0 and so OVERRODE send()'s own default on the way through. One
//      write to a closed peer then killed the process. Measured on Linux AND on
//      macOS: contrary to folklore, current Darwin DOES define and honour
//      MSG_NOSIGNAL (0x80000, "do not generate SIGPIPE on EOF"), so the hole is
//      not Linux-only and sys__socket.h's `#define MSG_NOSIGNAL 0` fallback only
//      ever fires on an SDK that lacks it (Windows, ancient BSDs).
//   2. Per descriptor -- SO_NOSIGPIPE, which BSD/macOS offer and Linux does not.
//      open() and accept_n() set it; the descriptor ADOPTION paths --
//      socket(socket_type) and operator=(socket_type), which is how
//      socket::accept() and tcp::listener::accept() produce their socket -- did
//      not, so the backstop was absent exactly where guard 1 had its hole.
//
// Windows has no SIGPIPE; nothing to test there.
//
// The two guards are NOT interchangeable and the cases below prove it, measured on
// macOS 26 (each row is a real build of the four combinations):
//
//   fix applied              SendN case   send case   guard case
//   ----------------------   ----------   ---------   ----------
//   neither                  SIGPIPE      pass        FAIL
//   SO_NOSIGPIPE only        SIGPIPE      pass        pass
//   flags default only       pass         pass        FAIL
//   both                     pass         pass        pass
//
// SO_NOSIGPIPE alone does NOT rescue send_n, because setsockopt(SO_NOSIGPIPE) on a
// socket whose peer already closed fails with EINVAL -- the backstop can only be
// armed while the connection is live. So each half of the fix is independently
// necessary, and each is pinned by a different case here. The `send` case passes in
// all four rows on purpose: it is the positive control.
//
// It has to run in a CHILD process for two reasons: the assertion is "was not
// killed by a signal", which cannot be observed from inside the victim; and the
// child must first restore SIGPIPE to SIG_DFL, because when qb is built WITH SSL
// the OpenSSL initialiser (io/tcp/ssl/init.cpp) installs a process-wide
// signal(SIGPIPE, SIG_IGN) that masks the whole class. That mask is why this first
// surfaced under the `feature-gates` preset (QB_WITH_SSL=OFF) -- the test removes
// it so the coverage does not depend on a build option.
// ===========================================================================

#if !defined(_WIN32)

namespace {

// Exit codes the child uses to report what happened, all distinct from a signal death.
enum : int {
    kChildSurvivedWithError = 0, // the write returned an error: correct
    kChildSetupFailed       = 3, // could not build the fixture; inconclusive
    kChildWriteClaimedOk    = 4  // the write to a dead peer reported full success
};

// Run `body` in a forked child and return its wait(2) status.
//
// Every failure of the harness ITSELF must land on kChildSetupFailed rather than on
// the all-zero status, because a zero status decodes as "exited 0" == the success
// code. A failed fork() that fell through to waitpid(-1) would otherwise leave
// status untouched at 0 and the case would pass without ever running.
template <typename F>
int
run_in_child(F &&body) {
    // WEXITSTATUS is the second byte of the status word on every POSIX platform, so
    // this is the wait-status encoding of "the child exited with kChildSetupFailed".
    const int setup_failed_status = kChildSetupFailed << 8;

    const pid_t pid = ::fork();
    if (pid == 0) {
        // Undo any inherited SIGPIPE masking (OpenSSL's SIG_IGN, a test harness, a
        // shell) so the child faces the DEFAULT disposition and the guard under test
        // is the only thing standing between it and death.
        ::signal(SIGPIPE, SIG_DFL);
        ::_exit(body());
    }
    if (pid < 0) {
        ADD_FAILURE() << "fork() failed: " << std::strerror(errno);
        return setup_failed_status;
    }

    int   status = 0;
    pid_t reaped = -1;
    while ((reaped = ::waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
    }
    if (reaped != pid) {
        ADD_FAILURE() << "waitpid() failed: " << std::strerror(errno);
        return setup_failed_status;
    }
    return status;
}

// Describe a wait status for the failure message.
std::string
describe_status(int status) {
    if (WIFSIGNALED(status)) {
        return "killed by signal " + std::to_string(WTERMSIG(status)) + (WTERMSIG(status) == SIGPIPE ? " (SIGPIPE)" : "");
    }
    if (WIFEXITED(status)) {
        return "exited " + std::to_string(WEXITSTATUS(status));
    }
    return "did not terminate normally";
}

// A connected AF_UNIX stream pair, one end closed: the shortest deterministic way to
// get a "peer has gone" socket with no port, no listener and no timing window. The
// pair is created by ::socketpair, so the surviving end reaches qb through the
// descriptor-ADOPTION path -- exactly the path that lacked the guard.
constexpr int kPeerClosedPayload = 4096;

} // namespace

TEST(SocketErrorPaths, SendNToAClosedPeerReportsAnErrorInsteadOfKillingTheProcess) {
    const int status = run_in_child([] {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            return kChildSetupFailed;
        }
        ::close(sv[1]); // the peer is gone

        qb::io::socket sock;
        sock = sv[0]; // qb takes ownership -> qb owns the guard

        std::vector<char> payload(kPeerClosedPayload, 'x');
        // Two writes: the first may be absorbed by the send buffer before the FIN is
        // processed, the second cannot be. Either raises SIGPIPE if unguarded.
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int sent = sock.send_n(payload.data(), static_cast<int>(payload.size()), 200ms);
            if (sent < static_cast<int>(payload.size())) {
                return kChildSurvivedWithError;
            }
        }
        return kChildWriteClaimedOk;
    });

    ASSERT_FALSE(WIFSIGNALED(status)) << "send_n() to a peer that closed must return an error, not kill the process "
                                         "(child "
                                      << describe_status(status) << ")";
    EXPECT_NE(WEXITSTATUS(status), kChildSetupFailed) << "socketpair fixture could not be built";
    EXPECT_EQ(WEXITSTATUS(status), kChildSurvivedWithError)
        << "send_n() reported full success writing to a closed peer (child " << describe_status(status) << ")";
}

TEST(SocketErrorPaths, SendToAClosedPeerReportsAnErrorInsteadOfKillingTheProcess) {
    // The same contract for the single-shot send(), which already defaulted its flags
    // to MSG_NOSIGNAL. This one passed before the fix as well as after -- it is the
    // POSITIVE CONTROL that proves the fixture really does produce a closed peer and
    // that the harness can tell "survived with an error" from "killed": if send() and
    // send_n() had both died, the fixture would be indicting itself rather than the
    // default. Keep it: it is what makes the send_n failure attributable.
    const int status = run_in_child([] {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            return kChildSetupFailed;
        }
        ::close(sv[1]);

        qb::io::socket sock;
        sock = sv[0];

        std::vector<char> payload(kPeerClosedPayload, 'x');
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (sock.send(payload.data(), static_cast<int>(payload.size())) < 0) {
                return kChildSurvivedWithError;
            }
        }
        return kChildWriteClaimedOk;
    });

    ASSERT_FALSE(WIFSIGNALED(status)) << "send() to a peer that closed must return an error, not kill the process "
                                         "(child "
                                      << describe_status(status) << ")";
    EXPECT_NE(WEXITSTATUS(status), kChildSetupFailed) << "socketpair fixture could not be built";
    EXPECT_EQ(WEXITSTATUS(status), kChildSurvivedWithError)
        << "send() reported full success writing to a closed peer (child " << describe_status(status) << ")";
}

#endif // !_WIN32

// Every descriptor-producing path of qb::io::socket must hand back a descriptor that
// carries the platform guard, not just the ones ::socket() created. On BSD/macOS that
// guard is observable directly, so assert it on each path rather than only through the
// behavioural test above -- a missing one is then named, not merely fatal.
#if defined(SO_NOSIGPIPE) && !defined(__linux__) && !defined(_WIN32)
TEST(SocketErrorPaths, EveryDescriptorAcquisitionPathCarriesTheSigpipeGuard) {
    const auto is_guarded = [](const qb::io::socket &s) {
        int on = 0;
        return s.get_optval(SOL_SOCKET, SO_NOSIGPIPE, on) == 0 && on != 0;
    };

    // 1. open() -- always had it.
    qb::io::socket opened;
    ASSERT_TRUE(opened.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_TRUE(is_guarded(opened)) << "open() must set SO_NOSIGPIPE";
    opened.close();

    // 2. socket(socket_type) -- adoption by construction, e.g. socket::accept().
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    {
        qb::io::socket adopted_by_ctor(sv[0]);
        EXPECT_TRUE(is_guarded(adopted_by_ctor)) << "socket(socket_type) must set SO_NOSIGPIPE on the adopted fd";

        // 3. operator=(socket_type) -- adoption by assignment, e.g. release_handle() transfer.
        qb::io::socket adopted_by_assign;
        adopted_by_assign = sv[1];
        EXPECT_TRUE(is_guarded(adopted_by_assign)) << "operator=(socket_type) must set SO_NOSIGPIPE on the adopted fd";
    }
}
#endif
