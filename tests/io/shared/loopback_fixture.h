/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/loopback_fixture.h
 * @brief TCP loopback scaffolding shared by the qb-io transport / session / coroutine suites.
 *
 * Every qb-io system test that needs a *real* connected socket pair faces the same chore: bind a
 * listener, learn which port the kernel handed out, spin up a server thread to accept, drive a
 * client, then join and tear everything down without leaking a thread or a descriptor. These
 * helpers hoist that chore — extracted verbatim from the anonymous-namespace helpers in
 * test-tcp-socket.cpp (the established source of truth) and lifted into a single shared header so
 * the split targets (the system tcp, udp, and session suites, plus
 * system/coroutine/tcp-connector-loopback) stop carrying byte-for-byte clones.
 *
 *   - `with_tcp_pair`               — RAII driver: binds an *ephemeral* (`:0`) v4 loopback listener,
 *                                     reads back the OS-assigned port, runs `server(accepted)` on a
 *                                     joined thread while `client(port)` runs on the caller, and
 *                                     guarantees the server thread is joined before returning. The
 *                                     `listener` is a local, so it is closed on scope exit — no fixed
 *                                     port, no manual cleanup, no inter-run collisions on CI.
 *   - `accept_low_level_connections`— deadline-bounded non-blocking accept loop over a raw
 *                                     `qb::io::socket` listener; counts `expected_count` accepts
 *                                     (each accepted handle is wrapped so it is closed on the spot).
 *   - `accept_tcp_connections`      — blocking accept of exactly `expected_count` `tcp::socket`
 *                                     connections, each immediately disconnected (server-side churn).
 *   - `reserve_free_tcp_port`       — bind `:0`, read back the kernel-assigned port, close, return it.
 *                                     The port is *probably* free for the brief window after close;
 *                                     used by the explicit-local-port connect tests to obtain a port
 *                                     to bind the client to. Inherently racy by nature (TOCTOU) — that
 *                                     is acceptable for loopback test scaffolding.
 *
 * CRITICAL invariant (per the restructure spec): NO fixed ports anywhere — always bind `:0` and read
 * the assignment back via `local_endpoint().port()`. This is the only collision-free strategy for a
 * parallel `ctest -j`.
 *
 * The accept helpers take gtest assertion macros, so a failed accept aborts the *helper* (an
 * `ASSERT_*` returns from the enclosing void function) — identical to the original in-file behavior;
 * the server thread then unwinds and the joining `with_tcp_pair` still completes. All helpers live in
 * namespace `qb::io::test`. POSIX-only raw-syscall bits are guarded with `#ifndef _WIN32`; the public
 * helpers themselves are portable (they go through the qb socket wrapper).
 */

#ifndef QB_IO_TESTS_SHARED_LOOPBACK_FIXTURE_H
#define QB_IO_TESTS_SHARED_LOOPBACK_FIXTURE_H

#include <chrono>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include <qb/io/system/sys__socket.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

namespace qb::io::test {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// RAII loopback driver: ephemeral v4 listener + joined server thread + client.
//
// `server` is invoked once on a dedicated thread with the freshly `accept`ed
// `qb::io::tcp::socket` (moved in). `client` is invoked on the calling thread
// with the kernel-assigned port. The server thread is always joined before
// returning, and the listener (a local) is closed on scope exit — so there is
// no leaked descriptor and no fixed port to collide on under `ctest -j`.
//
// Templated (and therefore inline-by-language) so it lives cleanly in a header
// without an ODR clash across the several suites that include it.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Deadline-bounded non-blocking accept loop over a raw `qb::io::socket` listener.
//
// Flips the listener to non-blocking and spins (polling every 5ms) until either
// `expected_count` connections have been accepted or a 3s wall-clock deadline
// elapses, then asserts the exact count. Each accepted handle is wrapped in a
// `qb::io::socket` whose destructor closes it immediately — the test only cares
// that the accept *happened*, not about the payload.
// ---------------------------------------------------------------------------
inline void
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

// ---------------------------------------------------------------------------
// Blocking accept of exactly `expected_count` `tcp::socket` connections, each
// immediately disconnected. Used as the server body when the test drives a known
// number of clients and only needs the server side to churn the accept queue.
// ---------------------------------------------------------------------------
inline void
accept_tcp_connections(qb::io::tcp::listener &listener, int expected_count) {
    for (int i = 0; i < expected_count; ++i) {
        qb::io::tcp::socket accepted;
        ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
        EXPECT_TRUE(accepted.is_open());
        accepted.disconnect();
    }
}

// ---------------------------------------------------------------------------
// Reserve (probabilistically) a free TCP port: bind `:0`, read the kernel's
// assignment back, close, and return it. The window between close and reuse is
// inherently racy (TOCTOU) — acceptable for the explicit-local-port connect
// tests that need *a* port to bind a client onto. Never used as a server port.
// ---------------------------------------------------------------------------
inline unsigned short
reserve_free_tcp_port() {
    qb::io::socket probe;
    EXPECT_EQ(probe.pserve("127.0.0.1", 0), 0);
    const auto port = probe.local_endpoint().port();
    probe.close();
    return port;
}

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_LOOPBACK_FIXTURE_H
