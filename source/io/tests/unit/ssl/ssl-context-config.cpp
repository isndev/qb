/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/ssl/ssl-context-config.cpp
 * @brief Pure SSL_CTX / SSL-object configuration logic — no socket, no handshake.
 *
 * These are the genuinely *unit*-tier cases peeled out of the former system test-ssl-socket.cpp:
 * they assert the return-code and sentinel-state contracts of the `qb::io::ssl::*` free-function
 * context helpers, the `qb::io::tcp::ssl::listener` wrapper accessors, and the pre-handshake
 * configuration surface of `qb::io::tcp::ssl::socket`. Not one of them opens a socket or performs a
 * TLS handshake — they build `SSL_CTX` / `SSL` objects in-process and drive every setter/getter with
 * a valid argument (expect success / a meaningful value) and an invalid one (expect the documented
 * sentinel: `false` / `nullptr` / `-1` / `0` / empty). The real loopback handshake lives in the
 * sibling system test system/tcp/ssl-socket-loopback.cpp.
 *
 * Cert policy (per the restructure spec §7): the harness *ships* `cert.pem` / `key.pem`, so the
 * positive-path cases that need a real cert treat its absence as a HARD FAILURE
 * (`ASSERT_TRUE(qb::io::test::require_ssl_files())`) — not a `GTEST_SKIP`. The original four
 * cert-gated `GTEST_SKIP()` masks let the entire positive configuration surface silently vanish in a
 * misconfigured checkout; here a missing cert is reported as the environment bug it is. The cert
 * locator itself is the shared `tests/shared/ssl_fixtures.h` (one source of truth across all four
 * SSL-touching targets), replacing the per-file 4-candidate clones.
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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/ssl_fixtures.h"

using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

namespace {

using ctx_guard = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

ctx_guard
guard_ctx(SSL_CTX *ctx) {
    return {ctx, SSL_CTX_free};
}

// --- OpenSSL callback stubs (signatures the setters demand) -----------------

int
always_verify(int, X509_STORE_CTX *) {
    return 1;
}

int
ocsp_callback(SSL *, void *) {
    return SSL_TLSEXT_ERR_NOACK;
}

int
sni_callback(SSL *, int *, void *) {
    return SSL_TLSEXT_ERR_OK;
}

void
keylog_callback(const SSL *, const char *) {}

void
info_callback(const SSL *, int, int) {}

void
msg_callback(int, int, int, const void *, std::size_t, SSL *, void *) {}

int
select_first_alpn(SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *) {
    if (!in || inlen == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out    = in + 1;
    *outlen = in[0];
    return SSL_TLSEXT_ERR_OK;
}

} // namespace

// ===========================================================================
// Free-function context helpers — null / invalid guards (no cert needed)
// ===========================================================================

TEST(SSLContextConfig, NullAndInvalidInputsFailCleanly) {
    EXPECT_FALSE(qb::io::ssl::attach_socket(nullptr, qb::io::inet::invalid_socket));
    EXPECT_TRUE(qb::io::ssl::get_certificate(nullptr).subject.empty());
    EXPECT_EQ(qb::io::ssl::create_client_context(nullptr), nullptr);
    EXPECT_EQ(qb::io::ssl::create_server_context(nullptr, {}, {}), nullptr);
    EXPECT_EQ(qb::io::ssl::create_server_context(TLS_server_method(), "missing-cert.pem", "missing-key.pem"), nullptr);

    EXPECT_FALSE(qb::io::ssl::load_ca_certificates(nullptr, "ca.pem"));
    EXPECT_FALSE(qb::io::ssl::load_ca_directory(nullptr, "."));
    EXPECT_FALSE(qb::io::ssl::set_cipher_list(nullptr, "HIGH"));
    EXPECT_FALSE(qb::io::ssl::set_ciphersuites_tls13(nullptr, "TLS_AES_128_GCM_SHA256"));
    EXPECT_FALSE(qb::io::ssl::set_tls_protocol_versions(nullptr, TLS1_2_VERSION, 0));
    EXPECT_FALSE(qb::io::ssl::configure_mtls_server_context(nullptr, ""));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(nullptr, "cert.pem", "key.pem"));
    EXPECT_FALSE(qb::io::ssl::set_alpn_protos_client(nullptr, {"h2"}));
    EXPECT_FALSE(qb::io::ssl::set_alpn_selection_callback_server(nullptr, select_first_alpn, nullptr));
    EXPECT_FALSE(qb::io::ssl::enable_server_session_caching(nullptr, 16));
    EXPECT_FALSE(qb::io::ssl::disable_client_session_cache(nullptr));
    EXPECT_FALSE(qb::io::ssl::set_custom_verify_callback(nullptr, always_verify, SSL_VERIFY_PEER));
    EXPECT_FALSE(qb::io::ssl::set_ocsp_stapling_client_callback(nullptr, ocsp_callback, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_ocsp_stapling_responder_server(nullptr, ocsp_callback, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_sni_hostname_selection_callback_server(nullptr, sni_callback, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_keylog_callback(nullptr, keylog_callback));
    EXPECT_FALSE(qb::io::ssl::configure_dh_parameters_server(nullptr, "dh.pem"));
    EXPECT_FALSE(qb::io::ssl::configure_ecdh_curves_server(nullptr, "prime256v1"));
    EXPECT_FALSE(qb::io::ssl::enable_post_handshake_auth_server(nullptr));

    qb::io::ssl::Session session;
    qb::io::ssl::free_session(session);
    EXPECT_FALSE(session.is_valid());
}

// ===========================================================================
// Free-function context helpers — valid vs invalid args on real contexts
// ===========================================================================

TEST(SSLContextConfig, ConfiguresClientAndServerContexts) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    const auto cert = ssl_resource_path("cert.pem");
    const auto key  = ssl_resource_path("key.pem");

    auto *client = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(client, nullptr);
    auto client_guard = guard_ctx(client);

    auto *server = qb::io::ssl::create_server_context(TLS_server_method(), cert, key);
    ASSERT_NE(server, nullptr);
    auto server_guard = guard_ctx(server);

    EXPECT_TRUE(qb::io::ssl::load_ca_certificates(client, cert));
    EXPECT_FALSE(qb::io::ssl::load_ca_certificates(client, ""));
    EXPECT_FALSE(qb::io::ssl::load_ca_directory(client, ""));
    EXPECT_TRUE(qb::io::ssl::set_cipher_list(client, "HIGH:!aNULL"));
    EXPECT_FALSE(qb::io::ssl::set_cipher_list(client, ""));
    EXPECT_TRUE(qb::io::ssl::set_ciphersuites_tls13(client, "TLS_AES_128_GCM_SHA256"));
    EXPECT_FALSE(qb::io::ssl::set_ciphersuites_tls13(client, ""));
    EXPECT_TRUE(qb::io::ssl::set_tls_protocol_versions(client, TLS1_2_VERSION, 0));
    EXPECT_TRUE(qb::io::ssl::configure_client_certificate(client, cert, key));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(client, "", key));
    EXPECT_TRUE(qb::io::ssl::set_alpn_protos_client(client, {"h2", "http/1.1"}));
    EXPECT_FALSE(qb::io::ssl::set_alpn_protos_client(client, {}));
    EXPECT_FALSE(qb::io::ssl::set_alpn_protos_client(client, {std::string(256, 'x')}));
    EXPECT_TRUE(qb::io::ssl::disable_client_session_cache(client));
    EXPECT_TRUE(qb::io::ssl::set_custom_verify_callback(client, always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(qb::io::ssl::set_ocsp_stapling_client_callback(client, ocsp_callback, nullptr));
    EXPECT_TRUE(qb::io::ssl::set_keylog_callback(client, keylog_callback));

    EXPECT_TRUE(qb::io::ssl::configure_mtls_server_context(server, cert));
    EXPECT_TRUE(qb::io::ssl::configure_mtls_server_context(server, ""));
    EXPECT_FALSE(qb::io::ssl::configure_mtls_server_context(server, "missing-ca.pem"));
    EXPECT_TRUE(qb::io::ssl::set_alpn_selection_callback_server(server, select_first_alpn, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_alpn_selection_callback_server(server, nullptr, nullptr));
    EXPECT_TRUE(qb::io::ssl::enable_server_session_caching(server, 32));
    EXPECT_TRUE(qb::io::ssl::set_ocsp_stapling_responder_server(server, ocsp_callback, nullptr));
    EXPECT_TRUE(qb::io::ssl::set_sni_hostname_selection_callback_server(server, sni_callback, nullptr));
    EXPECT_TRUE(qb::io::ssl::configure_ecdh_curves_server(server, "prime256v1"));
    EXPECT_FALSE(qb::io::ssl::configure_ecdh_curves_server(server, "not-a-curve"));
    EXPECT_FALSE(qb::io::ssl::configure_dh_parameters_server(server, "missing-dh.pem"));
    EXPECT_TRUE(qb::io::ssl::enable_post_handshake_auth_server(server));
}

TEST(SSLContextConfig, RejectsInvalidConfigurationOnRealContexts) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    const auto cert = ssl_resource_path("cert.pem");
    const auto key  = ssl_resource_path("key.pem");

    auto *client = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(client, nullptr);
    auto client_guard = guard_ctx(client);

    EXPECT_FALSE(qb::io::ssl::load_ca_certificates(client, "missing-ca.pem"));
    EXPECT_FALSE(qb::io::ssl::set_cipher_list(client, "NO-SUCH-CIPHER"));
    EXPECT_FALSE(qb::io::ssl::set_ciphersuites_tls13(client, "NO_SUCH_TLS13_SUITE"));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(client, "missing-cert.pem", key));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(client, cert, "missing-key.pem"));

    auto *server = qb::io::ssl::create_server_context(TLS_server_method(), cert, key);
    ASSERT_NE(server, nullptr);
    auto server_guard = guard_ctx(server);

    EXPECT_FALSE(qb::io::ssl::configure_mtls_server_context(server, "missing-client-ca.pem"));
    EXPECT_FALSE(qb::io::ssl::configure_ecdh_curves_server(server, "not-a-curve"));
    EXPECT_FALSE(qb::io::ssl::configure_dh_parameters_server(server, "missing-dh-params.pem"));
}

// ===========================================================================
// ssl::listener wrapper accessors — sentinel state then live context
// ===========================================================================

TEST(SSLListenerConfig, UninitializedAccessorsReturnSentinels) {
    qb::io::tcp::ssl::listener uninitialized;
    EXPECT_EQ(uninitialized.ssl_handle(), nullptr);
    EXPECT_FALSE(uninitialized.load_ca_certificates_for_client_auth("ca.pem"));
    EXPECT_FALSE(uninitialized.load_ca_directory_for_client_auth("."));
    EXPECT_FALSE(uninitialized.set_cipher_list("HIGH"));
    EXPECT_FALSE(uninitialized.set_ciphersuites_tls13("TLS_AES_128_GCM_SHA256"));
    EXPECT_FALSE(uninitialized.set_tls_protocol_versions(TLS1_2_VERSION, 0));
    EXPECT_FALSE(uninitialized.configure_mtls(""));
    EXPECT_FALSE(uninitialized.set_alpn_selection_callback(select_first_alpn, nullptr));
    EXPECT_FALSE(uninitialized.enable_session_caching());
    EXPECT_FALSE(uninitialized.set_custom_client_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_FALSE(uninitialized.set_ocsp_stapling_responder_callback(ocsp_callback, nullptr));
    EXPECT_FALSE(uninitialized.set_sni_selection_callback(sni_callback, nullptr));
    EXPECT_FALSE(uninitialized.set_keylog_callback(keylog_callback));
    EXPECT_FALSE(uninitialized.configure_dh_parameters("dh.pem"));
    EXPECT_FALSE(uninitialized.configure_ecdh_curves("prime256v1"));
    EXPECT_FALSE(uninitialized.enable_post_handshake_auth());
    EXPECT_FALSE(uninitialized.set_supported_alpn_protocols({"h2"}));
    EXPECT_EQ(uninitialized.get_min_protocol_version(), 0);
    EXPECT_EQ(uninitialized.get_max_protocol_version(), 0);
    EXPECT_EQ(uninitialized.get_verify_mode(), -1);
    EXPECT_EQ(uninitialized.get_verify_depth(), -1);
    EXPECT_EQ(uninitialized.get_session_cache_mode(), -1);
    EXPECT_EQ(uninitialized.get_session_cache_size(), -1);
    EXPECT_EQ(uninitialized.set_options(SSL_OP_NO_COMPRESSION), 0ull);
    EXPECT_EQ(uninitialized.clear_options(SSL_OP_NO_COMPRESSION), 0ull);
    EXPECT_EQ(uninitialized.set_session_timeout(10), 0);
    EXPECT_FALSE(uninitialized.set_info_callback(info_callback));
    EXPECT_FALSE(uninitialized.set_msg_callback(msg_callback, nullptr));
}

TEST(SSLListenerConfig, LiveContextExposesAndRoundTripsConfiguration) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    const auto cert = ssl_resource_path("cert.pem");
    const auto key  = ssl_resource_path("key.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(qb::io::ssl::create_server_context(TLS_server_method(), cert, key));
    ASSERT_NE(listener.ssl_handle(), nullptr);

    EXPECT_TRUE(listener.load_ca_certificates_for_client_auth(cert));
    EXPECT_FALSE(listener.load_ca_directory_for_client_auth(""));
    EXPECT_TRUE(listener.set_cipher_list("HIGH:!aNULL"));
    EXPECT_TRUE(listener.set_ciphersuites_tls13("TLS_AES_128_GCM_SHA256"));
    EXPECT_TRUE(listener.set_tls_protocol_versions(TLS1_2_VERSION, TLS1_3_VERSION));
    EXPECT_EQ(listener.get_min_protocol_version(), TLS1_2_VERSION);
    EXPECT_EQ(listener.get_max_protocol_version(), TLS1_3_VERSION);
    EXPECT_TRUE(listener.configure_mtls("", SSL_VERIFY_PEER));
    EXPECT_NE(listener.get_verify_mode(), -1);
    EXPECT_TRUE(listener.set_alpn_selection_callback(select_first_alpn, nullptr));
    EXPECT_TRUE(listener.enable_session_caching(64));
    EXPECT_GT(listener.get_session_cache_size(), 0);
    EXPECT_TRUE(listener.set_custom_client_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(listener.set_ocsp_stapling_responder_callback(ocsp_callback, nullptr));
    EXPECT_TRUE(listener.set_sni_selection_callback(sni_callback, nullptr));
    EXPECT_TRUE(listener.set_keylog_callback(keylog_callback));
    EXPECT_TRUE(listener.configure_ecdh_curves("prime256v1"));
    EXPECT_TRUE(listener.enable_post_handshake_auth());
    EXPECT_TRUE(listener.set_supported_alpn_protocols({"h2", "http/1.1"}));
    EXPECT_FALSE(listener.set_supported_alpn_protocols({}));
    EXPECT_FALSE(listener.set_supported_alpn_protocols({std::string(256, 'x')}));
    EXPECT_NE(listener.set_options(SSL_OP_NO_COMPRESSION), 0ull);
    EXPECT_NE(listener.clear_options(SSL_OP_NO_COMPRESSION), 0ull);
    EXPECT_GE(listener.set_session_timeout(5), 0);
    EXPECT_TRUE(listener.set_info_callback(info_callback));
    EXPECT_TRUE(listener.set_msg_callback(msg_callback, nullptr));
}

// ===========================================================================
// ssl::socket pre-handshake object state (no socket, no handshake)
// ===========================================================================

TEST(SSLSocketConfig, DefaultStateAndPreHandshakeConfiguration) {
    qb::io::tcp::ssl::socket socket;

    EXPECT_TRUE(socket.verify_peer()) << "secure-by-default: a fresh ssl::socket must verify the peer";
    EXPECT_EQ(socket.ssl_handle(), nullptr);
    EXPECT_FALSE(socket.handshake_complete());
    EXPECT_EQ(socket.handshake_status(), -1);
    EXPECT_EQ(socket.read(nullptr, 0), -1);
    EXPECT_EQ(socket.write("", 0), -1);
    EXPECT_EQ(socket.get_last_ssl_error_string(), "No SSL handle");
    EXPECT_TRUE(socket.get_peer_certificate_details().subject.empty());
    EXPECT_TRUE(socket.get_negotiated_cipher_suite().empty());
    EXPECT_TRUE(socket.get_negotiated_tls_version().empty());
    EXPECT_TRUE(socket.get_alpn_selected_protocol().empty());
    EXPECT_TRUE(socket.get_peer_certificate_chain().empty());
    EXPECT_FALSE(socket.get_session().is_valid());
    // Per-connection client toggles with NO ssl::Context equivalent DEFER on a handle-less socket (applied
    // when the SSL is minted at connect) instead of failing — so they work on a socket built from an
    // ssl::Context before it connects, exactly like sni()/alpn()/resume().
    EXPECT_TRUE(socket.disable_session_resumption());
    EXPECT_TRUE(socket.request_ocsp_stapling());
    // Verification IS configured on the ssl::Context (verify()/on_verify()); these raw socket-level
    // overrides still require an existing SSL handle (the init(SSL*) tier), so they fail closed here.
    EXPECT_FALSE(socket.set_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_FALSE(socket.set_verify_depth(2));
    qb::io::ssl::Session invalid_session;
    EXPECT_FALSE(socket.set_session(invalid_session));
    EXPECT_FALSE(socket.request_client_post_handshake_auth());
    EXPECT_FALSE(socket.set_sni_hostname(""));
    EXPECT_TRUE(socket.set_sni_hostname("localhost"));
    EXPECT_FALSE(socket.set_alpn_protocols({}));
    EXPECT_TRUE(socket.set_alpn_protocols({"h2", "http/1.1"}));

    auto *ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(ctx, nullptr);
    socket.init(SSL_new(ctx));
    // create_client_context() contract: the CALLER owns the SSL_CTX. Drop our reference now — the
    // socket's SSL keeps its own (SSL_new up-ref'd the context), so it lives until the socket frees
    // that SSL. (Under the pre-refcount-fix code the socket ALSO freed the context on teardown, so a
    // caller honouring this contract double-freed it: this line is a negative-proof of finding #4.)
    SSL_CTX_free(ctx);
    EXPECT_NE(socket.ssl_handle(), nullptr);
    EXPECT_TRUE(socket.disable_session_resumption());
    EXPECT_TRUE(socket.request_ocsp_stapling());
    EXPECT_TRUE(socket.set_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(socket.set_verify_depth(4));
    EXPECT_TRUE(socket.set_sni_hostname("localhost"));
    EXPECT_TRUE(socket.set_alpn_protocols({"h2"}));
    EXPECT_FALSE(socket.set_alpn_protocols({std::string(256, 'x')}));
    socket.set_insecure();
    EXPECT_FALSE(socket.verify_peer());
}

TEST(SSLSocketConfig, ReinitializesHandleAndFailsPreConnectionOperationsCleanly) {
    qb::io::tcp::ssl::socket socket;

    auto *first_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(first_ctx, nullptr);
    auto *first_ssl = SSL_new(first_ctx);
    ASSERT_NE(first_ssl, nullptr);
    SSL_CTX_free(first_ctx); // caller owns the context; the SSL holds its own reference (see above)
    socket.init(first_ssl);
    ASSERT_NE(socket.ssl_handle(), nullptr);

    ERR_clear_error();
    EXPECT_EQ(socket.get_last_ssl_error_string(), "No SSL error in queue");
    EXPECT_TRUE(socket.request_ocsp_stapling(false));
    EXPECT_TRUE(socket.disable_session_resumption());
    EXPECT_TRUE(socket.set_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(socket.set_verify_depth(3));
    EXPECT_EQ(socket.connected(), qb::io::SocketStatus::Error);
    EXPECT_EQ(socket.handshake_status(), -1);

    auto *second_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(second_ctx, nullptr);
    auto *second_ssl = SSL_new(second_ctx);
    ASSERT_NE(second_ssl, nullptr);
    SSL_CTX_free(second_ctx); // caller owns the context; the SSL holds its own reference
    socket.init(second_ssl);
    EXPECT_NE(socket.ssl_handle(), nullptr);
    EXPECT_FALSE(socket.handshake_complete());

    qb::io::ssl::Session invalid_session;
    EXPECT_FALSE(socket.set_session(invalid_session));
    socket.set_insecure();
    EXPECT_FALSE(socket.verify_peer());
}

// Regression (finding #4 — SSL_CTX reference counting): two ssl::sockets sharing ONE client SSL_CTX
// must each drop only their OWN reference on teardown, never SSL_CTX_free the shared context. The
// pre-fix `!SSL_is_server` heuristic freed the context for every client-mode SSL, so the first
// socket to die tore the shared context out from under the second (double-free / UAF). With the
// refcount fix the context survives until its last referencing SSL is freed AND the caller drops
// its own reference — proven here by still using the context after both sockets are destroyed.
TEST(SSLSocketConfig, SharedClientContextSurvivesUntilLastReferenceDropped) {
    auto *shared = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(shared, nullptr);
    {
        qb::io::tcp::ssl::socket s1;
        qb::io::tcp::ssl::socket s2;
        s1.init(SSL_new(shared)); // shared refs: creator + s1's SSL
        s2.init(SSL_new(shared)); // shared refs: + s2's SSL
        ASSERT_NE(s1.ssl_handle(), nullptr);
        ASSERT_NE(s2.ssl_handle(), nullptr);
    } // both sockets destroyed: each SSL_free drops ONLY its own ref. Pre-fix, the first also
      // SSL_CTX_free'd `shared`, making the second socket's teardown + the use below a UAF.
    SSL *probe = SSL_new(shared); // the caller's reference must still be alive
    EXPECT_NE(probe, nullptr) << "shared client SSL_CTX was freed early (double-free regression)";
    if (probe)
        SSL_free(probe);
    SSL_CTX_free(shared); // the caller's own reference (create_client_context contract)
}

// ===========================================================================
// set_options / clear_options carry the FULL 64-bit SSL_OP_* mask.
//
// OpenSSL 3.x types the option mask `uint64_t` and defines flags above bit 31:
// SSL_OP_NO_TX_CERTIFICATE_COMPRESSION (32), SSL_OP_NO_RX_CERTIFICATE_COMPRESSION (33),
// SSL_OP_ENABLE_KTLS_TX_ZEROCOPY_SENDFILE (34), SSL_OP_PREFER_NO_DHE_KEX (35),
// SSL_OP_LEGACY_EC_POINT_FORMATS (36). The wrapper used to take and return `long`, which is
// 64-bit on LP64 (Linux/macOS) but 32-BIT ON WINDOWS (LLP64).
//
// DISCRIMINATION: these cases FAIL only on an LLP64 target (Windows/MSVC). On LP64 `long` is
// already 64-bit, so they pass with or without the fix — they are a portability guard, not
// coverage, for macOS and Linux. That is not a defect in the test: the bug itself is unobservable
// on LP64. The static_assert below is what keeps the guard honest on every platform.
// ===========================================================================

// The contract under test, checkable everywhere: the parameter/return type must be wide enough for
// the flags OpenSSL actually defines. This fires at compile time on any target where it is not.
static_assert(sizeof(decltype(std::declval<qb::io::tcp::ssl::listener &>().set_options(0))) >= sizeof(std::uint64_t),
              "the SSL option mask must be 64-bit wide: SSL_OP_* flags reach bit 36 and a 32-bit "
              "long (Windows/LLP64) truncates them to 0");

// SSL_OP_NO_RX_CERTIFICATE_COMPRESSION arrived in OpenSSL 3.2; on 3.0/3.1 there is no >32-bit flag
// to drive this with, so the runtime half self-skips and the static_assert above carries the
// contract alone.
#if defined(SSL_OP_NO_RX_CERTIFICATE_COMPRESSION)
TEST(SSLListenerConfig, SetOptionsCarriesFlagsAboveBit31) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(qb::io::ssl::create_server_context(TLS_server_method(), ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
    ASSERT_NE(listener.ssl_handle(), nullptr);

    // Bit 33. Through a 32-bit long this truncates to 0, SSL_CTX_set_options() is handed 0, and the
    // option the caller asked for is silently never applied.
    const std::uint64_t high_bit_flag = SSL_OP_NO_RX_CERTIFICATE_COMPRESSION;
    ASSERT_GT(high_bit_flag, 0xFFFFFFFFull) << "this case is only meaningful for a flag above bit 31";

    listener.set_options(high_bit_flag);
    const std::uint64_t after_set = SSL_CTX_get_options(listener.ssl_handle());
    EXPECT_EQ(after_set & high_bit_flag, high_bit_flag) << "a >32-bit SSL_OP_* flag was dropped on the way to SSL_CTX_set_options()";

    listener.clear_options(high_bit_flag);
    EXPECT_EQ(SSL_CTX_get_options(listener.ssl_handle()) & high_bit_flag, 0ull)
        << "a >32-bit SSL_OP_* flag was dropped on the way to SSL_CTX_clear_options()";
}
#endif // SSL_OP_NO_RX_CERTIFICATE_COMPRESSION

TEST(SSLListenerConfig, SetOptionsBit31DoesNotSignExtendIntoTheHighFlags) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(qb::io::ssl::create_server_context(TLS_server_method(), ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
    ASSERT_NE(listener.ssl_handle(), nullptr);

    // Bit 31 is the nastier half of the same bug: through a 32-bit SIGNED long it becomes
    // 0x80000000 -> -2147483648, which sign-extends back to uint64_t as 0xFFFFFFFF80000000 and
    // sets EVERY option from bit 31 to bit 63 at once — including the certificate-compression and
    // DHE-kex flags the caller never mentioned.
    const std::uint64_t bit31 = SSL_OP_CRYPTOPRO_TLSEXT_BUG;
    ASSERT_EQ(bit31, 0x80000000ull);

    const std::uint64_t before = SSL_CTX_get_options(listener.ssl_handle());
    listener.set_options(bit31);
    const std::uint64_t after = SSL_CTX_get_options(listener.ssl_handle());

    EXPECT_EQ(after & bit31, bit31) << "the requested bit-31 flag was not applied";
    // Nothing above bit 31 that was not already set may have appeared.
    EXPECT_EQ((after & ~before) >> 32, 0ull) << "setting bit 31 smeared into the high SSL_OP_* flags (sign extension through a 32-bit long)";
}
