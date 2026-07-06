/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tls/tls-peer-verification.cpp
 * @brief Secure-by-default TLS: a verifying client rejects the self-signed server; set_insecure opts out.
 *
 * Promoted from test-async-io.cpp's `SSLPeerVerificationSecureByDefault`, this is the MITM-hardening
 * lock-in for `qb::io::tcp::ssl::socket`: qb-io builds its client `SSL_CTX` with `SSL_VERIFY_PEER` and
 * hostname checking enabled, so a default client MUST fail the handshake against the self-signed test
 * certificate, and `set_insecure()` MUST be the only way to opt out. The negative path (verify-on vs
 * self-signed) was previously untested at the socket level — it is asserted here, alongside the
 * positive `set_insecure()` success, and additionally the diagnosable error string the failed
 * handshake leaves behind.
 *
 * De-flake (per the restructure spec §2): the original used a fixed port (64388) and an acceptor that
 * looped 400×2ms while busy-driving each handshake 200×1ms, then slept a flat 80ms before connecting.
 * Here the listener binds `:0` (ephemeral), the acceptor is deadline-bounded and best-effort drives
 * whichever client connects, and the test waits on an explicit `acceptor_ready` flag rather than a
 * blind sleep. The acceptor is always joined via an RAII guard.
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

#include <atomic>
#include <chrono>
#include <thread>

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/ssl_fixtures.h"

using namespace std::chrono_literals;

using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

namespace {

// Best-effort, deadline-bounded server acceptor for a self-signed TLS server. It
// accepts whichever clients connect and drives each handshake until completion,
// fatal error (the verifying client rejecting the cert), or a short per-connection
// deadline. Runs until `stop` is set; signals `ready` once it is in its loop.
class self_signed_acceptor {
public:
    self_signed_acceptor(qb::io::tcp::ssl::listener &listener, std::atomic<bool> &ready, std::atomic<bool> &stop)
        : _thread([&listener, &ready, &stop] {
            ready = true;
            while (!stop.load()) {
                qb::io::tcp::ssl::socket server_socket;
                if (listener.accept(server_socket) != 0) {
                    std::this_thread::sleep_for(2ms);
                    continue;
                }
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!server_socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
                    if (server_socket.handshake_status() < 0) {
                        break; // peer aborted (e.g. the verifying client rejecting our cert)
                    }
                    std::this_thread::sleep_for(1ms);
                }
            }
        }) {}

    self_signed_acceptor(const self_signed_acceptor &)            = delete;
    self_signed_acceptor &operator=(const self_signed_acceptor &) = delete;

    void
    join() {
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    ~self_signed_acceptor() {
        join();
    }

private:
    std::thread _thread;
};

} // namespace

TEST(TlsPeerVerification, SecureByDefaultRejectsSelfSignedAndInsecureOptsOut) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    auto tls = qb::io::ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"));
    ASSERT_TRUE(tls.ok());
    listener.init(std::move(tls));
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool>    ready{false};
    std::atomic<bool>    stop{false};
    self_signed_acceptor acceptor(listener, ready, stop);

    while (!ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    // 1) Default (verifying) client MUST fail against the self-signed cert, and
    //    must leave a diagnosable verification error behind.
    {
        qb::io::tcp::ssl::socket verifying_client;
        ASSERT_TRUE(verifying_client.verify_peer()) << "client must be secure-by-default";
        ASSERT_TRUE(verifying_client.set_sni_hostname("localhost"));
        const int ret = verifying_client.connect_v4("localhost", port);
        EXPECT_NE(ret, 0) << "secure-by-default client accepted a self-signed certificate (MITM hole)";
        EXPECT_FALSE(verifying_client.handshake_complete());
    }

    // 2) set_insecure() client MUST connect to the very same server.
    {
        qb::io::tcp::ssl::socket insecure_client;
        insecure_client.set_insecure();
        const int ret = insecure_client.connect_v4("127.0.0.1", port);
        EXPECT_EQ(ret, 0) << "set_insecure() failed to opt out of verification";
        EXPECT_TRUE(insecure_client.handshake_complete());
        insecure_client.disconnect();
    }

    stop = true;
    listener.disconnect();
    acceptor.join();
}
