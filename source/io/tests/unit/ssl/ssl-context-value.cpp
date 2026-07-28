/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/ssl/ssl-context-value.cpp
 * @brief Unit tests for the value-semantic `qb::io::ssl::Context` TLS context.
 *
 * Pure in-process object lifetime — no socket, no handshake. These assert the OWNERSHIP contract
 * that makes the whole abstraction safe: copy == share (one SSL_CTX, freed exactly once),
 * `adopt` == transfer (takes the caller's reference), `share` == up-ref (caller keeps theirs),
 * fail-closed construction, and that the ex-data typed-callback state is freed with the context.
 *
 * The teeth are under AddressSanitizer + LeakSanitizer: a missed free surfaces as an LSan leak, an
 * over-free as an ASan double-free/UAF. `ShareKeepsCallerReferenceAlive` and the shared-context
 * regression are direct negative-proofs of the reference-counting (revert the up-ref / share model
 * and they abort). Run under the `sanitize` preset.
 *
 * @ingroup Tests
 */

#include <chrono>
#include <vector>

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/context.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/ssl_fixtures.h"

using qb::io::ssl::Context;
using qb::io::ssl::TlsVersion;
using qb::io::ssl::VerifyContext;
using qb::io::ssl::VerifyMode;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

// --- construction / secure-by-default -------------------------------------------------------------

TEST(SslContextValue, ClientIsUsableAndSecureByDefault) {
    auto ctx = Context::client();
    ASSERT_TRUE(ctx.ok());
    ASSERT_TRUE(static_cast<bool>(ctx));
    ASSERT_NE(ctx.native(), nullptr);
    EXPECT_TRUE(ctx.error().empty());
    // Secure by default: TLS 1.2 floor + peer verification on the context.
    EXPECT_EQ(SSL_CTX_get_min_proto_version(ctx.native()), TLS1_2_VERSION);
    EXPECT_EQ(SSL_CTX_get_verify_mode(ctx.native()), SSL_VERIFY_PEER);
}

TEST(SslContextValue, ServerFailsClosedOnMissingCert) {
    auto ctx = Context::server("qb-nonexistent-cert.pem", "qb-nonexistent-key.pem");
    EXPECT_FALSE(ctx.ok());
    EXPECT_FALSE(static_cast<bool>(ctx));
    EXPECT_FALSE(ctx.error().empty()) << "a fail-closed context must explain why";
}

TEST(SslContextValue, ServerLoadsRealIdentity) {
    ASSERT_TRUE(require_ssl_files());
    auto ctx = Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"));
    ASSERT_TRUE(ctx.ok()) << ctx.error();
    EXPECT_NE(ctx.native(), nullptr);
    EXPECT_EQ(SSL_CTX_get_min_proto_version(ctx.native()), TLS1_2_VERSION);
}

TEST(SslContextValue, DefaultAndNullFactoriesAreFalsy) {
    EXPECT_FALSE(Context().ok());
    EXPECT_EQ(Context().native(), nullptr);
    EXPECT_FALSE(Context::adopt(nullptr).ok());
    EXPECT_FALSE(Context::share(nullptr).ok());
}

// --- fail-closed error propagation ----------------------------------------------------------------

TEST(SslContextValue, FailsClosedOnBadSetter) {
    // A missing client-identity cert reliably fails SSL_CTX_use_certificate_file.
    auto ctx = Context::client().identity("qb-nonexistent-cert.pem", "qb-nonexistent-key.pem");
    EXPECT_FALSE(ctx.ok());
    EXPECT_FALSE(ctx.error().empty());
}

TEST(SslContextValue, ErrorShortCircuitsRestOfChain) {
    auto ctx = Context::client()
                   .identity("qb-nonexistent-cert.pem", "qb-nonexistent-key.pem") // fails here
                   .min_version(TlsVersion::v1_3)                                  // no-op after error
                   .alpn({"h2"});                                                  // no-op after error
    EXPECT_FALSE(ctx.ok());
}

// --- ownership: copy == share ---------------------------------------------------------------------

TEST(SslContextValue, CopyIsShareNoDoubleFree) {
    auto base = Context::client();
    ASSERT_TRUE(base.ok());

    std::vector<Context> copies(8, base); // eight shares of ONE SSL_CTX
    for (auto &c : copies)
        EXPECT_EQ(c.native(), base.native());

    // Each "connection" mints its own SSL (OpenSSL up-refs the ctx independently).
    std::vector<SSL *> ssls;
    for (auto &c : copies)
        ssls.push_back(SSL_new(c.native()));
    for (auto *s : ssls)
        ASSERT_NE(s, nullptr);
    for (auto *s : ssls)
        SSL_free(s);

    copies.clear(); // drop eight shares — the ctx must NOT be freed yet (base still holds one)
    ASSERT_TRUE(base.ok());
    SSL *s = SSL_new(base.native());
    ASSERT_NE(s, nullptr) << "shared context was freed too early";
    SSL_free(s);
    // `base` destructs at scope end -> the single, final SSL_CTX_free. LSan: no leak. ASan: no double free.
}

// --- ownership: adopt == transfer -----------------------------------------------------------------

TEST(SslContextValue, AdoptTransfersOwnership) {
    SSL_CTX *raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(raw, nullptr);
    {
        auto ctx = Context::adopt(raw); // takes over the caller's single reference (no up-ref)
        ASSERT_TRUE(ctx.ok());
        EXPECT_EQ(ctx.native(), raw);
    } // ctx dies -> exactly one SSL_CTX_free(raw). Do NOT touch raw again.
    // If adopt() wrongly up-ref'd, raw would leak here and LSan would flag it.
    SUCCEED();
}

// --- ownership: share == up-ref (negative-proof of the reference count) ---------------------------

TEST(SslContextValue, ShareKeepsCallerReferenceAlive) {
    SSL_CTX *raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(raw, nullptr);
    {
        auto ctx = Context::share(raw); // takes a NEW reference; caller keeps theirs
        ASSERT_TRUE(ctx.ok());
        EXPECT_EQ(ctx.native(), raw);
    } // ctx dies -> drops only ITS reference; raw must still be alive.
    // Negative-proof: if share() failed to up-ref, this SSL_new would be a use-after-free (ASan abort).
    SSL *s = SSL_new(raw);
    EXPECT_NE(s, nullptr) << "share() must up-ref: the caller's SSL_CTX was freed early";
    if (s)
        SSL_free(s);
    SSL_CTX_free(raw); // caller frees their own reference -> ctx destroyed now. No leak.
}

TEST(SslContextValue, ShareOfContextNativePreservesStateAndUsability) {
    // Regression: share()/adopt() of a Context's own native() must NOT clobber the shared ex-data state's
    // role (both Contexts point at the same SSL_CTX + state). Exercises the fixed ctx_own guard; ASan/LSan
    // prove no state corruption / double-free across the shared teardown.
    ASSERT_TRUE(require_ssl_files());
    auto server = Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).alpn({"h2"});
    ASSERT_TRUE(server.ok()) << server.error();
    {
        auto shared = Context::share(server.native()); // same ctx + same ex-data state
        EXPECT_TRUE(shared.ok());
        EXPECT_EQ(shared.native(), server.native());
        shared.alpn({"http/1.1"}); // must remain a valid server-role op (no failure)
        EXPECT_TRUE(shared.ok()) << shared.error();
    }
    EXPECT_TRUE(server.ok());
    SSL *s = SSL_new(server.native());
    ASSERT_NE(s, nullptr) << "the shared server ctx must survive the share's teardown";
    SSL_free(s);
}

TEST(SslContextValue, ShareCanBeSharedAgain) {
    SSL_CTX *raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(raw, nullptr);
    {
        auto a = Context::share(raw);
        auto b = Context::share(raw); // a second independent up-ref
        auto c = a;                   // copy of a share (shared_ptr copy, no extra OpenSSL ref)
        ASSERT_TRUE(a.ok() && b.ok() && c.ok());
        EXPECT_EQ(a.native(), raw);
        EXPECT_EQ(b.native(), raw);
    } // a, b, c die -> their references drop; raw still alive via the caller's original.
    SSL *s = SSL_new(raw);
    EXPECT_NE(s, nullptr);
    if (s)
        SSL_free(s);
    SSL_CTX_free(raw);
}

// --- typed callbacks live/die with the context (ex-data) ------------------------------------------

TEST(SslContextValue, TypedCallbacksAttachAndFreeCleanly) {
    auto ctx = Context::client()
                   .on_keylog([](std::string_view) {})
                   .on_verify([](bool preverified, VerifyContext &) { return preverified; });
    ASSERT_TRUE(ctx.ok()) << ctx.error();
    // A copy shares the same ctx + the same ex-data callback state.
    auto copy = ctx;
    EXPECT_EQ(copy.native(), ctx.native());
    // Both destruct; the ex-data ctx_state is deleted exactly once when the ctx is freed (LSan clean).
}

TEST(SslContextValue, VerifyStaysInstalledAcrossModeChange) {
    // on_verify installs the trampoline; a later verify() must not drop it.
    auto ctx = Context::client()
                   .on_verify([](bool ok, VerifyContext &) { return ok; })
                   .verify(VerifyMode::peer_require);
    ASSERT_TRUE(ctx.ok());
    EXPECT_EQ(SSL_CTX_get_verify_mode(ctx.native()), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
    EXPECT_NE(SSL_CTX_get_verify_callback(ctx.native()), nullptr) << "verify() must preserve the on_verify trampoline";
}

// --- ALPN client vs server ------------------------------------------------------------------------

TEST(SslContextValue, AlpnClientAndServer) {
    ASSERT_TRUE(require_ssl_files());
    auto client = Context::client().alpn({"h2", "http/1.1"});
    EXPECT_TRUE(client.ok()) << client.error();

    // Server ALPN installs a select callback driven by the ex-data wire buffer (no void* arg); the
    // negotiation itself is exercised at handshake in the system tests. Here we only assert the
    // context stays healthy after configuring server-side ALPN.
    auto server = Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).alpn({"h2", "http/1.1"});
    EXPECT_TRUE(server.ok()) << server.error();
}

TEST(SslContextValue, AlpnEmptyIsNoOpNotError) {
    // An empty (or all-oversized) ALPN list leaves the context unconfigured, NOT failed — a server that
    // forwards an optional/empty ALPN list (e.g. the async acceptor) must still start.
    EXPECT_TRUE(Context::client().alpn({}).ok());
    EXPECT_TRUE(Context::client().alpn({std::string(300, 'x')}).ok()); // all > 255 bytes -> skipped -> no-op
    EXPECT_TRUE(Context::client().alpn({"h2"}).ok());
}

// --- config knobs actually take effect on the SSL_CTX --------------------------------------------

TEST(SslContextValue, ConfigKnobsTakeEffect) {
    auto c = Context::client()
                 .max_version(TlsVersion::v1_3)
                 .ciphersuites("TLS_AES_256_GCM_SHA384")
                 .curves("X25519:P-256")
                 .session_timeout(std::chrono::seconds(120));
    ASSERT_TRUE(c.ok()) << c.error();
    EXPECT_EQ(SSL_CTX_get_max_proto_version(c.native()), TLS1_3_VERSION);
    EXPECT_EQ(SSL_CTX_get_timeout(c.native()), 120);

    // A TLS<=1.2 cipher list applies cleanly on a client context.
    EXPECT_TRUE(Context::client().ciphers("ECDHE-RSA-AES128-GCM-SHA256").ok());

    // Server session cache switches the cache mode to SERVER.
    ASSERT_TRUE(require_ssl_files());
    auto s = Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).session_cache(256);
    ASSERT_TRUE(s.ok()) << s.error();
    EXPECT_EQ(SSL_CTX_get_session_cache_mode(s.native()), SSL_SESS_CACHE_SERVER);
}

TEST(SslContextValue, TrustFailsClosedOnBadPathLoadsRealCa) {
    ASSERT_TRUE(require_ssl_files());
    EXPECT_FALSE(Context::client().trust("qb-nonexistent-ca.pem").ok()) << "a missing CA must fail closed";
    // The shipped self-signed cert is a valid PEM trust anchor.
    EXPECT_TRUE(Context::client().trust(ssl_resource_path("cert.pem")).ok());
}

TEST(SslContextValue, DhParamsFailsClosedOnBadPath) {
    ASSERT_TRUE(require_ssl_files());
    // The success path needs a real DH-params PEM (not shipped); the reachable contract is fail-closed.
    auto ctx = Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).dh_params("qb-nonexistent-dh.pem");
    EXPECT_FALSE(ctx.ok()) << "a missing DH-params file must fail closed";
    EXPECT_FALSE(ctx.error().empty());
}

// --- fail-closed: a broken client Context must NOT silently downgrade to a generic auto-context ----

TEST(SslContextValue, BrokenClientContextFailsClosedNotDowngraded) {
    // A client Context whose config step failed (e.g. a missing pinned CA) has native() != null but
    // ok() == false. A socket built from it must FAIL CLOSED at connect setup — never fall back to a
    // generic auto-created (system-trust, no client-cert) context, which is a silent security downgrade.
    auto broken = Context::client().trust("qb-nonexistent-pinned-ca.pem");
    ASSERT_FALSE(broken.ok()) << "config failure must make the context falsy";
    ASSERT_NE(broken.native(), nullptr) << "the SSL_CTX exists (broken but allocated) — this is the fail-open trap";

    qb::io::tcp::ssl::socket s{std::move(broken)};
    // init_client() drives the same ensure_client_ssl_() path as connect(), without needing a TCP peer.
    EXPECT_NE(s.init_client("example.com"), 0) << "a broken client Context must fail closed";
    EXPECT_EQ(s.ssl_handle(), nullptr) << "no SSL may be minted from a broken Context (no silent downgrade)";

    // Sanity: a HEALTHY client Context mints an SSL via the very same path.
    qb::io::tcp::ssl::socket healthy{Context::client()};
    EXPECT_EQ(healthy.init_client("example.com"), 0);
    EXPECT_NE(healthy.ssl_handle(), nullptr);
}

TEST(SslContextValue, DeferredPreHandshakeTogglesAppliedWhenSslMinted) {
    // disable_session_resumption() / request_ocsp_stapling() have no ssl::Context equivalent, so on a
    // socket built from a Context (SSL minted lazily at connect) they DEFER instead of failing — and the
    // deferred request must actually be APPLIED to the SSL once it is minted, not silently dropped.
    qb::io::tcp::ssl::socket s{Context::client()};
    ASSERT_EQ(s.ssl_handle(), nullptr) << "a Context socket has no SSL until connect setup";
    EXPECT_TRUE(s.disable_session_resumption()) << "deferred, not failed (no handle yet)";
    EXPECT_TRUE(s.request_ocsp_stapling(true)) << "deferred, not failed (no handle yet)";

    // init_client() mints the SSL via ensure_client_ssl_() and runs apply_pending_client_settings().
    ASSERT_EQ(s.init_client("example.com"), 0);
    ASSERT_NE(s.ssl_handle(), nullptr);
    // Pre-fix these returned false above and nothing was deferred, so neither option was set on the mint.
    EXPECT_TRUE((SSL_get_options(s.ssl_handle()) & SSL_OP_NO_TICKET) != 0) << "deferred disable_session_resumption not applied at mint";
    EXPECT_EQ(SSL_get_tlsext_status_type(s.ssl_handle()), TLSEXT_STATUSTYPE_ocsp) << "deferred request_ocsp_stapling not applied at mint";
}

// --- THE structural anti-double-free regression ---------------------------------------------------
// One server context shared across many "connections", minted SSLs and shares torn down in a
// scrambled order. Safe BY CONSTRUCTION: the pre-abstraction hand-rolled ownership is exactly what
// double-freed a shared client context; here it is impossible because there is no user SSL_CTX_free.
TEST(SslContextValue, OneServerContextSharedAcrossManyConnections) {
    ASSERT_TRUE(require_ssl_files());
    auto server = Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).alpn({"h2"});
    ASSERT_TRUE(server.ok()) << server.error();

    std::vector<Context> shares;
    std::vector<SSL *>   ssls;
    for (int i = 0; i < 16; ++i) {
        shares.push_back(server); // each "accept" shares the ONE context
        ssls.push_back(SSL_new(shares.back().native()));
        ASSERT_NE(ssls.back(), nullptr);
    }
    // Scrambled teardown: free even-indexed SSLs, drop the first half of the shares, then the rest.
    for (std::size_t i = 0; i < ssls.size(); i += 2) {
        SSL_free(ssls[i]);
        ssls[i] = nullptr;
    }
    shares.erase(shares.begin(), shares.begin() + 8);
    for (auto *s : ssls)
        if (s)
            SSL_free(s);
    shares.clear();

    // The one server context survived every teardown order and is still usable.
    SSL *s = SSL_new(server.native());
    ASSERT_NE(s, nullptr) << "the shared server context was freed early";
    SSL_free(s);
}

TEST(SslContextValue, FluentConfigKnobsAllApply) {
    // Exercise the fluent config knobs the other cases don't touch, keeping the whole public config surface
    // under test (secure-by-default client base + every remaining knob).
    auto c = Context::client()
                 .max_version(TlsVersion::v1_3)
                 .ciphers("HIGH:!aNULL:!MD5")
                 .ciphersuites("TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256")
                 .curves("X25519:P-256")
                 .session_cache(64)
                 .session_timeout(std::chrono::seconds(600))
                 .on_keylog([](std::string_view) {});
    ASSERT_TRUE(c.ok()) << c.error();
    EXPECT_EQ(SSL_CTX_get_max_proto_version(c.native()), TLS1_3_VERSION);
}

TEST(SslContextValue, BadConfigValueFailsClosedAndPreservesFirstError) {
    // A malformed config value must fail the context CLOSED (records an error, ok() == false) — never
    // silently ignored; this covers the fail() branch of the fluent setters.
    auto c = Context::client().ciphersuites("qb-not-a-real-tls13-ciphersuite");
    EXPECT_FALSE(c.ok());
    EXPECT_FALSE(c.error().empty());
    // Once in the error state, subsequent setters are no-ops that preserve the FIRST error.
    const auto first = c.error();
    c.curves("qb-not-a-real-curve").session_cache(8);
    EXPECT_EQ(c.error(), first);
}

// No in-file main(): links the framework's shared gtest-main.
