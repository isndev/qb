/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tls/tls-context-reload.cpp
 * @brief `tcp::ssl::listener::reload_context`: a certificate renewed without a restart (Huly QB-205).
 *
 * The contract, one assertion per clause:
 *   - the connection accepted AFTER the reload presents the new certificate, through both `accept`
 *     overloads (the one `transport::saccept` calls, and the one returning a socket);
 *   - a connection accepted BEFORE it keeps the certificate it was minted with -- one already established,
 *     which goes on exchanging data, and one whose handshake had not started yet, which completes it
 *     with the previous certificate;
 *   - a replacement that failed to load (the renewal left half-done: a new certificate beside the old
 *     key, whose error names the mismatch; missing files; an empty context) is refused, and the listener
 *     goes on serving the certificate it had;
 *   - the ALPN list `set_supported_alpn_protocols` sets is the CONTEXT's (Huly QB-308): the context a reload
 *     leaves keeps its list for the connections minted from it, and a context outlives the listener that
 *     configured it with the list intact.
 *
 * Hermetic and single-threaded, on the shared `tls_pump.h` harness: two self-signed certificates are
 * generated in memory (no shipped resource names the certificate a connection must show), and every
 * handshake is pumped from the test thread -- client and server non-blocking, alternately -- so
 * "accepted before the reload, handshaken after" is an ORDER the test writes down, not a race it hopes
 * to win.
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

#include <string>

#include <gtest/gtest.h>

#include "../../shared/tls_pump.h"

using qb::io::test::accept_form;
using qb::io::test::complete_tls_handshake;
using qb::io::test::generated_identity;
using qb::io::test::open_tls_connection;
using qb::io::test::tls_pair;
using qb::io::test::tls_round_trip;

namespace {

// The certificates are self-signed: what is under test is WHICH one a connection is shown.
tls_pair
insecure_pair() {
    tls_pair pair;
    pair.client.set_insecure();
    return pair;
}

// A client offering both protocols: which one it gets is the server's list.
tls_pair
alpn_pair() {
    tls_pair pair = insecure_pair();
    pair.client.set_alpn_protocols({"h2", "http/1.1"});
    return pair;
}

std::string
presented_name(const qb::io::tcp::ssl::socket &client) {
    return client.get_peer_certificate_details().subject; // X509_NAME_oneline: "/CN=<cn>"
}

} // namespace

TEST(TlsContextReload, TheNextConnectionGetsTheNewCertificateAndTheOpenOnesKeepTheirs) {
    const generated_identity before{"qb-reload-before"};
    const generated_identity after{"qb-reload-after"};
    ASSERT_TRUE(before.ok() && after.ok()) << "could not generate the two test certificates";

    qb::io::tcp::ssl::listener listener{before.context()};
    ASSERT_TRUE(listener.context().ok()) << listener.context().error();
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    // Established before the reload.
    auto established = insecure_pair();
    ASSERT_TRUE(open_tls_connection(listener, port, established));
    ASSERT_TRUE(complete_tls_handshake(established));
    EXPECT_NE(presented_name(established.client).find("CN=qb-reload-before"), std::string::npos) << presented_name(established.client);

    // Accepted before the reload -- the listener has minted its SSL -- but no TLS byte exchanged yet.
    auto accepted_only = insecure_pair();
    ASSERT_TRUE(open_tls_connection(listener, port, accepted_only));

    SSL_CTX *const previous = listener.ssl_handle();
    const auto     renewed  = after.context();
    ASSERT_TRUE(renewed.ok()) << renewed.error();
    ASSERT_TRUE(listener.reload_context(renewed));
    EXPECT_EQ(listener.ssl_handle(), renewed.native());
    EXPECT_NE(listener.ssl_handle(), previous);

    // The one accepted before completes its handshake with the certificate it was minted with.
    ASSERT_TRUE(complete_tls_handshake(accepted_only));
    EXPECT_NE(presented_name(accepted_only.client).find("CN=qb-reload-before"), std::string::npos) << presented_name(accepted_only.client);

    // A connection accepted after it presents the new certificate, through each accept overload.
    for (const auto form : {accept_form::into_socket, accept_form::returning_socket}) {
        auto fresh = insecure_pair();
        ASSERT_TRUE(open_tls_connection(listener, port, fresh, form));
        ASSERT_TRUE(complete_tls_handshake(fresh));
        EXPECT_NE(presented_name(fresh.client).find("CN=qb-reload-after"), std::string::npos)
            << "accept form " << static_cast<int>(form) << " presented " << presented_name(fresh.client);
        EXPECT_TRUE(tls_round_trip(fresh, "new?", "new!"));
    }

    // The connections opened before the reload are untouched by it.
    EXPECT_TRUE(tls_round_trip(established, "ping", "pong"));
    EXPECT_TRUE(tls_round_trip(accepted_only, "late", "fine"));
}

TEST(TlsContextReload, AContextThatFailedToLoadIsRefusedAndTheCurrentOneServes) {
    const generated_identity current{"qb-reload-current"};
    ASSERT_TRUE(current.ok()) << "could not generate the test certificate";

    qb::io::tcp::ssl::listener listener{current.context()};
    ASSERT_TRUE(listener.context().ok()) << listener.context().error();
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);
    SSL_CTX *const serving = listener.ssl_handle();

    // A renewal left half-done -- the new certificate beside the old key -- whose error names the mismatch
    // (OpenSSL's reason, not only the file); a renewal whose files are not there; no context at all.
    const generated_identity renewed{"qb-reload-renewed"};
    ASSERT_TRUE(renewed.ok()) << "could not generate the renewed certificate";
    const auto half = qb::io::ssl::Context::server(renewed.cert(), current.key());
    ASSERT_FALSE(half.ok());
    EXPECT_NE(half.error().find("mismatch"), std::string::npos) << half.error();
    EXPECT_FALSE(listener.reload_context(half));
    const auto missing = qb::io::ssl::Context::server("qb-reload-no-such-cert.pem", "qb-reload-no-such-key.pem");
    ASSERT_FALSE(missing.ok());
    EXPECT_FALSE(listener.reload_context(missing));
    EXPECT_FALSE(listener.reload_context(qb::io::ssl::Context{}));
    EXPECT_EQ(listener.ssl_handle(), serving);

    auto next = insecure_pair();
    ASSERT_TRUE(open_tls_connection(listener, port, next));
    ASSERT_TRUE(complete_tls_handshake(next));
    EXPECT_NE(presented_name(next.client).find("CN=qb-reload-current"), std::string::npos) << presented_name(next.client);
    EXPECT_TRUE(tls_round_trip(next, "ping", "pong"));
}

// The ALPN list a listener sets is the context's (Huly QB-308). It used to be a buffer the listener owned and
// registered with the context's select callback, so the context a reload left read whatever list the listener
// set NEXT: the connection accepted before the reload, handshaken after it, negotiated the new context's list.
TEST(TlsContextReload, TheAlpnListStaysWithTheContextAReloadLeaves) {
    const generated_identity before{"qb-alpn-before"};
    const generated_identity after{"qb-alpn-after"};
    ASSERT_TRUE(before.ok() && after.ok()) << "could not generate the two test certificates";

    qb::io::tcp::ssl::listener listener{before.context()};
    ASSERT_TRUE(listener.set_supported_alpn_protocols({"h2"}));
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    // Accepted on the first context, its handshake left for after the reload.
    auto accepted_before = alpn_pair();
    ASSERT_TRUE(open_tls_connection(listener, port, accepted_before));

    const auto renewed = after.context();
    ASSERT_TRUE(listener.reload_context(renewed));
    ASSERT_TRUE(listener.set_supported_alpn_protocols({"http/1.1"}));

    ASSERT_TRUE(complete_tls_handshake(accepted_before));
    EXPECT_EQ(accepted_before.client.get_alpn_selected_protocol(), "h2") << "the context the reload left negotiated the new context's list";

    auto accepted_after = alpn_pair();
    ASSERT_TRUE(open_tls_connection(listener, port, accepted_after));
    ASSERT_TRUE(complete_tls_handshake(accepted_after));
    EXPECT_EQ(accepted_after.client.get_alpn_selected_protocol(), "http/1.1");
    EXPECT_TRUE(tls_round_trip(accepted_before, "old?", "old!"));
}

// A context outlives the listener that set its ALPN list (Huly QB-308): the list the listener owned was freed
// with it while the context's select callback still pointed at it -- a heap use-after-free on the next
// listener's handshakes (red under ASan; without it, the freed bytes may still read as the list).
TEST(TlsContextReload, TheAlpnListOutlivesTheListenerThatSetIt) {
    const generated_identity identity{"qb-alpn-shared"};
    ASSERT_TRUE(identity.ok()) << "could not generate the test certificate";
    const auto shared = identity.context();
    ASSERT_TRUE(shared.ok()) << shared.error();
    {
        qb::io::tcp::ssl::listener first{shared};
        ASSERT_TRUE(first.set_supported_alpn_protocols({"h2"}));
    } // the listener goes; the context it configured serves on

    qb::io::tcp::ssl::listener second{shared};
    ASSERT_EQ(second.listen_v4(0, "127.0.0.1"), 0);
    const auto port = second.local_endpoint().port();
    ASSERT_NE(port, 0);

    auto pair = alpn_pair();
    ASSERT_TRUE(open_tls_connection(second, port, pair));
    ASSERT_TRUE(complete_tls_handshake(pair));
    EXPECT_EQ(pair.client.get_alpn_selected_protocol(), "h2");
    EXPECT_TRUE(tls_round_trip(pair, "ping", "pong"));
}
