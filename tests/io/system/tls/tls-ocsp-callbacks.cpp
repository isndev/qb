/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tls/tls-ocsp-callbacks.cpp
 * @brief OCSP through `qb::io::ssl::Context`'s typed callbacks, and every typed callback failing closed (Huly QB-83).
 *
 * The contract, one assertion per clause:
 *   - a server context's `on_ocsp_staple` is asked for a response with the client's SNI name, and the response
 *     reaches a client context's `on_ocsp_response`, which can check it for real through the view -- parse,
 *     signature by the issuing CA, status GOOD for the leaf; a client without the check asks for nothing and the
 *     stapler is not called;
 *   - the client check decides: `false` fails the handshake; a server that staples nothing hands the check an
 *     empty response, which a must-staple client rejects and a tolerant one accepts;
 *   - a typed callback that throws is contained at the OpenSSL boundary and fails closed: a verify or an OCSP
 *     check that throws rejects the handshake, an SNI router that throws aborts it, a stapler that throws
 *     staples nothing, a keylog sink that throws loses its line;
 *   - a typed callback and its raw counterpart share one OpenSSL slot: whichever was set last is called.
 *
 * The response is a real one: a generated CA issues the server's leaf and signs a GOOD OCSP response for it.
 * OpenSSL 3.6 parses a staple on both ends -- the server staples only a response that names its certificate,
 * and never for a self-signed one; the client fails a staple that does not parse before any callback runs --
 * so a stand-in byte string proves nothing there, and a real response proves the same on every version.
 * Single-threaded, on the shared `tls_pump.h` harness.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/ocsp.h>

#include <gtest/gtest.h>

#include "../../shared/tls_pump.h"

using qb::io::ssl::Context;
using qb::io::ssl::OcspContext;
using qb::io::test::complete_tls_handshake;
using qb::io::test::generated_identity;
using qb::io::test::open_tls_connection;
using qb::io::test::tls_pair;

namespace tls_ocsp_callbacks_test {

// The issuing CA and the server's leaf, generated once per test.
struct chain {
    generated_identity ca{"qb-ocsp-ca", nullptr, generated_identity::role::authority};
    generated_identity leaf{"localhost", &ca};

    [[nodiscard]] bool
    ok() const noexcept {
        return ca.ok() && leaf.ok();
    }
};

// A GOOD OCSP response for `leaf`, signed by its issuer `ca` (the signer certificate included), DER-encoded.
std::vector<unsigned char>
good_response(const generated_identity &leaf, const generated_identity &ca) {
    std::vector<unsigned char> der;
    OCSP_CERTID               *id   = OCSP_cert_to_id(EVP_sha1(), leaf.x509(), ca.x509());
    OCSP_BASICRESP            *bs   = OCSP_BASICRESP_new();
    ASN1_TIME                 *now  = X509_gmtime_adj(nullptr, 0);
    ASN1_TIME                 *next = X509_gmtime_adj(nullptr, 3600);
    const bool     ok   = id && bs && now && next && OCSP_basic_add1_status(bs, id, V_OCSP_CERTSTATUS_GOOD, 0, nullptr, now, next) != nullptr
                          && OCSP_basic_sign(bs, ca.x509(), ca.pkey(), EVP_sha256(), nullptr, 0) == 1;
    OCSP_RESPONSE *resp = ok ? OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, bs) : nullptr;
    if (resp) {
        unsigned char *p = nullptr;
        const int      n = i2d_OCSP_RESPONSE(resp, &p);
        if (n > 0)
            der.assign(p, p + n);
        OPENSSL_free(p);
    }
    OCSP_RESPONSE_free(resp);
    ASN1_TIME_free(next);
    ASN1_TIME_free(now);
    OCSP_BASICRESP_free(bs);
    OCSP_CERTID_free(id);
    return der;
}

// What a real client check does with the view: the response parses, its signature chains to the trusted CA,
// and it says the certificate the server presented is GOOD.
bool
leaf_is_good(OcspContext &ocsp, const generated_identity &ca) {
    const auto der = ocsp.response();
    if (der.empty())
        return false;
    const unsigned char *p     = der.data();
    OCSP_RESPONSE       *resp  = d2i_OCSP_RESPONSE(nullptr, &p, static_cast<long>(der.size()));
    OCSP_BASICRESP      *bs    = resp ? OCSP_response_get1_basic(resp) : nullptr;
    X509_STORE          *store = X509_STORE_new();
    X509                *leaf  = SSL_get0_peer_certificate(ocsp.native()); // borrowed
    bool                 good  = false;
    if (bs && store && leaf && X509_STORE_add_cert(store, ca.x509()) == 1 && OCSP_basic_verify(bs, nullptr, store, 0) == 1) {
        OCSP_CERTID *id     = OCSP_cert_to_id(EVP_sha1(), leaf, ca.x509());
        int          status = -1;
        good = id && OCSP_resp_find_status(bs, id, &status, nullptr, nullptr, nullptr, nullptr) == 1 && status == V_OCSP_CERTSTATUS_GOOD;
        OCSP_CERTID_free(id);
    }
    X509_STORE_free(store);
    OCSP_BASICRESP_free(bs);
    OCSP_RESPONSE_free(resp);
    return good;
}

// A client socket from `ctx`; the chain is trusted only by the OCSP check, so the client does not verify the
// certificate itself -- unless the test is about verification.
qb::io::tcp::ssl::socket
client_of(Context ctx, bool verify = false) {
    if (!verify)
        ctx.verify(qb::io::ssl::VerifyMode::none);
    return qb::io::tcp::ssl::socket{std::move(ctx)};
}

int g_raw_calls = 0;

int
raw_client_status_cb(SSL *, void *) {
    ++g_raw_calls;
    return 1;
}

} // namespace tls_ocsp_callbacks_test

using namespace tls_ocsp_callbacks_test;

TEST(TlsOcspCallbacks, AStapledResponseReachesTheClientCheckWhichVerifiesIt) {
    const chain c;
    ASSERT_TRUE(c.ok());
    const auto staple = good_response(c.leaf, c.ca);
    ASSERT_FALSE(staple.empty());
    int         staple_calls = 0;
    std::string asked_for;
    auto        server_ctx = c.leaf.context().on_ocsp_staple([&](std::string_view servername) {
        ++staple_calls;
        asked_for = std::string(servername);
        return staple;
    });
    ASSERT_TRUE(server_ctx.ok()) << server_ctx.error();
    qb::io::tcp::ssl::listener listener{server_ctx};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();

    // A client without the check asks for nothing: the stapler is not called.
    {
        tls_pair plain{client_of(Context::client())};
        ASSERT_TRUE(open_tls_connection(listener, port, plain));
        ASSERT_TRUE(complete_tls_handshake(plain));
        EXPECT_EQ(staple_calls, 0);
    }

    // A client with it asks, receives the response, and checks it for real.
    std::vector<unsigned char> seen;
    std::string                seen_name;
    int                        check_calls = 0;
    bool                       verified    = false;
    tls_pair                   checked{client_of(Context::client().on_ocsp_response([&](OcspContext &ocsp) {
        ++check_calls;
        const auto r = ocsp.response();
        seen.assign(r.begin(), r.end());
        seen_name = std::string(ocsp.servername());
        verified  = leaf_is_good(ocsp, c.ca);
        return verified;
    }))};
    ASSERT_TRUE(open_tls_connection(listener, port, checked));
    ASSERT_TRUE(complete_tls_handshake(checked));
    EXPECT_EQ(staple_calls, 1);
    EXPECT_EQ(asked_for, "localhost");
    EXPECT_EQ(check_calls, 1);
    EXPECT_EQ(seen, staple);
    EXPECT_EQ(seen_name, "localhost");
    EXPECT_TRUE(verified) << "the stapled response must verify against the CA and say GOOD";
}

TEST(TlsOcspCallbacks, TheClientCheckDecidesAndAMissingStapleIsItsToJudge) {
    const chain c;
    ASSERT_TRUE(c.ok());
    const auto                 staple = good_response(c.leaf, c.ca);
    qb::io::tcp::ssl::listener stapling{c.leaf.context().on_ocsp_staple([&](std::string_view) { return staple; })};
    ASSERT_EQ(stapling.listen_v4(0, "127.0.0.1"), 0);
    qb::io::tcp::ssl::listener silent{c.leaf.context()}; // staples nothing
    ASSERT_EQ(silent.listen_v4(0, "127.0.0.1"), 0);

    // A check that rejects a staple fails the handshake.
    tls_pair rejected{client_of(Context::client().on_ocsp_response([](OcspContext &) { return false; }))};
    ASSERT_TRUE(open_tls_connection(stapling, stapling.local_endpoint().port(), rejected));
    EXPECT_FALSE(complete_tls_handshake(rejected));
    EXPECT_FALSE(rejected.client.handshake_complete());

    // A must-staple check against a server that staples nothing: called with an empty response, rejects.
    bool     empty_seen = false;
    tls_pair must_staple{client_of(Context::client().on_ocsp_response([&](OcspContext &ocsp) {
        empty_seen = ocsp.response().empty();
        return !ocsp.response().empty();
    }))};
    ASSERT_TRUE(open_tls_connection(silent, silent.local_endpoint().port(), must_staple));
    EXPECT_FALSE(complete_tls_handshake(must_staple));
    EXPECT_TRUE(empty_seen);

    // A check tolerant of a missing staple lets the same handshake complete.
    tls_pair tolerant{client_of(Context::client().on_ocsp_response([](OcspContext &) { return true; }))};
    ASSERT_TRUE(open_tls_connection(silent, silent.local_endpoint().port(), tolerant));
    EXPECT_TRUE(complete_tls_handshake(tolerant));
}

TEST(TlsOcspCallbacks, ATypedCallbackThatThrowsFailsClosed) {
    const chain c;
    ASSERT_TRUE(c.ok());

    // A stapler that throws staples nothing; the handshake goes on, and the client check sees no staple.
    qb::io::tcp::ssl::listener throwing_stapler{c.leaf.context().on_ocsp_staple(
        [](std::string_view) -> std::vector<unsigned char> { throw std::runtime_error("stapler failure"); })};
    ASSERT_EQ(throwing_stapler.listen_v4(0, "127.0.0.1"), 0);
    bool     staple_absent = false;
    tls_pair after_stapler{client_of(Context::client().on_ocsp_response([&](OcspContext &ocsp) {
        staple_absent = ocsp.response().empty();
        return true;
    }))};
    ASSERT_TRUE(open_tls_connection(throwing_stapler, throwing_stapler.local_endpoint().port(), after_stapler));
    EXPECT_TRUE(complete_tls_handshake(after_stapler));
    EXPECT_TRUE(staple_absent);

    qb::io::tcp::ssl::listener plain{c.leaf.context()};
    ASSERT_EQ(plain.listen_v4(0, "127.0.0.1"), 0);
    const auto port = plain.local_endpoint().port();

    // An OCSP check that throws rejects.
    tls_pair check{client_of(Context::client().on_ocsp_response([](OcspContext &) -> bool { throw std::runtime_error("check failure"); }))};
    ASSERT_TRUE(open_tls_connection(plain, port, check));
    EXPECT_FALSE(complete_tls_handshake(check));

    // A verify callback that throws rejects, where one that returns true accepts: the callback decides.
    tls_pair verify{client_of(Context::client().trust(c.ca.cert()).on_verify([](bool, qb::io::ssl::VerifyContext &) -> bool {
        throw std::runtime_error("verify failure");
    }),
                              /*verify=*/true)};
    ASSERT_TRUE(open_tls_connection(plain, port, verify));
    EXPECT_FALSE(complete_tls_handshake(verify));
    tls_pair verify_ok{client_of(Context::client().trust(c.ca.cert()).on_verify([](bool, qb::io::ssl::VerifyContext &) { return true; }),
                                 /*verify=*/true)};
    ASSERT_TRUE(open_tls_connection(plain, port, verify_ok));
    EXPECT_TRUE(complete_tls_handshake(verify_ok));

    // A keylog sink that throws loses its line; the handshake completes.
    tls_pair keylog{client_of(Context::client().on_keylog([](std::string_view) { throw std::runtime_error("sink failure"); }))};
    ASSERT_TRUE(open_tls_connection(plain, port, keylog));
    EXPECT_TRUE(complete_tls_handshake(keylog));

    // An SNI router that throws aborts the handshake on the server.
    qb::io::tcp::ssl::listener routing{c.leaf.context().on_sni(
        [](std::string_view) -> Context { throw std::runtime_error("router failure"); })};
    ASSERT_EQ(routing.listen_v4(0, "127.0.0.1"), 0);
    tls_pair routed{client_of(Context::client())};
    ASSERT_TRUE(open_tls_connection(routing, routing.local_endpoint().port(), routed));
    EXPECT_FALSE(complete_tls_handshake(routed));
    EXPECT_FALSE(routed.server.handshake_complete());
}

TEST(TlsOcspCallbacks, ATypedCallbackAndItsRawCounterpartShareOneSlot) {
    const chain c;
    ASSERT_TRUE(c.ok());
    const auto                 staple = good_response(c.leaf, c.ca);
    qb::io::tcp::ssl::listener listener{c.leaf.context().on_ocsp_staple([&](std::string_view) { return staple; })};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();

    int  typed_calls = 0;
    auto ctx         = Context::client().on_ocsp_response([&](OcspContext &) {
        ++typed_calls;
        return true;
    });
    // The raw setter, after: it replaces the typed check (one status callback per SSL_CTX).
    g_raw_calls = 0;
    ASSERT_TRUE(qb::io::ssl::set_ocsp_stapling_client_callback(ctx.native(), &raw_client_status_cb, nullptr));
    {
        tls_pair raw_last{client_of(ctx)};
        ASSERT_TRUE(open_tls_connection(listener, port, raw_last));
        ASSERT_TRUE(complete_tls_handshake(raw_last));
    }
    EXPECT_EQ(g_raw_calls, 1);
    EXPECT_EQ(typed_calls, 0);

    // The typed setter, after: it takes the slot back.
    ctx.on_ocsp_response([&](OcspContext &) {
        ++typed_calls;
        return true;
    });
    {
        tls_pair typed_last{client_of(ctx)};
        ASSERT_TRUE(open_tls_connection(listener, port, typed_last));
        ASSERT_TRUE(complete_tls_handshake(typed_last));
    }
    EXPECT_EQ(g_raw_calls, 1);
    EXPECT_EQ(typed_calls, 1);
}
