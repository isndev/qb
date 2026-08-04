/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/tcp-connector-state-machine.cpp
 * @brief The async TCP connector's internal completion state machine — mock-driven, deterministic.
 *
 * The real loopback contract (co_await connect resumes with an open/empty socket) is proven
 * end-to-end in system/coroutine/tcp-connector-loopback.cpp. This file proves the *authoritative
 * connector state machine* — `qb::io::async::tcp::connector<Socket_>` (qb/io/async/tcp/connector.h)
 * — by substituting a fake socket so each branch is reached on demand, with zero kernel timing
 * noise. Where the loopback file cannot deterministically force "n_connect returns EINPROGRESS,
 * then SO_ERROR clears, then handshake_status flips 0→1 across two EV_WRITE turns", this file dials
 * exactly that with a scripted `FakeConnectorSocket`.
 *
 * The fake models every surface the connector's `if constexpr` probes drive:
 *   - n_connect(uri)               : returns 0 (direct), or -1 with EINPROGRESS (pending) / ECONNREFUSED.
 *   - handshake_status()           : a *scripted* sequence (e.g. {0,1} = pending then done, {-1} = fail)
 *                                    that finalize_transport_connect() consumes one step per call.
 *   - get_optval(SO_ERROR)         : feeds the connector's post-EV_WRITE error check.
 *   - set_insecure()               : counted, to prove the verify_peer=false policy reaches the socket.
 *   - disconnect()                 : counted, to prove failure/deadline paths tear the socket down once.
 * It is kept inline (single consumer, no shared header) per the restructure spec.
 *
 * Contracts asserted (each an exact-count / exact-value oracle, never a tautology):
 *   - InvalidUriResumesEmpty              : an unparseable URI resolves to a disengaged optional.
 *   - RefusedPortCallbackFiresExactlyOnce : a *real* refused port delivers the callback once, empty.
 *   - DirectHandshakeSuccessDeliversOpen  : handshake {1} on a direct connect → open, n_connect once.
 *   - DirectHandshakeFailureClosesAndVerifyPeerOff : handshake {-1} with verify_peer=false → empty,
 *                                            and set_insecure() ran exactly once (policy reached the
 *                                            socket) and disconnect() ran exactly once.
 *   - DeadlineCompletesPendingHandshakeOnce: a pending handshake that never resolves is failed by the
 *                                            deadline exactly once; socket disconnected once.
 *   - IoEventCompletesPendingHandshake    : scripted {0,1} → the second EV_WRITE turn finalizes open;
 *                                            ≥2 get_optval probes, no disconnect.
 *   - IoEventFailsPendingHandshake        : scripted {-1} after EINPROGRESS → empty; one probe, one
 *                                            disconnect.
 *   - AwaiterWithFakeSocketMovesResult    : the coroutine awaiter over connect_with_socket resumes
 *                                            engaged and the fake's n_connect ran once.
 *   - DestroyedAwaiterIgnoresLateCallback : an awaiter destroyed before completion never touches a
 *                                            dangling frame; a later independent connect still tears
 *                                            its own socket down exactly once.
 *   - ParallelAwaitersAllCompleteOnRefused: N coroutine awaiters on a refused port each resume empty.
 *
 * De-flake: every wait is `qb::io::test::pump_until` (loud bounded timeout). A *direct* success is
 * delivered synchronously inside connect(); a *failure* (and any pending path) is delivered from the
 * event loop on a later turn — so the failure tests pump and the direct-success test does not,
 * matching the connector's documented completion ordering.
 *
 * @ingroup Tests
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <qb/io/async/coroutine.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

// ---------------------------------------------------------------------------
// Fixture: fresh per-test event loop in SetUp(), coroutine-scheduler + watcher
// teardown in TearDown() (the established coroutine-suite idiom).
// ---------------------------------------------------------------------------
class TcpConnectorStateMachineTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }

    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

// ---------------------------------------------------------------------------
// Bind an ephemeral v4 loopback listener, close it, and return its now-refusing
// port. Used by the two tests that need a *real* refusal (the connector's
// synchronous-vs-deferred failure delivery is a real-fd behaviour worth proving
// against the kernel, not a mock).
// ---------------------------------------------------------------------------
unsigned short
refused_port() {
    qb::io::tcp::listener listener;
    EXPECT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    listener.disconnect();
    listener.close();
    return port;
}

std::string
refused_uri(unsigned short port) {
    return "tcp://127.0.0.1:" + std::to_string(port);
}

#ifdef _WIN32
// Windows has no socketpair(). Emulate a CONNECTED loopback TCP pair so the fake
// socket has a real, writable, wepoll-registerable handle: arm_io() accepts it
// (is_open()/native_handle() valid) AND the EV_WRITE readiness turn actually fires
// (a connected loopback socket is immediately writable), so the connector's
// post-EV_WRITE SO_ERROR / handshake state machine is exercised on Windows exactly
// as the POSIX socketpair drives it. The connector never reads/writes these fds (it
// consults the scripted handshake_status()/get_optval()), so the peer end is inert.
// qb stores socket handles as int on Windows too (it casts SOCKET->int for
// libev/wepoll), so matching the fake's int fd here is consistent with the framework.
inline bool
make_loopback_pair(int &a, int &b) {
    a = b      = -1;
    SOCKET lst = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lst == INVALID_SOCKET)
        return false;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    int  len             = static_cast<int>(sizeof(addr));
    bool ok              = false;
    if (::bind(lst, reinterpret_cast<sockaddr *>(&addr), len) == 0 && ::listen(lst, 1) == 0
        && ::getsockname(lst, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
        SOCKET cli = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (cli != INVALID_SOCKET) {
            if (::connect(cli, reinterpret_cast<sockaddr *>(&addr), len) == 0) {
                SOCKET srv = ::accept(lst, nullptr, nullptr);
                if (srv != INVALID_SOCKET) {
                    a  = static_cast<int>(cli);
                    b  = static_cast<int>(srv);
                    ok = true;
                }
            }
            if (!ok)
                ::QB_CLOSESOCKET(cli);
        }
    }
    ::QB_CLOSESOCKET(lst);
    return ok;
}
#endif

// ---------------------------------------------------------------------------
// FakeConnectorSocket — a programmable stand-in for the connector's Socket_.
//
// All observable counters live in a shared `state` block so the test holds one
// handle while the connector holds a moved-in copy that points at the same state
// (the connector moves the socket through several internal hops; sharing the
// state keeps the assertions visible from the test side). Mirrors the surface the
// connector's `if constexpr` probes require:
//   handshake_status() present  -> connector uses the scripted finalize path.
//   get_optval<int>(...)        -> post-EV_WRITE SO_ERROR gate.
//   set_insecure()              -> verify_peer policy hook.
// A real socketpair fd backs is_open()/native_handle() so arm_io() accepts it.
// ---------------------------------------------------------------------------
class FakeConnectorSocket {
public:
    enum class connect_result { direct, pending, fail };

    struct state {
        connect_result   result = connect_result::direct;
        std::vector<int> handshake_results{1};
        std::size_t      handshake_index    = 0u;
        int              fd                 = -1;
        int              peer_fd            = -1;
        int              n_connect_calls    = 0;
        int              disconnect_calls   = 0;
        int              connected_calls    = 0;
        int              get_optval_calls   = 0;
        int              set_insecure_calls = 0;
        int              so_error           = 0;
        int              get_optval_result  = 0;
    };

private:
    std::shared_ptr<state> _state;

    void
    ensure_fd() {
        if (_state->fd >= 0)
            return;
#ifdef _WIN32
        make_loopback_pair(_state->fd, _state->peer_fd);
#else
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            _state->fd      = fds[0];
            _state->peer_fd = fds[1];
        }
#endif
    }

public:
    FakeConnectorSocket()
        : _state(std::make_shared<state>()) {}

    explicit FakeConnectorSocket(std::shared_ptr<state> shared_state)
        : _state(std::move(shared_state)) {}

    [[nodiscard]] std::shared_ptr<state>
    shared_state() const noexcept {
        return _state;
    }

    [[nodiscard]] bool
    is_open() const noexcept {
        return _state && _state->fd >= 0;
    }

    [[nodiscard]] int
    native_handle() const noexcept {
        return _state ? _state->fd : -1;
    }

    int
    n_connect(qb::io::uri const &) {
        ++_state->n_connect_calls;
        if (_state->result == connect_result::fail) {
            qb::io::socket::set_last_errno(ECONNREFUSED);
            return -1;
        }

        ensure_fd();
        if (_state->result == connect_result::pending) {
            qb::io::socket::set_last_errno(EINPROGRESS);
            return -1;
        }
        return 0;
    }

    // Scripted handshake: returns the next entry in handshake_results, clamping at
    // the last entry once exhausted (so a {0,1} script reports pending, then done,
    // then stays done). finalize_transport_connect() reads this: >0 done, 0 pending,
    // <0 failed.
    int
    handshake_status() {
        const auto index  = std::min(_state->handshake_index, _state->handshake_results.size() - 1u);
        const auto result = _state->handshake_results[index];
        if (_state->handshake_index + 1u < _state->handshake_results.size())
            ++_state->handshake_index;
        return result;
    }

    int
    connected() {
        ++_state->connected_calls;
        return 0;
    }

    template <typename T>
    int
    get_optval(int, int, T &out) {
        ++_state->get_optval_calls;
        out = static_cast<T>(_state->so_error);
        return _state->get_optval_result;
    }

    void
    set_insecure() {
        ++_state->set_insecure_calls;
    }

    void
    disconnect() {
        ++_state->disconnect_calls;
#ifdef _WIN32
        if (_state->fd >= 0) {
            ::QB_CLOSESOCKET(static_cast<SOCKET>(_state->fd));
            _state->fd = -1;
        }
        if (_state->peer_fd >= 0) {
            ::QB_CLOSESOCKET(static_cast<SOCKET>(_state->peer_fd));
            _state->peer_fd = -1;
        }
#else
        if (_state->fd >= 0) {
            ::close(_state->fd);
            _state->fd = -1;
        }
        if (_state->peer_fd >= 0) {
            ::close(_state->peer_fd);
            _state->peer_fd = -1;
        }
#endif
    }
};

// Transport adapter so connect_with_socket<FakeConnectorTransport>(...) resolves
// the socket type from a transport, exactly like qb::io::transport::tcp.
struct FakeConnectorTransport {
    using transport_io_type = FakeConnectorSocket;
};

} // namespace

// ---------------------------------------------------------------------------
// A URI that cannot be connected resolves to a disengaged optional (no crash,
// no hang). Use a DOTTED RFC-6761 ".invalid" host (guaranteed NXDOMAIN): the qb
// uri parser is lenient and treats the old single-label "uri" as a hostname, and
// the connector resolves it synchronously via getaddrinfo. On Windows a single-
// label name triggers LLMNR + NetBIOS fallback (~2.7s with retries), which raced
// past pump_until's 2s budget. A dotted ".invalid" name skips single-label
// resolution and gets a fast NXDOMAIN on every platform. The connect deadline
// (100ms) never even arms because resolution fails first.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, InvalidUriResumesEmpty) {
    std::atomic<bool> done{false};
    bool              connected = true;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{"tcp://no-such-host.invalid:80"}, 100ms);
        connected   = socket.has_value();
        done.store(true);
        co_return;
    });

    // Generous budget: resolution is fast (NXDOMAIN), but give slow-DNS environments headroom.
    EXPECT_TRUE(pump_until([&] { return done.load(); }, 6s)) << "invalid-uri coroutine never completed";

    ASSERT_TRUE(done.load());
    EXPECT_FALSE(connected);
}

// ---------------------------------------------------------------------------
// The callback connector against a *real* refused port delivers the callback
// exactly once, with an empty socket (the deferred-failure path must not double-
// fire or fire zero times).
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, RefusedPortCallbackFiresExactlyOnce) {
    const unsigned short port = refused_port();
    ASSERT_GT(port, 0u);

    std::atomic<int> completions{0};
    bool             connected = true;

    qb::io::async::tcp::connect<qb::io::tcp::socket>(
        qb::io::uri{refused_uri(port)},
        [&](qb::io::tcp::socket &&socket) {
            connected = socket.is_open();
            completions.fetch_add(1);
        },
        500ms);

    EXPECT_TRUE(pump_until([&] { return completions.load() > 0; })) << "refused callback never fired";

    EXPECT_EQ(completions.load(), 1);
    EXPECT_FALSE(connected);
}

// ---------------------------------------------------------------------------
// Direct connect (n_connect == 0) with a handshake script of {1}: delivered
// synchronously, open, exactly one n_connect. (No pump — a direct success is the
// only completion the connector delivers inline.)
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, DirectHandshakeSuccessDeliversOpen) {
    auto shared               = std::make_shared<FakeConnectorSocket::state>();
    shared->result            = FakeConnectorSocket::connect_result::direct;
    shared->handshake_results = {1};

    std::atomic<int> completions{0};
    bool             connected = false;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:1"},
        [&](FakeConnectorSocket &&socket) {
            connected = socket.is_open();
            ++completions;
        },
        0ms);

    EXPECT_EQ(completions.load(), 1);
    EXPECT_TRUE(connected);
    EXPECT_EQ(shared->n_connect_calls, 1);
    EXPECT_EQ(shared->disconnect_calls, 0);
}

// ---------------------------------------------------------------------------
// Direct connect with handshake {-1} (immediate handshake failure) AND
// verify_peer=false: the failure is delivered from the event loop (deferred), the
// socket is empty, and the verify_peer=false policy reached the socket exactly
// once via set_insecure(), with a single disconnect(). This is the dossier's
// "verify_peer effect on set_insecure" contract.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, DirectHandshakeFailureClosesAndVerifyPeerOff) {
    auto shared               = std::make_shared<FakeConnectorSocket::state>();
    shared->result            = FakeConnectorSocket::connect_result::direct;
    shared->handshake_results = {-1};

    std::atomic<int> completions{0};
    bool             connected = true;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:2"},
        [&](FakeConnectorSocket &&socket) {
            connected = socket.is_open();
            ++completions;
        },
        0ms, /*verify_peer=*/false);

    // A connect *failure* is delivered from the event loop (never re-entrantly inside
    // connect()), so it completes on a later turn — pump for it.
    EXPECT_TRUE(pump_until([&] { return completions.load() > 0; })) << "handshake-failure never delivered";

    EXPECT_EQ(completions.load(), 1);
    EXPECT_FALSE(connected);
    EXPECT_EQ(shared->n_connect_calls, 1);
    EXPECT_EQ(shared->set_insecure_calls, 1); // verify_peer=false policy reached the socket
    EXPECT_EQ(shared->disconnect_calls, 1);
}

// ---------------------------------------------------------------------------
// A pending handshake (n_connect EINPROGRESS, handshake stuck at {0}) that never
// resolves is failed by the deadline — exactly once, with one disconnect.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, DeadlineCompletesPendingHandshakeOnce) {
    auto shared               = std::make_shared<FakeConnectorSocket::state>();
    shared->result            = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {0};

    std::atomic<int> completions{0};
    bool             connected = true;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:3"},
        [&](FakeConnectorSocket &&socket) {
            connected = socket.is_open();
            ++completions;
        },
        1ms);

    EXPECT_TRUE(pump_until([&] { return completions.load() > 0; })) << "deadline never fired";

    EXPECT_EQ(completions.load(), 1);
    EXPECT_FALSE(connected);
    EXPECT_EQ(shared->n_connect_calls, 1);
    EXPECT_EQ(shared->disconnect_calls, 1);
}

// ---------------------------------------------------------------------------
// Pending connect whose handshake script is {0,1}: the first EV_WRITE turn reports
// pending (re-arms EV_READ|EV_WRITE), the second reports done and delivers an open
// socket. ≥2 get_optval probes (one per readiness turn), and NO disconnect.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, IoEventCompletesPendingHandshake) {
#ifdef _WIN32
    // This scripted handshake {0,1} needs TWO EV_WRITE turns. On POSIX the mock's
    // connected socketpair is level-triggered (EPOLLOUT re-fires every loop turn
    // while writable), so the second turn arrives for free. On Windows, wepoll
    // signals write-readiness only ONCE for a statically-writable socket (IOCP is
    // edge-ish for EPOLLOUT) — a mock with no data flow never produces the second
    // turn, so the handshake can't be driven this way. Real multi-turn handshakes
    // work on Windows because actual TLS data flow re-triggers readiness; that path
    // is covered end-to-end by system/coroutine/tcp-connector-loopback.cpp.
    GTEST_SKIP() << "wepoll signals EV_WRITE once for a static socket; the scripted "
                    "two-turn handshake needs POSIX level-triggered write-readiness. "
                    "Real multi-turn handshakes are covered by tcp-connector-loopback.cpp.";
#endif
    auto shared               = std::make_shared<FakeConnectorSocket::state>();
    shared->result            = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {0, 1};

    std::atomic<int> completions{0};
    bool             connected = false;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:7"},
        [&](FakeConnectorSocket &&socket) {
            connected = socket.is_open();
            ++completions;
        },
        500ms);

    EXPECT_TRUE(pump_until([&] { return completions.load() > 0; })) << "pending handshake never finalized";

    EXPECT_EQ(completions.load(), 1);
    EXPECT_TRUE(connected);
    EXPECT_GE(shared->get_optval_calls, 2);
    EXPECT_EQ(shared->disconnect_calls, 0);
}

// ---------------------------------------------------------------------------
// Pending connect whose handshake reports {-1} on the first readiness turn:
// delivered empty, exactly one get_optval probe, exactly one disconnect.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, IoEventFailsPendingHandshake) {
    auto shared               = std::make_shared<FakeConnectorSocket::state>();
    shared->result            = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {-1};

    std::atomic<int> completions{0};
    bool             connected = true;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:8"},
        [&](FakeConnectorSocket &&socket) {
            connected = socket.is_open();
            ++completions;
        },
        500ms);

    EXPECT_TRUE(pump_until([&] { return completions.load() > 0; })) << "pending handshake never failed";

    EXPECT_EQ(completions.load(), 1);
    EXPECT_FALSE(connected);
    EXPECT_EQ(shared->get_optval_calls, 1);
    EXPECT_EQ(shared->disconnect_calls, 1);
}

// ---------------------------------------------------------------------------
// The coroutine awaiter over connect_with_socket<FakeConnectorTransport> resumes
// with an engaged optional on a direct {1} handshake, and the fake's n_connect ran
// once (the existing socket was moved through and used).
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, AwaiterWithFakeSocketMovesResult) {
    auto shared               = std::make_shared<FakeConnectorSocket::state>();
    shared->result            = FakeConnectorSocket::connect_result::direct;
    shared->handshake_results = {1};

    std::atomic<bool> done{false};
    bool              connected = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect_with_socket<FakeConnectorTransport>(FakeConnectorSocket{shared},
                                                                                               qb::io::uri{"tcp://fake.local:4"}, 0ms);
        connected   = socket.has_value() && socket->is_open();
        done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "connect_with_socket awaiter never resumed";

    ASSERT_TRUE(done.load());
    EXPECT_TRUE(connected);
    EXPECT_EQ(shared->n_connect_calls, 1);
}

// ---------------------------------------------------------------------------
// An awaiter constructed and destroyed before it ever completes must not leave a
// dangling coroutine frame for a late callback to resume (the awaiter's dtor
// flips active=false). We then run a separate, independent connect on a fresh fake
// to prove the loop is healthy and tears that socket down exactly once.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, DestroyedAwaiterIgnoresLateCallback) {
    {
        auto awaiter = qb::io::async::tcp::connect_awaiter<FakeConnectorSocket>{qb::io::uri{"tcp://fake.local:5"}, 1ms};
        EXPECT_FALSE(awaiter.await_ready());
        // awaiter destroyed here without ever being co_awaited / resumed.
    }

    auto shared               = std::make_shared<FakeConnectorSocket::state>();
    shared->result            = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {0};

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:6"}, [](FakeConnectorSocket &&) {}, 1ms);

    EXPECT_TRUE(pump_until([&] { return shared->disconnect_calls > 0; })) << "independent connect never tore down";

    EXPECT_EQ(shared->disconnect_calls, 1);
}

// ---------------------------------------------------------------------------
// N coroutine awaiters racing the same refused port each resume empty and all
// complete (exact counts: every awaiter completes, zero connect).
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, ParallelAwaitersAllCompleteOnRefused) {
    const unsigned short port = refused_port();
    ASSERT_GT(port, 0u);

    constexpr int     kConnectors = 5;
    std::atomic<int>  completions{0};
    std::atomic<int>  successes{0};
    const std::string uri = refused_uri(port);

    for (int i = 0; i < kConnectors; ++i) {
        coro_scheduler().spawn([&, uri]() -> task<void> {
            auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{uri}, 500ms);
            if (socket)
                ++successes;
            ++completions;
            co_return;
        });
    }

    EXPECT_TRUE(pump_until([&] { return completions.load() == kConnectors; }, 3s)) << "not all refused awaiters completed";

    EXPECT_EQ(completions.load(), kConnectors);
    EXPECT_EQ(successes.load(), 0);
}

// ---------------------------------------------------------------------------
// Leak regression (connector.h deliver_failure_deferred): a SYNCHRONOUS connect failure schedules a
// 1 ns deferred-failure Timeout on a path that arms NO io watcher — so it installs NO
// on_listener_teardown reclaim hook (that hook lives only in arm_io). If the listener is torn down
// BEFORE the deferred callback fires (engine/VirtualCore shutdown, or teardown in the same tick as
// the sync failure), clear() destroys that Timeout. The deferred callback must own the connector via
// a STRONG capture so destroying the Timeout reclaims the connector + its captured user callback
// (and, for the coroutine path, the awaiter state co-owned by it). A self_hold_ self-cycle + a
// weak_ptr capture would NOT be reclaimed and would leak.
//
// We trigger the synchronous-failure path (FakeConnectorSocket result=fail → n_connect returns
// ECONNREFUSED inline → run() reaches deliver_failure_deferred), then tear the loop down WITHOUT
// pumping (so the 1 ns Timeout never fires), and assert via a weak_ptr to a sentinel OWNED by the
// user callback that the whole graph is freed. Pre-fix this test fails (sentinel still alive after
// teardown) and LSan additionally reports the connector leak under the sanitize build.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorStateMachineTest, SyncFailureDeferredCallbackReclaimedOnListenerTeardown) {
    auto shared    = std::make_shared<FakeConnectorSocket::state>();
    shared->result = FakeConnectorSocket::connect_result::fail; // n_connect → ECONNREFUSED, synchronously

    auto               sentinel      = std::make_shared<int>(7); // owned by the user callback below
    std::weak_ptr<int> weak_sentinel = sentinel;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:1"},
        [s = sentinel](FakeConnectorSocket &&) { (void) s; }, // the connector's func_ owns the sentinel
        0ms);
    sentinel.reset(); // the ONLY remaining ref to *sentinel is now inside the connector's captured callback

    // Deliberately do NOT pump: the 1 ns deferred Timeout has not fired. Its lambda strong-holds the
    // connector (the local op shared_ptr created inside connect() is already gone). The connector
    // (and the sentinel it transitively owns) must therefore still be alive here.
    EXPECT_FALSE(weak_sentinel.expired()) << "connector should still be alive (held by the pending deferred Timeout)";

    // Tear the loop down WITHOUT pumping. clear() destroys the pending deferred Timeout; with the
    // strong-capture fix that releases the connector → user callback → sentinel.
    qb::io::async::listener::current.clear();

    EXPECT_TRUE(weak_sentinel.expired()) << "LEAK: the deferred-failure Timeout was destroyed by listener teardown but never released "
                                            "the connector (self-hold cycle) — connector + user callback leaked";
    EXPECT_EQ(shared->n_connect_calls, 1) << "the synchronous-failure path must have been taken";
}
