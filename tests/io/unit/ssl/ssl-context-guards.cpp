/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/ssl/ssl-context-guards.cpp
 * @brief Failure-state and bad-input guards of the value-semantic `qb::io::ssl::Context`.
 *
 * Pure unit tier — no socket, no handshake. Complements ssl-context-value.cpp (happy path) by
 * driving the *negative* configuration surface that the loopback/handshake tests never reach:
 *   - a Context that failed to build (bad cert/key) must treat EVERY subsequent fluent setter as a
 *     no-op (each setter early-returns on `!usable()`), leaving `ok()`/`error()` unchanged — proving
 *     the "never silently degrade to an insecure context" contract in context.cpp;
 *   - the documented empty-path / bad-string failures (`trust("")`, bogus cipher/curve/suite lists)
 *     transition a good context to `!ok()` with a meaningful `error()`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include <string>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/context.h>

#include "../../shared/ssl_fixtures.h"

using qb::io::ssl::Context;
using qb::io::ssl::VerifyContext;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

// A Context that failed to build (nonexistent cert+key) is !ok(); every fluent setter must be a
// no-op that leaves ok()/error() untouched (the `!usable()` early-return in each setter body).
TEST(SslContextGuards, FailedContextSettersAreNoOps) {
    Context broken = Context::server("does-not-exist-cert.pem", "does-not-exist-key.pem");
    ASSERT_FALSE(broken.ok()) << "a Context built from a missing cert/key must be falsy";
    const std::string original_error = broken.error();
    EXPECT_FALSE(original_error.empty());

    // Chain every setter; each must early-return on the failed context (no crash, no state change).
    broken.trust("some/ca")
        .identity("cert.pem", "key.pem")
        .dh_params("dh.pem")
        .ciphers("ECDHE-RSA-AES128-GCM-SHA256")
        .ciphersuites("TLS_AES_128_GCM_SHA256")
        .curves("X25519:P-256")
        .session_cache(64)
        .alpn({"h2", "http/1.1"})
        .on_keylog([](std::string_view) {})
        .on_verify([](bool preverified, VerifyContext &) { return preverified; })
        .on_sni([](std::string_view) { return Context::client(); });

    EXPECT_FALSE(broken.ok()) << "setters must not resurrect a failed context";
    EXPECT_EQ(broken.error(), original_error) << "a no-op setter must not alter the recorded error";
}

// trust("") is the documented empty-path failure.
TEST(SslContextGuards, ClientTrustEmptyPathFails) {
    Context ctx = Context::client();
    ASSERT_TRUE(ctx.ok()) << "a default client Context must be usable";

    ctx.trust("");
    EXPECT_FALSE(ctx.ok());
    EXPECT_NE(ctx.error().find("empty path"), std::string::npos) << "error must name the empty-path cause";
}

// Bogus TLS<=1.2 cipher list, TLS1.3 ciphersuites, and ECDH group list each fail a good context.
TEST(SslContextGuards, GoodClientRejectsBogusCryptoStrings) {
    {
        Context ctx = Context::client();
        ASSERT_TRUE(ctx.ok());
        ctx.ciphers("not-a-real-cipher");
        EXPECT_FALSE(ctx.ok()) << "an unmatched cipher list must fail the context";
    }
    {
        Context ctx = Context::client();
        ASSERT_TRUE(ctx.ok());
        ctx.ciphersuites("TLS_NOT_A_SUITE");
        EXPECT_FALSE(ctx.ok()) << "a bogus TLS1.3 ciphersuite must fail the context";
    }
    {
        Context ctx = Context::client();
        ASSERT_TRUE(ctx.ok());
        ctx.curves("not-a-curve");
        EXPECT_FALSE(ctx.ok()) << "an unknown ECDH group must fail the context";
    }
}

// server() with a valid cert but a nonexistent key file fails to load the identity.
TEST(SslContextGuards, ServerWithMissingKeyFails) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");
    Context ctx = Context::server(ssl_resource_path("cert.pem"), "does-not-exist-key.pem");
    EXPECT_FALSE(ctx.ok()) << "a missing private key must yield a falsy server Context";
    EXPECT_FALSE(ctx.error().empty());
}
