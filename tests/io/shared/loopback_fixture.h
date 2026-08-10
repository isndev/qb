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
 *   - `accept_within`               — deadline-bounded accept of ONE `tcp::socket` connection.
 *   - `read_some_within`            — ONE bounded `read()` of up to `cap` bytes.
 *   - `read_exact_within`           — bounded read of EXACTLY `want` bytes.
 *   - `accept_low_level_connections`— deadline-bounded non-blocking accept loop over a raw
 *                                     `qb::io::socket` listener; counts `expected_count` accepts
 *                                     (each accepted handle is wrapped so it is closed on the spot).
 *   - `accept_tcp_connections`      — bounded accept of exactly `expected_count` `tcp::socket`
 *                                     connections, each immediately disconnected (server-side churn).
 *   - `reserve_free_tcp_port`       — bind `:0`, read back the kernel-assigned port, close, return it.
 *                                     The port is *probably* free for the brief window after close;
 *                                     used by the explicit-local-port connect tests to obtain a port
 *                                     to bind the client to. Inherently racy by nature (TOCTOU) — that
 *                                     is acceptable for loopback test scaffolding.
 *
 * ---------------------------------------------------------------------------------------------
 * WHY EVERY WAIT HERE IS NON-BLOCKING  (read before "simplifying" one back to a blocking call)
 * ---------------------------------------------------------------------------------------------
 * A blocking `accept()` / `read()` parks the calling thread inside the kernel. Any deadline the
 * surrounding code carries — the `while (… < deadline)` of a read loop, the budget of a
 * `pump_until`, the implicit "this thread will finish so `join()` returns" of the caller — is
 * evaluated only BETWEEN calls, so none of it can fire while the call is parked. The bound is
 * therefore not a bound at all: it is a comment. What actually ends the wait is the peer, and
 * a peer that stops responding without closing its socket ends it never.
 *
 * That is not merely slow, it is a test that CANNOT REPORT THE DEFECT IT WAS WRITTEN TO CATCH.
 * The assertion the author wrote — "the connector hands back a usable socket", "connect() reaches
 * the loopback server" — never executes; the run turns into a ctest `***Timeout`, which reads in a
 * CI log as infrastructure flake rather than as a finding. A real regression becomes
 * indistinguishable from a slow runner, and under `ctest -j` it holds its slot forever.
 *
 * So every wait in this header is a NON-BLOCKING call in a loop that re-checks a `steady_clock`
 * deadline it can actually reach, and reports a bounded failure to the caller. The cost of the
 * polling is 1 ms of sleep per idle iteration on a path that, healthy, never iterates twice.
 * `accept_low_level_connections` below was already written this way; these helpers make it the
 * rule rather than the exception.
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
// Default budget for the bounded helpers below.
//
// Sized from both ends: a healthy loopback accept or echo completes in single-digit
// milliseconds here (the whole tcp suite runs in ~60 ms), so 3 s is three orders of
// magnitude of headroom and cannot fire on a merely loaded runner; and it is far
// below ctest's timeout, so a wedged peer surfaces as the caller's own assertion
// message rather than as a killed test binary. Callers that already carried an
// explicit deadline pass it through instead of taking this default, so no existing
// number changes.
// ---------------------------------------------------------------------------
inline constexpr std::chrono::milliseconds kLoopbackWaitBudget{3000};

// ---------------------------------------------------------------------------
// accept_within — accept ONE connection, with a deadline the accept cannot defeat.
//
// Flips the listener non-blocking so `accept()` returns immediately when the queue
// is empty, then polls until a connection lands or `budget` elapses. Returns true
// iff `out` holds a freshly accepted socket. A false return is the loud timeout:
// callers must surface it (ASSERT_TRUE) rather than proceeding on an unopened socket.
//
// The accepted socket is unaffected: `qb::io::socket::accept_n` sets every accepted
// descriptor non-blocking regardless of the listener's own mode.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool
accept_within(qb::io::tcp::listener &listener, qb::io::tcp::socket &out, std::chrono::milliseconds budget = kLoopbackWaitBudget) {
    listener.set_nonblocking(true);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        if (listener.accept(out) == qb::io::SocketStatus::Done)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
}

// ---------------------------------------------------------------------------
// read_some_within — ONE bounded read of up to `cap` bytes.
//
// Returns exactly what a single `read()` would: the byte count of the first read
// that produced data, or the failing return of the last attempt if nothing arrived
// within `budget`. Retrying only while the socket has nothing to give is what makes
// this a drop-in for a blocking `read()` — the value the caller asserts on is still
// the result of ONE read() call, so a test that pins "the payload arrived in a single
// segment" keeps pinning exactly that.
// ---------------------------------------------------------------------------
inline int
read_some_within(qb::io::tcp::socket &sock, void *dest, std::size_t cap, std::chrono::milliseconds budget = kLoopbackWaitBudget) {
    sock.set_nonblocking(true);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        const int n = sock.read(dest, cap);
        if (n > 0)
            return n;
        if (std::chrono::steady_clock::now() >= deadline)
            return n; // -1: nothing arrived (empty socket, peer close, or error)
        std::this_thread::sleep_for(1ms);
    }
}

// ---------------------------------------------------------------------------
// read_exact_within — read EXACTLY `want` bytes, or as many as arrived by the deadline.
//
// The read-loop form: returns the number of bytes actually collected, so a caller
// asserting `== want` gets a precise short-read failure instead of a hang when the
// peer stops mid-message. `tcp::socket::read()` reports both "nothing yet" and
// "peer closed" as -1, and telling them apart needs an errno read that a later qb
// call can clobber — so both simply run the budget out and return the short count.
// A failure therefore costs `budget`, never the run.
// ---------------------------------------------------------------------------
inline std::size_t
read_exact_within(qb::io::tcp::socket &sock, void *dest, std::size_t want, std::chrono::milliseconds budget = kLoopbackWaitBudget) {
    sock.set_nonblocking(true);
    auto       *out      = static_cast<char *>(dest);
    std::size_t got      = 0;
    const auto  deadline = std::chrono::steady_clock::now() + budget;
    while (got < want) {
        const int n = sock.read(out + got, want - got);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(1ms);
    }
    return got;
}

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
        // Bounded: a client that never arrives (its connect regressed, or its
        // ASSERT aborted the client body before connecting) must fail this thread
        // fast. A blocking accept here parks forever and takes `join()` below with
        // it, converting the client's own failed assertion into a hung binary.
        ASSERT_TRUE(accept_within(listener, accepted)) << "no client connected within the accept budget";
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
// Bounded accept of exactly `expected_count` `tcp::socket` connections, each
// immediately disconnected. Used as the server body when the test drives a known
// number of clients and only needs the server side to churn the accept queue.
//
// Bounded per connection for the same reason as `with_tcp_pair`: every caller runs
// this on a thread it later joins unconditionally, so one connect that does not
// happen (a failed EXPECT in the client half leaves the count short) would park
// this loop forever and hang the join instead of reporting the short count.
// ---------------------------------------------------------------------------
inline void
accept_tcp_connections(qb::io::tcp::listener &listener, int expected_count) {
    for (int i = 0; i < expected_count; ++i) {
        qb::io::tcp::socket accepted;
        ASSERT_TRUE(accept_within(listener, accepted)) << "only " << i << " of " << expected_count << " clients connected within the accept budget";
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
