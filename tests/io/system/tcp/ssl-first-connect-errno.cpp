/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/ssl-first-connect-errno.cpp
 * @brief The FIRST non-blocking TLS connect of a process reports its errno like every later one.
 *
 * One test, alone in its binary, on purpose: it has to be the first SSL operation the process
 * performs, and a binary with a single test is the only ordering `--gtest_shuffle` (on by default,
 * cmake/qbFunctions.cmake) cannot change.
 *
 * The defect it pins is Windows-only and first-op-only, which is why it hid in a suite that shuffles:
 * `ssl::socket::n_connect(endpoint, hostname)` returns the TCP connect's -1 (WSAEWOULDBLOCK, "in
 * progress") and then mints the SSL state, and the process's first `SSL_CTX_new()` runs OpenSSL's
 * one-time library init, whose successful Win32 calls clear the thread's last-error slot —
 * measured with a probe: that call, and only that one, turned a pending 10035 into 0. Every caller
 * reads `get_last_errno()` AFTER the return to tell "in progress" from "failed":
 * `async::tcp::connector::run()` (`qb/io/async/tcp/connector.h`) saw 0, `socket_no_error(0)` is
 * false, and the first TLS client connect of a process was delivered as a failure.
 * `ssl-socket-loopback`'s `NonBlockingConnectVariantsPrepareSslState` saw the same
 * (`n_connect result=-1 errno=0`) exactly when the shuffle drew it first — 1 of 5 presets on one
 * `verify-windows.ps1` run, 5/5 in isolation. POSIX errno is untouched by the same calls, so
 * nothing else could see it; this binary is what makes the Windows shape a measurement on every
 * platform.
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
#include <string>

#include <gtest/gtest.h>

#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/ssl/socket.h>

namespace {

// The contract every caller of n_connect() relies on: a non-zero return is "in progress" iff the
// errno read right after it says so. An errno of 0 beside a -1 is the defect, whatever the platform.
void
expect_in_progress_or_done(const char *what, int ret, int err) {
    if (ret == 0)
        return; // loopback connected synchronously — legal, nothing to read
    EXPECT_NE(err, 0) << what << ": n_connect returned " << ret << " with errno 0 — the connect's errno was "
                      << "clobbered between the TCP connect and the return";
    EXPECT_TRUE(err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << what << ": unexpected n_connect result=" << ret << " errno=" << err;
}

} // namespace

TEST(SSLFirstConnectErrno, FirstNonBlockingConnectOfTheProcessReportsInProgress) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    // First SSL operation of the process: the auto-created client context is minted inside this call.
    qb::io::tcp::ssl::socket first;
    first.set_insecure();
    const int first_ret = first.n_connect(qb::io::endpoint("127.0.0.1", port), "localhost");
    const int first_err = qb::io::socket::get_last_errno();
    expect_in_progress_or_done("first TLS connect of the process", first_ret, first_err);
    EXPECT_NE(first.ssl_handle(), nullptr);
    EXPECT_FALSE(first.handshake_complete());
    first.close();

    // Control: the second one, with OpenSSL initialised, is the shape the suite always measured.
    qb::io::tcp::ssl::socket second;
    second.set_insecure();
    const int second_ret = second.n_connect(qb::io::endpoint("127.0.0.1", port), "localhost");
    const int second_err = qb::io::socket::get_last_errno();
    expect_in_progress_or_done("second TLS connect of the process", second_ret, second_err);
    EXPECT_NE(second.ssl_handle(), nullptr);
    second.close();

    // The two must agree: whatever the first connect reported is what the second reports.
    EXPECT_EQ(first_ret, second_ret);
    if (first_ret != 0 && second_ret != 0) {
        EXPECT_EQ(first_err, second_err);
    }
}
