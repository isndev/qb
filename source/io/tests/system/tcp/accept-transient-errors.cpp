/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the specific terms.
 */

/**
 * @file system/tcp/accept-transient-errors.cpp
 * @brief `qb::io::transport::accept` — the TRANSIENT-error remap branch and the repeated/closed
 *        accept edges that the wave-1 happy-path loopback test (transport-accept-loopback.cpp) leaves
 *        uncovered.
 *
 * transport-accept-loopback.cpp already drives accept()'s `SocketStatus::Done` (a pending connection
 * yields the accepted native handle), the no-pending `(size_t)-1`, and `flush()`/`close()`/`eof()`.
 * It does NOT reach the *transient-error remap* path inside `read()`:
 *
 *     if (ret == ECONNABORTED || ret == EMFILE || ret == ENFILE || ret == ENOMEM || ret == ENOBUFS)
 *         qb::io::socket::set_last_errno(EWOULDBLOCK);   // tell the async accept loop to RETRY
 *     return (size_t)-1;
 *
 * That branch is the DoS-hardening heart of the acceptor: a per-process fd-table exhaustion (`EMFILE`)
 * — or a peer that aborted its half-open connection (`ECONNABORTED`) — must be reported to the caller
 * as a RETRYABLE would-block (so the async loop re-arms on the next readiness event) rather than as a
 * hard listener failure that would dispose the whole server. The previous suites only ever saw the
 * native EWOULDBLOCK from an empty non-blocking accept queue, never the *remap* of a real resource
 * error onto EWOULDBLOCK.
 *
 * We force a genuine `EMFILE` deterministically by clamping this process's `RLIMIT_NOFILE` to the
 * current high-water mark, then presenting the acceptor a *real pending connection* it cannot accept
 * (the kernel has the SYN queued, but accept(2) fails to allocate an fd). read() must:
 *   - return `(size_t)-1` (no socket produced), AND
 *   - leave `errno == EWOULDBLOCK` (the remap), proving the branch ran — a raw EMFILE would leave the
 *     original errno and trip the server-disposing `not_recv_error()` path.
 *
 * POSIX-only (`WINDOWS_EXCLUDE`): `setrlimit(RLIMIT_NOFILE)` / `getrlimit` are POSIX; Windows has no
 * per-process fd-table cap to clamp this way. The fd-limit clamp is fully restored on scope exit by an
 * RAII guard so the test cannot starve a parallel `ctest -j` of descriptors.
 *
 * Signatures relied on:
 *   transport::accept: tcp::listener& transport(); std::size_t read(); void eof() const noexcept;
 *   qb::io::socket::get_last_errno() / set_last_errno(int)  (sys__socket.h:1325-1326)
 *   tcp::listener::listen_v4(uint16_t, std::string) -> int(0==Done); set_nonblocking(bool); local_endpoint().
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
#include <cerrno>
#include <chrono>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <qb/io/system/sys__socket.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/transport/accept.h>

using namespace std::chrono_literals;

namespace {

// RAII guard that clamps this process's RLIMIT_NOFILE soft limit and restores it
// on destruction. The clamp lets us provoke a genuine EMFILE from accept(2)
// without leaking the constraint into other (parallel) tests.
class FdLimitClamp {
public:
    explicit FdLimitClamp(rlim_t soft_limit)
        : _ok(false) {
        if (::getrlimit(RLIMIT_NOFILE, &_saved) != 0)
            return;
        rlimit clamped = _saved;
        clamped.rlim_cur = soft_limit;
        _ok              = (::setrlimit(RLIMIT_NOFILE, &clamped) == 0);
    }

    FdLimitClamp(const FdLimitClamp &)            = delete;
    FdLimitClamp &operator=(const FdLimitClamp &) = delete;

    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }

    ~FdLimitClamp() {
        if (_ok)
            ::setrlimit(RLIMIT_NOFILE, &_saved);
    }

private:
    rlimit _saved{};
    bool   _ok;
};

} // namespace

// ===========================================================================
// EMFILE remap: a pending connection that accept() cannot turn into an fd must
// be reported as a retryable (size_t)-1 with errno remapped to EWOULDBLOCK, so
// the async accept loop re-arms instead of disposing the server.
// ===========================================================================
TEST(AcceptTransientErrors, FdExhaustionRemapsToWouldBlockAndKeepsAcceptorAlive) {
    qb::io::transport::accept acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = acceptor.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    // Non-blocking so accept() returns immediately (the pending connection is
    // queued in the kernel; the failure we want is fd-table allocation, not a
    // wait on an empty queue).
    acceptor.transport().set_nonblocking(true);

    // A client connects so the kernel has a real pending connection to hand out.
    // It stays open (and is fully connected) BEFORE we clamp the fd table.
    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);

    // Give the SYN/ACK time to land in the listener's accept queue.
    std::this_thread::sleep_for(50ms);

    // Occupy a handful of descriptors, then clamp RLIMIT_NOFILE to the current
    // open high-water mark so the very next fd allocation (accept's new socket)
    // fails with EMFILE — deterministic and bounded, no million-fd loop. The
    // clamp is restored on scope exit by FdLimitClamp's RAII.
    std::vector<int> hogs;
    for (int i = 0; i < 8; ++i) {
        const int fd = ::dup(0);
        if (fd < 0)
            break;
        hogs.push_back(fd);
    }
    ASSERT_FALSE(hogs.empty()) << "could not reserve any descriptors";

    // The highest fd we hold + 1 is a soft ceiling at (or just above) our current
    // usage; accept()'s fresh descriptor would exceed it -> EMFILE.
    int highest = 0;
    for (const int fd : hogs)
        highest = std::max(highest, fd);

    {
        FdLimitClamp clamp(static_cast<rlim_t>(highest + 1));
        if (!clamp.ok()) {
            for (const int fd : hogs)
                ::close(fd);
            GTEST_SKIP() << "could not clamp RLIMIT_NOFILE on this host";
        }

        // Confirm the clamp actually exhausts the table: a probe dup() must now
        // fail with EMFILE. If the host left a free slot below the ceiling (sparse
        // fd table), the clamp can't force EMFILE here — skip rather than report a
        // false failure. The remap branch is the contract; this guards the SETUP.
        const int probe = ::dup(0);
        if (probe >= 0) {
            ::close(probe);
            for (const int fd : hogs)
                ::close(fd);
            GTEST_SKIP() << "fd table not exhausted under the clamp on this host (sparse fd slots)";
        }

        qb::io::socket::set_last_errno(0);
        const std::size_t handle = acceptor.read();

        // The remap branch fired: no socket produced, errno remapped to EWOULDBLOCK.
        EXPECT_EQ(handle, static_cast<std::size_t>(-1)) << "read() must not yield a socket under fd exhaustion";
        EXPECT_EQ(qb::io::socket::get_last_errno(), EWOULDBLOCK)
            << "an EMFILE accept failure must be remapped to EWOULDBLOCK (retryable), got errno="
            << qb::io::socket::get_last_errno();

        // The acceptor itself is untouched: the listener is still open and would
        // accept once fds free up. eof() is a documented no-op and must be harmless.
        EXPECT_TRUE(acceptor.transport().is_open()) << "fd exhaustion must NOT close the listener";
        acceptor.eof();
    } // clamp restored here

    for (const int fd : hogs)
        ::close(fd);
    client.disconnect();
}

// ===========================================================================
// Sanity companion (no clamp): with fds freely available the SAME pending
// connection is accepted — proving the EMFILE above was the fd ceiling, not a
// broken listener, and that read() is repeatable (a second read() with the queue
// now empty returns the no-pending (size_t)-1 WITHOUT the remap firing).
// ===========================================================================
TEST(AcceptTransientErrors, AcceptSucceedsThenSecondReadIsPlainWouldBlock) {
    qb::io::transport::accept acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = acceptor.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    acceptor.transport().set_nonblocking(true);

    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);

    // Bounded poll: the pending connection must be accepted (Done branch).
    std::size_t handle   = static_cast<std::size_t>(-1);
    const auto  deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        handle = acceptor.read();
        if (handle != static_cast<std::size_t>(-1))
            break;
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_NE(handle, static_cast<std::size_t>(-1)) << "the pending connection was never accepted";
    EXPECT_TRUE(acceptor.getAccepted().is_open());

    // flush() hands the descriptor off (moved-out semantics) so the acceptor's
    // dtor won't close it; we reclaim and close it ourselves below.
    acceptor.flush(0);
    EXPECT_EQ(acceptor.getAccepted().native_handle(), qb::io::inet::invalid_socket);

    // Second read(): the accept queue is now empty, so accept() would-blocks
    // natively — read() returns (size_t)-1 with no remap needed.
    qb::io::socket::set_last_errno(0);
    EXPECT_EQ(acceptor.read(), static_cast<std::size_t>(-1)) << "an empty queue must report no-pending as (size_t)-1";

    qb::io::socket reclaimed(static_cast<::socket_type>(handle));
    reclaimed.close();
    client.disconnect();
}
