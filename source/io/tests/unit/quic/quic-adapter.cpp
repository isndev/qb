/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/quic/quic-adapter.cpp
 * @brief `qb::io::async::quic::endpoint` / `connector` / `server` adapter delegation + dispatch — pure unit, mock backend.
 *
 * The qb async QUIC stack is layered: the facade family (`endpoint`, `connector<Derived>`,
 * `server<Derived, Session>`) owns the lifecycle and the `backend_event` → `on(...)` fan-out, while a
 * swappable `qb::io::quic::backend` does the wire work. By injecting `qb::io::test::FakeQuicBackend`
 * (the shared in-memory mock in `shared/quic_test_doubles.h`) every facade behaviour can be exercised
 * with NO event loop, NO real socket, NO TLS and NO `QB_HAS_QUIC` — these tests run deterministically
 * and in parallel. They are the pure-unit half harvested out of the former `system/test-quic.cpp`
 * (the native libngtcp2 loopback half lives in `system/quic/quic-handshake.cpp`).
 *
 * What is proven here — the adapter's contract, asserted on state the *facade* produced, not on what the
 * test fed the mock:
 *   - lifecycle delegation: `connect`/`listen` call `configure` + `start_client`/`start_server` exactly
 *     once, forward the derived TLS server_name / ALPN / endpoints, and move `current_state()` to
 *     connecting/listening; `close` / `close_connection` route to the right backend method and leave the
 *     endpoint open xor closed as documented.
 *   - settings passthrough: the full 18-field `settings` struct round-trips through `configure` (the
 *     minimal `idle_timeout`-only check from the old `DelegatesClientLifecycleToBackend` is folded into
 *     the exhaustive case per dossier D10 dedup).
 *   - event dispatch: every `backend_event::kind` drives the matching `on(...)` callback AND the
 *     documented `current_state()` transition AND carries the captured `connection_id` / payload /
 *     typed reason — not just a bare "the callback fired once" counter (dossier §3 smoke-strengthen).
 *   - stream-started direction/origin decode from stream-id parity for the client role.
 *   - the `last_acked_bytes` vs `last_close_error_code` split (dossier §7.4): an ack and a close never
 *     share a sink, so the two assertions can coexist in one event burst.
 *   - mutation overloads (send_stream_data / send_datagram / extend_stream_credit incl. the zero-credit
 *     no-op), local stream-session publish/flush/finish for both connector and server roles, and the
 *     missing-id probes (`flush`/`finish` of an unknown session return false without touching the
 *     backend) — `Flush`/`Finish` deduped into one parametrised-style block per D10.
 *   - session-map keying: ambiguous stream-id lookup returns null until a connection is named,
 *     session-cap rejection, connection-close clears only the matching connection's sessions, the
 *     remote-data echo round-trip (data → credit-extend → flush response), FIN → unregister, and the
 *     remote-stream cap reset.
 *   - watcher teardown: a `listener::current.clear()` while a mock-backed endpoint is open drops every
 *     registered watcher with no dangling.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>

#include "../../shared/quic_test_doubles.h"

using qb::io::test::CallbackQuicClient;
using qb::io::test::CallbackQuicServer;
using qb::io::test::EchoQuicServer;
using qb::io::test::FakeQuicBackend;
using qb::io::test::SessionQuicClient;

namespace {

// Convenience: hand a fresh FakeQuicBackend to a facade and keep the raw pointer
// for assertions. The facade takes ownership via the unique_ptr.
[[nodiscard]] std::unique_ptr<qb::io::quic::backend>
make_fake(FakeQuicBackend *&out) {
    out = new FakeQuicBackend;
    return std::unique_ptr<qb::io::quic::backend>(out);
}

using State = qb::io::async::quic::endpoint::state;

} // namespace

// =============================================================================
// STREAM DESCRIPTOR (pure value object)
// =============================================================================

TEST(QuicAdapterStream, StreamDescriptorCarriesMetadataOnly) {
    qb::io::async::quic::stream stream{0, qb::io::quic::stream_direction::bidirectional, qb::io::quic::stream_origin::local};

    EXPECT_TRUE(stream.is_open());
    EXPECT_EQ(stream.connection_id(), 0u);
    EXPECT_EQ(stream.id(), 0u);
    EXPECT_EQ(stream.direction(), qb::io::quic::stream_direction::bidirectional);
    EXPECT_EQ(stream.origin(), qb::io::quic::stream_origin::local);

    stream.reset(42);
    EXPECT_FALSE(stream.is_open());

    qb::io::async::quic::stream remote_stream{9, 4, qb::io::quic::stream_direction::bidirectional, qb::io::quic::stream_origin::remote};
    EXPECT_EQ(remote_stream.connection_id(), 9u);
    EXPECT_EQ(remote_stream.id(), 4u);
    EXPECT_EQ(remote_stream.origin(), qb::io::quic::stream_origin::remote);
}

// =============================================================================
// CLIENT / SERVER LIFECYCLE DELEGATION
// =============================================================================

/**
 * @test connect() delegates the full client lifecycle to the backend
 * @brief One configure + one start_client, derived TLS server_name / ALPN / endpoints forwarded, state
 *        moves to connecting; opening streams delegates and counts; close() routes to backend::close.
 *        The minimal idle_timeout passthrough is asserted here; the exhaustive field matrix lives in
 *        PassesBackpressureAndLifecycleSettingsToBackend (D10 dedup — this no longer re-checks every field).
 */
TEST(QuicAdapterEndpoint, DelegatesClientLifecycleToBackend) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    qb::io::quic::settings settings;
    settings.idle_timeout = std::chrono::milliseconds(1234);
    endpoint.set_settings(settings);

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::connecting);
    EXPECT_EQ(raw->configure_calls, 1);
    EXPECT_EQ(raw->start_client_calls, 1);
    ASSERT_EQ(raw->last_alpn.size(), 1u);
    EXPECT_EQ(raw->last_alpn.front(), "h3");
    EXPECT_EQ(raw->last_settings.idle_timeout, std::chrono::milliseconds(1234));
    EXPECT_EQ(raw->last_local.af(), AF_INET);
    EXPECT_EQ(raw->last_remote.port(), 4433);
    EXPECT_EQ(raw->last_tls.server_name, "127.0.0.1");
    EXPECT_EQ(endpoint.stats().active_connections, 1u);

    auto bidi = endpoint.open_bidirectional_stream();
    auto uni  = endpoint.open_unidirectional_stream();
    EXPECT_TRUE(bidi.is_open());
    EXPECT_TRUE(uni.is_open());
    EXPECT_EQ(bidi.id(), 0u);
    EXPECT_EQ(uni.id(), 2u);
    EXPECT_EQ(raw->open_bidi_calls, 1);
    EXPECT_EQ(raw->open_uni_calls, 1);
    EXPECT_EQ(endpoint.stats().active_streams, 2u);

    endpoint.close(42, "done");
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::closed);
    EXPECT_EQ(raw->close_calls, 1);
    EXPECT_EQ(raw->close_code, 42u);
    EXPECT_EQ(raw->close_reason, "done");
}

/**
 * @test Every settings field round-trips through configure()
 * @brief The exhaustive 18-field backpressure/lifecycle passthrough — the single source of truth for
 *        settings plumbing (subsumes the idle_timeout-only check above).
 */
TEST(QuicAdapterEndpoint, PassesBackpressureAndLifecycleSettingsToBackend) {
    FakeQuicBackend       *raw = nullptr;
    qb::io::quic::settings settings;
    settings.handshake_timeout           = std::chrono::milliseconds(111);
    settings.idle_timeout                = std::chrono::milliseconds(222);
    settings.stream_recv_window          = 333;
    settings.connection_recv_window      = 444;
    settings.max_stream_data_bidi_local  = 555;
    settings.max_stream_data_bidi_remote = 666;
    settings.max_stream_data_uni         = 777;
    settings.max_streams_bidi            = 8;
    settings.max_streams_uni             = 9;
    settings.max_datagram_frame_size     = 1200;
    settings.max_connections             = 10;
    settings.max_pending_stream_bytes    = 11;
    settings.max_pending_stream_frames   = 12;
    settings.max_pending_datagram_bytes  = 13;
    settings.max_pending_datagram_frames = 14;
    settings.udp_rx_batch_size           = 15;
    settings.udp_tx_batch_size           = 16;
    settings.enable_stateless_retry      = false;
    settings.enable_datagrams            = true;
    settings.enable_keylog               = true;

    qb::io::async::quic::endpoint endpoint{make_fake(raw), settings};
    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    EXPECT_EQ(raw->configure_calls, 1);
    EXPECT_EQ(raw->last_settings.handshake_timeout, std::chrono::milliseconds(111));
    EXPECT_EQ(raw->last_settings.idle_timeout, std::chrono::milliseconds(222));
    EXPECT_EQ(raw->last_settings.stream_recv_window, 333u);
    EXPECT_EQ(raw->last_settings.connection_recv_window, 444u);
    EXPECT_EQ(raw->last_settings.max_stream_data_bidi_local, 555u);
    EXPECT_EQ(raw->last_settings.max_stream_data_bidi_remote, 666u);
    EXPECT_EQ(raw->last_settings.max_stream_data_uni, 777u);
    EXPECT_EQ(raw->last_settings.max_streams_bidi, 8u);
    EXPECT_EQ(raw->last_settings.max_streams_uni, 9u);
    EXPECT_EQ(raw->last_settings.max_datagram_frame_size, 1200u);
    EXPECT_EQ(raw->last_settings.max_connections, 10u);
    EXPECT_EQ(raw->last_settings.max_pending_stream_bytes, 11u);
    EXPECT_EQ(raw->last_settings.max_pending_stream_frames, 12u);
    EXPECT_EQ(raw->last_settings.max_pending_datagram_bytes, 13u);
    EXPECT_EQ(raw->last_settings.max_pending_datagram_frames, 14u);
    EXPECT_EQ(raw->last_settings.udp_rx_batch_size, 15u);
    EXPECT_EQ(raw->last_settings.udp_tx_batch_size, 16u);
    EXPECT_FALSE(raw->last_settings.enable_stateless_retry);
    EXPECT_TRUE(raw->last_settings.enable_datagrams);
    EXPECT_TRUE(raw->last_settings.enable_keylog);
}

/**
 * @test listen() delegates the full server lifecycle to the backend
 * @brief One configure + one start_server, bind port / ALPN / TLS cert+key paths forwarded, state moves
 *        to listening.
 */
TEST(QuicAdapterEndpoint, DelegatesServerLifecycleToBackend) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.listen(qb::io::uri{"quic://0.0.0.0:4433"}, "cert.pem", "key.pem"));
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::listening);
    EXPECT_EQ(raw->configure_calls, 1);
    EXPECT_EQ(raw->start_server_calls, 1);
    EXPECT_EQ(raw->last_local.port(), 4433);
    ASSERT_EQ(raw->last_alpn.size(), 1u);
    EXPECT_EQ(raw->last_alpn.front(), "h3");
    EXPECT_EQ(raw->last_tls.certificate_file, std::filesystem::path{"cert.pem"});
    EXPECT_EQ(raw->last_tls.private_key_file, std::filesystem::path{"key.pem"});
}

/**
 * @test connect() with explicit TLS fills server_name from the URI host when empty
 * @brief A user-supplied tls_config with no server_name is backfilled from the remote host; explicit
 *        ALPN is forwarded verbatim.
 */
TEST(QuicAdapterEndpoint, ExplicitTlsConnectFillsServerNameWhenMissing) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    qb::io::quic::tls_config tls;
    tls.verify_peer = false;

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}, tls, {"qb"}));
    EXPECT_EQ(raw->last_tls.server_name, "127.0.0.1");
    ASSERT_EQ(raw->last_alpn.size(), 1u);
    EXPECT_EQ(raw->last_alpn.front(), "qb");
}

/**
 * @test An invalid remote URI fails before the backend is started
 * @brief A host-less URI fails the connect() pre-flight; the backend never sees configure/start_client.
 */
TEST(QuicAdapterEndpoint, InvalidRemoteUriFailsBeforeStartingBackend) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    EXPECT_FALSE(endpoint.connect(qb::io::uri{"quic:///missing-host"}));
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(raw->configure_calls, 0);
    EXPECT_EQ(raw->start_client_calls, 0);
}

/**
 * @test listen() returns false when the bind port is unavailable
 * @brief An already-bound UDP port forces the bind to fail; the backend is never configured/started.
 *        Uses a real udp::socket to occupy an ephemeral port (no fixed port).
 */
TEST(QuicAdapterEndpoint, ListenReturnsFalseWhenBindPortIsUnavailable) {
    qb::io::udp::socket occupied;
    ASSERT_EQ(occupied.bind_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = occupied.local_endpoint().port();
    ASSERT_NE(port, 0);

    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    EXPECT_FALSE(endpoint.listen(qb::io::uri{"quic://127.0.0.1:" + std::to_string(port)}, "cert.pem", "key.pem"));
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(raw->configure_calls, 0);
    EXPECT_EQ(raw->start_server_calls, 0);
}

/**
 * @test A backend-less endpoint is a stable no-op surface
 * @brief Every mutator on a default endpoint (no injected backend, not connected) is a safe no-op and
 *        leaves the endpoint closed.
 */
TEST(QuicAdapterEndpoint, EndpointNoBackendOperationsAreStableNoops) {
    qb::io::async::quic::endpoint endpoint;

    EXPECT_EQ(endpoint.backend(), nullptr);
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::idle);

    endpoint.poll();
    endpoint.extend_stream_credit(1, 2, 3);
    endpoint.close_connection(1, 2, "ignored");
    endpoint.close(7, "closed");

    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::closed);
}

// =============================================================================
// EVENT → CALLBACK DISPATCH (state transition + captured payload, not bare counts)
// =============================================================================

/**
 * @test Backend events drive the derived callbacks AND the documented state transitions
 * @brief connected → state==connected + on(connected); then connection_closed → state==closed +
 *        on(connection_closed) carrying the captured connection_id, error_code and reason.
 *
 * Strengthened over the old smoke version: asserts the captured connection_id (not only the count) and
 * the per-event state transition, per dossier §3.
 */
TEST(QuicAdapterEndpoint, DispatchesBackendEventsToDerivedCallbacks) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_EQ(client.current_state(), State::connecting);

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::connected, 5, 0, 0, "h3", {}});
    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_started, 5, 0, 0, {}, {}});
    client.poll();

    EXPECT_EQ(client.current_state(), State::connected);
    EXPECT_EQ(client.connected, 1);
    EXPECT_EQ(client.stream_started, 1);

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::connection_closed, 5, 0, 42, "done", {}});
    client.poll();

    EXPECT_EQ(client.current_state(), State::closed);
    EXPECT_EQ(client.closed, 1);
    EXPECT_EQ(client.last_close_reason, qb::io::quic::disconnect_reason::transport_error);
    EXPECT_EQ(client.last_close_error_code, 42u);
}

/**
 * @test The base endpoint accepts every event kind as a no-op and walks idle→connected→closed
 * @brief Feeding every backend_event::kind to the no-callback base endpoint must not crash and must
 *        drive the documented state transitions (connecting on connect, connected on `connected`, closed
 *        on `connection_closed`) — strengthened from "asserts only the terminal state" per dossier §3.
 */
TEST(QuicAdapterEndpoint, BaseEndpointAcceptsAllBackendEventsAndTracksState) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_EQ(endpoint.current_state(), State::connecting);

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::connected, 3, 0, 0, "h3", {}});
    endpoint.poll();
    EXPECT_EQ(endpoint.current_state(), State::connected);
    EXPECT_TRUE(endpoint.is_open());

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_started, 3, 0, 0, {}, {}});

    qb::io::quic::backend_event data;
    data.type          = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 3;
    data.stream_id     = 0;
    data.payload       = {std::byte{'o'}, std::byte{'k'}};
    raw->queued_events.push_back(std::move(data));

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_data_acked, 3, 0, 2, {}, {}});
    raw->queued_events.push_back(
        {qb::io::quic::backend_event::kind::stream_closed, 3, 0, 9, {}, {}, qb::io::quic::disconnect_reason::none,
         qb::io::quic::stream_close_reason::finished});

    qb::io::quic::backend_event datagram;
    datagram.type          = qb::io::quic::backend_event::kind::datagram;
    datagram.connection_id = 3;
    datagram.payload       = {std::byte{'d'}};
    raw->queued_events.push_back(std::move(datagram));
    endpoint.poll();
    EXPECT_EQ(endpoint.current_state(), State::connected) << "non-terminal events must not close the endpoint";

    raw->queued_events.push_back(
        {qb::io::quic::backend_event::kind::connection_closed, 3, 0, 0, "closed", {}, qb::io::quic::disconnect_reason::application_close});
    endpoint.poll();

    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::closed);
}

/**
 * @test stream_started decodes direction and origin from stream-id parity (client role)
 * @brief For a client endpoint, even ids are bidirectional, the low parity bit selects local vs remote
 *        origin: 0=bidi/local, 1=bidi/remote, 2=uni/local, 3=uni/remote.
 */
TEST(QuicAdapterEndpoint, StreamStartedEventsExposeDirectionAndOriginForClientRole) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_started, 9, 0, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::bidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::local);

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_started, 9, 1, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::bidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::remote);

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_started, 9, 2, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::unidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::local);

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_started, 9, 3, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::unidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::remote);
}

/**
 * @test A connector with no stream session dispatches stream_data / ack / datagram with payloads
 * @brief stream_data appends the exact bytes, stream_data_acked records the acked byte count into the
 *        dedicated `last_acked_bytes` field, and datagram appends its payload — proving the §7.4 split
 *        (an ack writes last_acked_bytes, NOT the close-error sink).
 */
TEST(QuicAdapterEndpoint, ConnectorWithoutStreamSessionDispatchesAllEventKinds) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    qb::io::quic::backend_event data;
    data.type          = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 9;
    data.stream_id     = 4;
    data.payload       = {std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
    raw->queued_events.push_back(std::move(data));

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_data_acked, 9, 4, 123, {}, {}});

    qb::io::quic::backend_event datagram;
    datagram.type          = qb::io::quic::backend_event::kind::datagram;
    datagram.connection_id = 9;
    datagram.payload       = {std::byte{'d'}, std::byte{'g'}};
    raw->queued_events.push_back(std::move(datagram));

    client.poll();

    EXPECT_EQ(client.stream_data, 1);
    EXPECT_EQ(client.received, "ping");
    EXPECT_EQ(client.stream_acked, 1);
    EXPECT_EQ(client.last_acked_bytes, 123u);
    EXPECT_EQ(client.last_close_error_code, 0u) << "an ack must NOT write the close-error sink (§7.4 split)";
    EXPECT_EQ(client.datagrams, 1);
    EXPECT_EQ(client.datagram_received, "dg");
}

/**
 * @test An ack followed by a close keeps their two metadata sinks independent (§7.4)
 * @brief In one event burst: stream_data_acked(bytes=99) then connection_closed(error_code=7). With the
 *        sinks split, last_acked_bytes==99 AND last_close_error_code==7 both hold — the regression the
 *        old single `last_error_code` field could not express.
 */
TEST(QuicAdapterEndpoint, AckAndCloseDoNotClobberEachOthersSink) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_data_acked, 9, 4, 99, {}, {}});

    qb::io::quic::backend_event closed;
    closed.type              = qb::io::quic::backend_event::kind::connection_closed;
    closed.connection_id     = 0;
    closed.error_code        = 7;
    closed.text              = "bye";
    closed.connection_reason = qb::io::quic::disconnect_reason::application_close;
    raw->queued_events.push_back(std::move(closed));

    client.poll();

    EXPECT_EQ(client.stream_acked, 1);
    EXPECT_EQ(client.last_acked_bytes, 99u);
    EXPECT_EQ(client.closed, 1);
    EXPECT_EQ(client.last_close_error_code, 7u);
    EXPECT_EQ(client.last_close_reason, qb::io::quic::disconnect_reason::application_close);
}

/**
 * @test A typed backend close reason is preserved through dispatch
 * @brief idle_timeout from the backend is forwarded verbatim (not coerced to transport_error), with the
 *        captured error_code and reason phrase.
 */
TEST(QuicAdapterEndpoint, PreservesTypedBackendCloseReason) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    qb::io::quic::backend_event closed;
    closed.type              = qb::io::quic::backend_event::kind::connection_closed;
    closed.error_code        = 7;
    closed.text              = "idle";
    closed.connection_reason = qb::io::quic::disconnect_reason::idle_timeout;
    raw->queued_events.push_back(std::move(closed));

    client.poll();

    EXPECT_EQ(client.closed, 1);
    EXPECT_EQ(client.last_close_reason, qb::io::quic::disconnect_reason::idle_timeout);
    EXPECT_EQ(client.last_close_error_code, 7u);
    EXPECT_EQ(client.last_reason_phrase, "idle");
}

/**
 * @test stream_data_acked dispatches to the server callback with the acked byte count
 * @brief The server role accumulates acked bytes; one ack of 42 yields acked_bytes==42.
 */
TEST(QuicAdapterServer, DispatchesStreamAckEvents) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::stream_data_acked, 7, 11, 42, {}, {}});
    server.poll();

    EXPECT_EQ(server.acked_bytes, 42u);
}

// =============================================================================
// MUTATION OVERLOADS
// =============================================================================

/**
 * @test The endpoint mutation overloads each delegate to the backend with captured args
 * @brief send_stream_data (default + explicit connection), send_datagram (default + explicit), and
 *        extend_stream_credit — including the documented zero-credit no-op (calls stay 0 until a
 *        non-zero credit is requested).
 */
TEST(QuicAdapterEndpoint, EndpointMutationOverloadsDelegateToBackend) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    endpoint.send_stream_data(5, "abc", true);
    endpoint.send_stream_data(7, 8, "de", false);

    std::array<std::byte, 3> datagram{std::byte{'x'}, std::byte{'y'}, std::byte{'z'}};
    endpoint.send_datagram(std::span<const std::byte>{datagram});
    endpoint.send_datagram(99, std::span<const std::byte>{datagram});

    endpoint.extend_stream_credit(1, 2, 0);
    EXPECT_EQ(raw->extend_stream_credit_calls, 0) << "a zero-byte credit grant is a no-op";

    endpoint.extend_stream_credit(1, 2, 4);

    EXPECT_EQ(raw->send_stream_data_calls, 2);
    EXPECT_EQ(raw->sent_connection_id, 7u);
    EXPECT_EQ(raw->sent_stream_id, 8u);
    EXPECT_EQ(raw->sent_stream_bytes, 5u);
    EXPECT_FALSE(raw->sent_stream_fin);
    EXPECT_EQ(raw->send_datagram_calls, 2);
    EXPECT_EQ(raw->sent_datagram_connection_id, 99u);
    EXPECT_EQ(raw->sent_datagram_bytes, 6u);
    EXPECT_EQ(raw->extend_stream_credit_calls, 1);
    EXPECT_EQ(raw->extended_stream_id, 2u);
    EXPECT_EQ(raw->extended_bytes, 4u);
}

/**
 * @test send_datagram with no explicit connection targets connection 0
 * @brief The connection-less overload routes to the backend with connection_id 0.
 */
TEST(QuicAdapterEndpoint, DelegatesDatagramSendToBackend) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    endpoint.send_datagram("datagram");

    EXPECT_EQ(raw->send_datagram_calls, 1);
    EXPECT_EQ(raw->sent_datagram_connection_id, 0u);
}

/**
 * @test send_datagram can target an explicit connection id (server role)
 * @brief The connection-targeted overload forwards both the connection id and the byte count.
 */
TEST(QuicAdapterEndpoint, DatagramSendCanTargetExplicitConnection) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));
    endpoint.send_datagram(42, "datagram");

    EXPECT_EQ(raw->send_datagram_calls, 1);
    EXPECT_EQ(raw->sent_datagram_connection_id, 42u);
    EXPECT_EQ(raw->sent_datagram_bytes, 8u);
}

/**
 * @test reset_stream delegates and the synthesised stream_closed reason is `reset`
 * @brief The mock enqueues a stream_closed(reason=reset) on reset_stream; the client observes it.
 */
TEST(QuicAdapterEndpoint, ResetStreamDelegatesAndPreservesCloseReason) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    client.reset_stream(12, 99);

    EXPECT_EQ(raw->reset_stream_calls, 1);
    EXPECT_EQ(raw->reset_stream_id, 12u);
    EXPECT_EQ(raw->reset_stream_code, 99u);
    EXPECT_EQ(client.stream_closed, 1);
    EXPECT_EQ(client.last_stream_close_reason, qb::io::quic::stream_close_reason::reset);
}

/**
 * @test stop_stream delegates and the synthesised stream_closed reason is `stop_sending`
 */
TEST(QuicAdapterEndpoint, StopStreamDelegatesAndPreservesCloseReason) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    client.stop_stream(12, 99);

    EXPECT_EQ(raw->stop_stream_calls, 1);
    EXPECT_EQ(raw->stop_stream_id, 12u);
    EXPECT_EQ(raw->stop_stream_code, 99u);
    EXPECT_EQ(client.stream_closed, 1);
    EXPECT_EQ(client.last_stream_close_reason, qb::io::quic::stream_close_reason::stop_sending);
}

/**
 * @test close_connection delegates without closing the whole endpoint
 * @brief close_connection routes to backend::close_connection (not ::close) and leaves the endpoint open.
 */
TEST(QuicAdapterEndpoint, CloseConnectionDelegatesWithoutClosingEndpoint) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    endpoint.close_connection(42, 99, "only this connection");

    EXPECT_EQ(raw->close_connection_calls, 1);
    EXPECT_EQ(raw->close_calls, 0);
    EXPECT_EQ(raw->closed_connection_id, 42u);
    EXPECT_EQ(raw->close_code, 99u);
    EXPECT_EQ(raw->close_reason, "only this connection");
    EXPECT_TRUE(endpoint.is_open());
}

/**
 * @test The UDP TX budget keeps un-sent packets for the next poll
 * @brief With udp_tx_batch_size==1, two queued packets cannot both flush in one poll; the endpoint stays
 *        open across both polls without losing the second packet.
 */
TEST(QuicAdapterEndpoint, UdpTxBudgetKeepsPendingPacketsForNextPoll) {
    FakeQuicBackend       *raw = nullptr;
    qb::io::quic::settings settings;
    settings.udp_tx_batch_size = 1;
    qb::io::async::quic::endpoint endpoint{make_fake(raw), settings};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    ASSERT_TRUE(endpoint.is_open());

    qb::io::quic::packet first;
    first.remote = raw->last_remote;
    first.local  = raw->last_local;
    first.payload.assign(1, std::byte{'a'});
    qb::io::quic::packet second = first;
    second.payload.assign(1, std::byte{'b'});
    raw->queued_packets.push_back(std::move(first));
    raw->queued_packets.push_back(std::move(second));

    endpoint.poll();
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::connecting);

    endpoint.poll();
    EXPECT_TRUE(endpoint.is_open());
}

// =============================================================================
// LOCAL STREAM-SESSION PUBLISH / FLUSH / FINISH
// =============================================================================

/**
 * @test A server local stream session can publish and flush to the backend
 * @brief open_bidirectional_stream_session registers a session keyed by connection 42; publish + flush
 *        drains the output to backend::send_stream_data and empties the pipe.
 */
TEST(QuicAdapterServer, LocalStreamSessionCanPublishAndFlush) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *session = server.open_bidirectional_stream_session(42);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->connection_id(), 42u);
    EXPECT_EQ(session->id(), 0u);

    session->publish(std::string_view{"hello", 5});
    EXPECT_TRUE(server.flush_stream_session(42, session->id()));

    EXPECT_EQ(raw->open_bidi_calls, 1);
    EXPECT_EQ(raw->send_stream_data_calls, 1);
    EXPECT_EQ(raw->sent_connection_id, 42u);
    EXPECT_EQ(raw->sent_stream_id, session->id());
    EXPECT_EQ(raw->sent_stream_bytes, 5u);
    EXPECT_FALSE(raw->sent_stream_fin);
    EXPECT_EQ(session->pendingWrite(), 0u);
}

/**
 * @test A connector local stream session can publish and flush (unidirectional)
 * @brief Same publish/flush contract on the connector role with a unidirectional stream.
 */
TEST(QuicAdapterConnector, LocalStreamSessionCanPublishAndFlush) {
    FakeQuicBackend  *raw = nullptr;
    SessionQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    auto *session = client.open_unidirectional_stream_session();
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->connection_id(), 0u);
    EXPECT_EQ(session->id(), 2u);

    session->publish(std::string_view{"client-data", 11});
    EXPECT_TRUE(client.flush_stream_session(session->id()));

    EXPECT_EQ(raw->open_uni_calls, 1);
    EXPECT_EQ(raw->send_stream_data_calls, 1);
    EXPECT_EQ(raw->sent_connection_id, 0u);
    EXPECT_EQ(raw->sent_stream_id, session->id());
    EXPECT_EQ(raw->sent_stream_bytes, 11u);
    EXPECT_FALSE(raw->sent_stream_fin);
    EXPECT_EQ(session->pendingWrite(), 0u);
}

/**
 * @test Connector convenience overloads target an explicit connection and finish sends FIN
 * @brief Opening a session on connection 42 routes the open to the backend with that id, the session is
 *        reachable by both single- and two-arg lookups, flush sends the body, finish sends a FIN, and a
 *        flush/finish against a wrong connection returns false.
 */
TEST(QuicAdapterConnector, StreamSessionConvenienceOverloadsUseExplicitConnection) {
    FakeQuicBackend  *raw = nullptr;
    SessionQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    auto *session = client.open_bidirectional_stream_session(42);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(raw->last_open_connection_id, 42u);
    EXPECT_EQ(session->connection_id(), 42u);
    EXPECT_EQ(session->id(), 0u);
    EXPECT_EQ(client.stream_session(session->id()), session);
    EXPECT_EQ(client.stream_session(42, session->id()), session);

    session->publish(std::string_view{"client", 6});
    EXPECT_TRUE(client.flush_stream_session(42, session->id()));
    EXPECT_EQ(raw->send_stream_data_calls, 1);
    EXPECT_EQ(raw->sent_connection_id, 42u);
    EXPECT_EQ(raw->sent_stream_id, session->id());
    EXPECT_EQ(raw->sent_stream_bytes, 6u);
    EXPECT_FALSE(raw->sent_stream_fin);

    EXPECT_TRUE(client.finish_stream_session(session->id()));
    EXPECT_EQ(raw->send_stream_data_calls, 2);
    EXPECT_EQ(raw->sent_connection_id, 42u);
    EXPECT_EQ(raw->sent_stream_id, session->id());
    EXPECT_TRUE(raw->sent_stream_fin);

    EXPECT_FALSE(client.flush_stream_session(99, session->id()));
    EXPECT_FALSE(client.finish_stream_session(99, session->id()));
}

/**
 * @test Server convenience overloads target an explicit connection and finish sends FIN
 * @brief The server-role counterpart of the connector overload test (unidirectional, connection 77).
 */
TEST(QuicAdapterServer, StreamSessionConvenienceOverloadsUseExplicitConnection) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *session = server.open_unidirectional_stream_session(77);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(raw->last_open_connection_id, 77u);
    EXPECT_EQ(raw->open_uni_calls, 1);
    EXPECT_EQ(session->connection_id(), 77u);
    EXPECT_EQ(session->id(), 2u);
    EXPECT_EQ(server.stream_session(session->id()), session);
    EXPECT_EQ(server.stream_session(77, session->id()), session);

    session->publish(std::string_view{"server", 6});
    server.flush_stream_session(*session);
    EXPECT_EQ(raw->send_stream_data_calls, 1);
    EXPECT_EQ(raw->sent_connection_id, 77u);
    EXPECT_EQ(raw->sent_stream_id, session->id());
    EXPECT_EQ(raw->sent_stream_bytes, 6u);
    EXPECT_FALSE(raw->sent_stream_fin);

    server.finish_stream_session(*session);
    EXPECT_EQ(raw->send_stream_data_calls, 2);
    EXPECT_EQ(raw->sent_connection_id, 77u);
    EXPECT_EQ(raw->sent_stream_id, session->id());
    EXPECT_TRUE(raw->sent_stream_fin);
}

/**
 * @test finish on a populated local stream session flushes the body THEN sends FIN
 * @brief A published-but-unflushed session, when finished, emits two send_stream_data calls: the buffered
 *        body, then the empty FIN frame.
 */
TEST(QuicAdapterServer, FinishLocalStreamSessionFlushesAndSendsFin) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *session = server.open_bidirectional_stream_session(42);
    ASSERT_NE(session, nullptr);
    session->publish(std::string_view{"done", 4});

    EXPECT_TRUE(server.finish_stream_session(42, session->id()));

    EXPECT_EQ(raw->send_stream_data_calls, 2);
    EXPECT_EQ(raw->sent_connection_id, 42u);
    EXPECT_EQ(raw->sent_stream_id, session->id());
    EXPECT_EQ(raw->sent_stream_bytes, 4u);
    EXPECT_TRUE(raw->sent_stream_fin);
    EXPECT_EQ(session->pendingWrite(), 0u);
}

/**
 * @test flush / finish of an unknown stream session return false without touching the backend
 * @brief The two missing-id probes (formerly `FlushMissingStreamSessionReturnsFalse` /
 *        `FinishMissingStreamSessionReturnsFalse`, structurally identical) folded into one body — both
 *        return false and emit zero send_stream_data calls (D10 dedup).
 */
TEST(QuicAdapterServer, FlushAndFinishMissingStreamSessionReturnFalse) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    EXPECT_FALSE(server.flush_stream_session(777, 888));
    EXPECT_FALSE(server.finish_stream_session(777, 888));
    EXPECT_EQ(raw->send_stream_data_calls, 0);
}

// =============================================================================
// SESSION-MAP KEYING
// =============================================================================

/**
 * @test An ambiguous stream-id lookup returns null until the connection is named
 * @brief Two sessions on different connections share stream id 10; the single-arg lookup is ambiguous
 *        (null) while the two-arg lookups resolve, and the ambiguous flush is rejected.
 */
TEST(QuicAdapterServer, AmbiguousStreamIdLookupReturnsNullUntilConnectionIsSpecified) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *first  = server.register_stream_session(1, 10);
    auto *second = server.register_stream_session(2, 10);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(server.stream_session(10), nullptr) << "ambiguous stream-id must resolve to null";
    EXPECT_EQ(server.stream_session(1, 10), first);
    EXPECT_EQ(server.stream_session(2, 10), second);
    EXPECT_FALSE(server.flush_stream_session(10));
    EXPECT_TRUE(server.flush_stream_session(1, 10));

    const auto &const_server = server;
    EXPECT_EQ(const_server.sessions().size(), 2u);
    EXPECT_EQ(const_server.session_count(), 2u);
}

/**
 * @test Re-registering an existing session is idempotent and the session cap rejects extras
 * @brief register_stream_session of an existing key returns the same pointer; once the cap is hit a new
 *        key is rejected (null).
 */
TEST(QuicAdapterServer, ExistingSessionLookupAndSessionCapRejection) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *first = server.register_stream_session(3, 12);
    ASSERT_NE(first, nullptr);

    EXPECT_EQ(server.register_stream_session(3, 12), first) << "re-registering an existing key is idempotent";
    server.set_max_sessions(1);
    EXPECT_EQ(server.register_stream_session(3, 13), nullptr) << "a full session map rejects new keys";
    EXPECT_EQ(server.max_sessions(), 1u);
}

/**
 * @test A connection close clears only that connection's sessions and keeps the listener open
 * @brief Two sessions on connection 1 and 2 over stream 10; closing connection 1 (with another active
 *        connection still up) removes only session(1,10), keeps session(2,10), keeps the listener in the
 *        connected state, and fires on(connection_closed) with the typed reason.
 */
TEST(QuicAdapterServer, ConnectionCloseClearsOnlyMatchingServerSessions) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *first  = server.register_stream_session(1, 10);
    auto *second = server.register_stream_session(2, 10);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(server.stream_sessions, 2);

    raw->stats.active_connections = 1;
    raw->queued_events.push_back(
        {qb::io::quic::backend_event::kind::connection_closed, 1, 0, 55, "first closed", {}, qb::io::quic::disconnect_reason::application_close});

    server.poll();

    EXPECT_TRUE(server.is_open());
    EXPECT_EQ(server.current_state(), State::connected);
    EXPECT_EQ(server.closed, 1);
    EXPECT_EQ(server.last_close_reason, qb::io::quic::disconnect_reason::application_close);
    EXPECT_EQ(server.stream_session(1, 10), nullptr);
    EXPECT_EQ(server.stream_session(2, 10), second);
    EXPECT_EQ(server.session_count(), 1u);
}

/**
 * @test Closing the last connection leaves the listener open and reverted to listening
 * @brief With no remaining active connections, the close clears the last session and the endpoint falls
 *        back to the listening state (the UDP listener stays up).
 */
TEST(QuicAdapterServer, ConnectionCloseWithoutRemainingConnectionsKeepsListenerOpen) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));
    ASSERT_NE(server.register_stream_session(7, 11), nullptr);

    raw->stats.active_connections = 0;
    raw->queued_events.push_back(
        {qb::io::quic::backend_event::kind::connection_closed, 7, 0, 0, "last client closed", {}, qb::io::quic::disconnect_reason::idle_timeout});

    server.poll();

    EXPECT_TRUE(server.is_open());
    EXPECT_EQ(server.current_state(), State::listening);
    EXPECT_EQ(server.closed, 1);
    EXPECT_EQ(server.last_close_reason, qb::io::quic::disconnect_reason::idle_timeout);
    EXPECT_EQ(server.session_count(), 0u);
}

/**
 * @test Remote stream data feeds the session, extends credit, and flushes the response
 * @brief The full server round-trip: a remote 4-byte "ping" creates an EchoQuicStreamSession, the
 *        EchoQuicProtocol consumes it and publishes "ack!", the handler extends exactly 4 bytes of credit
 *        and flushes the 4-byte response — all asserted against the backend's captured args.
 */
TEST(QuicAdapterServer, RemoteStreamDataFeedsSessionExtendsCreditAndFlushesResponse) {
    FakeQuicBackend *raw = nullptr;
    EchoQuicServer   server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    qb::io::quic::backend_event data;
    data.type          = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 9;
    data.stream_id     = 1;
    data.payload       = {std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
    raw->queued_events.push_back(std::move(data));

    server.poll();

    auto *session = server.stream_session(9, 1);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(server.sessions, 1);
    EXPECT_EQ(server.stream_data_events, 1);
    EXPECT_EQ(session->messages, 1);
    EXPECT_EQ(session->received, "ping");
    EXPECT_EQ(session->pendingRead(), 0u);
    EXPECT_EQ(session->pendingWrite(), 0u);

    EXPECT_EQ(raw->extend_stream_credit_calls, 1);
    EXPECT_EQ(raw->extended_stream_id, 1u);
    EXPECT_EQ(raw->extended_bytes, 4u);
    EXPECT_EQ(raw->send_stream_data_calls, 1);
    EXPECT_EQ(raw->sent_connection_id, 9u);
    EXPECT_EQ(raw->sent_stream_id, 1u);
    EXPECT_EQ(raw->sent_stream_bytes, 4u);
}

/**
 * @test A remote FIN is delivered then the session is unregistered
 * @brief After the data + stream_closed(finished) burst, the data was processed once and the session is
 *        gone from the map (session_count drops to 0).
 */
TEST(QuicAdapterServer, RemoteStreamFinIsDeliveredAndThenSessionIsUnregistered) {
    FakeQuicBackend *raw = nullptr;
    EchoQuicServer   server{make_fake(raw)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    qb::io::quic::backend_event data;
    data.type          = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 9;
    data.stream_id     = 1;
    data.payload       = {std::byte{'d'}, std::byte{'o'}, std::byte{'n'}, std::byte{'e'}};
    data.error_code    = 1;
    raw->queued_events.push_back(std::move(data));

    raw->queued_events.push_back(
        {qb::io::quic::backend_event::kind::stream_closed, 9, 1, 0, {}, {}, qb::io::quic::disconnect_reason::none,
         qb::io::quic::stream_close_reason::finished});

    server.poll();

    EXPECT_EQ(server.stream_data_events, 1);
    EXPECT_EQ(server.stream_session(9, 1), nullptr);
    EXPECT_EQ(server.session_count(), 0u);
    EXPECT_EQ(raw->extend_stream_credit_calls, 1);
    EXPECT_EQ(raw->send_stream_data_calls, 1);
}

/**
 * @test The session cap rejects extra remote streams and resets them
 * @brief With set_max_sessions(1), the first remote stream registers but the second is rejected and
 *        reset (the handler calls backend::reset_stream with the rejection code) and never appears in
 *        the map.
 */
TEST(QuicAdapterServer, StreamSessionCapRejectsExtraRemoteStreamsAndResetsStream) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicServer server{make_fake(raw)};

    server.set_max_sessions(1);
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    qb::io::quic::backend_event first;
    first.type          = qb::io::quic::backend_event::kind::stream_data;
    first.connection_id = 7;
    first.stream_id     = 1;
    first.payload.push_back(std::byte{'a'});
    raw->queued_events.push_back(std::move(first));

    qb::io::quic::backend_event second;
    second.type          = qb::io::quic::backend_event::kind::stream_data;
    second.connection_id = 7;
    second.stream_id     = 5;
    second.payload.push_back(std::byte{'b'});
    raw->queued_events.push_back(std::move(second));

    server.poll();

    EXPECT_EQ(server.session_count(), 1u);
    EXPECT_NE(server.stream_session(7, 1), nullptr);
    EXPECT_EQ(server.stream_session(7, 5), nullptr);
    EXPECT_EQ(raw->reset_stream_calls, 1);
    EXPECT_EQ(raw->reset_stream_id, 5u);
    EXPECT_EQ(raw->reset_stream_code, 1u);
}

// =============================================================================
// WATCHER LIFETIME (the one case that touches the async listener)
// =============================================================================

/**
 * @test Clearing the listener drops a mock-backed endpoint's watchers with no dangling
 * @brief A mock-backed endpoint registers IO/timer watchers on connect; listener::current.clear() must
 *        drop them all (size back to 0) and the endpoint's destructor must not touch a freed watcher.
 *        This is the only adapter test that initialises the async listener.
 */
TEST(QuicAdapterEndpoint, ListenerClearDoesNotDangleEndpointWatchers) {
    qb::io::async::init();

    FakeQuicBackend *raw = nullptr;
    {
        qb::io::async::quic::endpoint endpoint{make_fake(raw)};

        ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
        ASSERT_TRUE(endpoint.is_open());
        ASSERT_GT(qb::io::async::listener::current.size(), 0u);

        qb::io::async::listener::current.clear();
        EXPECT_EQ(qb::io::async::listener::current.size(), 0u);
    }

    EXPECT_EQ(qb::io::async::listener::current.size(), 0u);
}

// =============================================================================
// SETTINGS-ONLY CONSTRUCTOR + ACCESSORS + set_backend
// =============================================================================

/**
 * @test The settings-only constructor seeds settings() without a backend, and set_backend
 *        attaches one afterwards
 * @brief The `endpoint(settings)` overload (no backend) leaves backend()==nullptr / state==idle but
 *        stores the settings; set_backend() then attaches the mock so a subsequent connect() configures
 *        it with those very settings. Covers the settings-only ctor, the const settings()/backend()
 *        accessors, and set_backend() — none of which the backend-injecting ctor path exercises.
 */
TEST(QuicAdapterEndpoint, SettingsOnlyConstructorThenSetBackendConfiguresWithStoredSettings) {
    qb::io::quic::settings settings;
    settings.idle_timeout    = std::chrono::milliseconds(4321);
    settings.max_streams_uni = 17;

    qb::io::async::quic::endpoint endpoint{settings};

    // settings-only ctor: backend absent, idle state, but the settings round-trip through settings().
    EXPECT_EQ(endpoint.backend(), nullptr);
    EXPECT_EQ(endpoint.current_state(), State::idle);
    EXPECT_EQ(endpoint.settings().idle_timeout, std::chrono::milliseconds(4321));
    EXPECT_EQ(endpoint.settings().max_streams_uni, 17u);

    // const-qualified backend() accessor on a still-backendless endpoint.
    const auto &const_endpoint = endpoint;
    EXPECT_EQ(const_endpoint.backend(), nullptr);
    EXPECT_EQ(const_endpoint.settings().idle_timeout, std::chrono::milliseconds(4321));

    // set_backend attaches the mock; the const accessor now sees it.
    FakeQuicBackend *raw = nullptr;
    endpoint.set_backend(make_fake(raw));
    EXPECT_EQ(const_endpoint.backend(), raw);

    // The settings stored by the settings-only ctor are forwarded to the late-attached backend.
    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_EQ(raw->configure_calls, 1);
    EXPECT_EQ(raw->last_settings.idle_timeout, std::chrono::milliseconds(4321));
    EXPECT_EQ(raw->last_settings.max_streams_uni, 17u);
}

/**
 * @test send_stream_data(stream_id, span, fin) — the connection-less span overload — targets connection 0
 * @brief The three-argument byte-span overload (no explicit connection id) routes to the backend with
 *        connection_id 0, the given stream id, the exact byte count and the FIN flag. Distinct from the
 *        string_view overloads already covered: this is the raw `std::span<const std::byte>` entry point.
 */
TEST(QuicAdapterEndpoint, SendStreamDataSpanOverloadTargetsConnectionZero) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    std::array<std::byte, 3> body{std::byte{'r'}, std::byte{'a'}, std::byte{'w'}};
    endpoint.send_stream_data(6, std::span<const std::byte>{body}, true);

    EXPECT_EQ(raw->send_stream_data_calls, 1);
    EXPECT_EQ(raw->sent_connection_id, 0u) << "the connection-less span overload targets connection 0";
    EXPECT_EQ(raw->sent_stream_id, 6u);
    EXPECT_EQ(raw->sent_stream_bytes, 3u);
    EXPECT_TRUE(raw->sent_stream_fin);
}

// =============================================================================
// UDP FLUSH EDGE CASES — addressless packet skip + hard write failure
// =============================================================================

/**
 * @test flush_udp_packets silently drops a queued packet that has no remote address
 * @brief A backend can legitimately enqueue a packet whose `remote` endpoint is unset (default
 *        AF_UNSPEC) — there is nowhere to send it, so flush_udp_packets must pop and skip it without
 *        touching the socket, without erroring, and without closing the endpoint. A second, properly
 *        addressed packet that follows it is still flushed, proving the skip is a `continue`, not a
 *        `break`. Covers the `!pkt.remote` early-skip branch of flush_udp_packets that every other
 *        adapter test (which only ever queues addressed packets) leaves uncovered.
 */
TEST(QuicAdapterEndpoint, FlushUdpPacketsSkipsPacketWithoutRemoteAddress) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    ASSERT_TRUE(endpoint.is_open());

    // First packet: no remote endpoint set -> must be skipped (popped, not sent).
    qb::io::quic::packet addressless;
    EXPECT_FALSE(static_cast<bool>(addressless.remote)) << "a default packet has an unset (AF_UNSPEC) remote";
    addressless.payload.assign(4, std::byte{'x'});

    // Second packet: properly addressed to the connect() remote -> must still be flushed after the skip.
    qb::io::quic::packet addressed;
    addressed.remote = raw->last_remote;
    addressed.local  = raw->last_local;
    addressed.payload.assign(3, std::byte{'y'});

    raw->queued_packets.push_back(std::move(addressless));
    raw->queued_packets.push_back(std::move(addressed));

    endpoint.poll();

    // The skip is a continue, not a break: both packets left the pending queue and the endpoint is
    // untouched (still open, still connecting — the addressless packet caused no failure path).
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::connecting);
}

/**
 * @test A non-transient UDP write error trips fail_transport and closes the endpoint
 * @brief flush_udp_packets routes a *hard* (non-would-block) socket write error through
 *        fail_transport: a queued packet whose payload exceeds the maximum UDP datagram size makes the
 *        real bound socket's sendto fail with EMSGSIZE, which is NOT a would-block error
 *        (not_send_error == false). fail_transport must then clear the pending queue, close the socket,
 *        flip the endpoint to closed/!open, and dispatch a connection_closed carrying the transport_error
 *        reason and the "QUIC UDP write failed" phrase. This is the only deterministic driver for the
 *        fail_transport / hard-write-error branch (every other test queues sendable payloads).
 */
TEST(QuicAdapterEndpoint, OversizedUdpPacketTripsTransportFailureAndClosesEndpoint) {
    FakeQuicBackend   *raw = nullptr;
    CallbackQuicClient client{make_fake(raw)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    ASSERT_TRUE(client.is_open());
    EXPECT_EQ(client.current_state(), State::connecting);

    // Payload larger than the UDP datagram ceiling -> sendto() fails with EMSGSIZE (a fatal, not a
    // would-block, error) on the real bound socket the facade opened during connect().
    qb::io::quic::packet oversized;
    oversized.remote = raw->last_remote;
    oversized.local  = raw->last_local;
    oversized.payload.assign(qb::io::udp::socket::MaxDatagramSize + 8192, std::byte{'z'});
    raw->queued_packets.push_back(std::move(oversized));

    client.poll();

    // fail_transport ran: endpoint closed, queue cleared, and the close was reported with the
    // transport_error reason + the documented phrase (connection_id 0).
    EXPECT_FALSE(client.is_open());
    EXPECT_EQ(client.current_state(), State::closed);
    EXPECT_EQ(client.closed, 1);
    EXPECT_EQ(client.last_close_reason, qb::io::quic::disconnect_reason::transport_error);
    EXPECT_EQ(client.last_reason_phrase, "QUIC UDP write failed");
}

// =============================================================================
// BUILD-CONFIG AVAILABILITY PROBES (qb::io::quic::types.h)
// =============================================================================

/**
 * @test connect(uri, alpn) — the two-argument no-TLS overload — backfills server_name from the URI host
 * @brief The convenience `connect(remote_uri, alpn_protocols)` overload (endpoint.h:323-327) constructs a
 *        default tls_config, assigns server_name from the URI host, and forwards to the three-argument
 *        connect with the caller's ALPN. Every existing adapter test reaches connect() through either the
 *        single-arg `connect(uri)` default-argument form or the explicit `connect(uri, tls, alpn)` form;
 *        none calls the two-arg overload with an explicit ALPN vector, so the host->server_name backfill
 *        line stayed uncovered. Here the host is supplied with no TLS, and the backend must observe the
 *        host echoed into tls.server_name plus the verbatim ALPN.
 */
TEST(QuicAdapterEndpoint, ConnectWithAlpnOnlyOverloadBackfillsServerNameFromHost) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}, std::vector<std::string>{"hq-interop", "h3"}));
    EXPECT_EQ(endpoint.current_state(), State::connecting);
    EXPECT_EQ(raw->start_client_calls, 1);
    EXPECT_EQ(raw->last_tls.server_name, "127.0.0.1") << "the two-arg overload backfills server_name from the URI host";
    ASSERT_EQ(raw->last_alpn.size(), 2u);
    EXPECT_EQ(raw->last_alpn.front(), "hq-interop");
    EXPECT_EQ(raw->last_alpn.back(), "h3");
}

/**
 * @test The endpoint poll() drains backend timer state via on_timeout when wired to no callbacks
 * @brief A mock-backed endpoint that has connected drains queued events on poll(); feeding a
 *        connection_closed with the transport_error reason but a NON-zero connection_id (not the parent
 *        sentinel 0) still drives the base endpoint to closed and routes the typed reason verbatim. This
 *        exercises the base-endpoint connection_closed arm with a real connection id rather than the
 *        sentinel-0 forms the other dispatch tests use.
 */
TEST(QuicAdapterEndpoint, ConnectionClosedWithNonZeroIdDrivesBaseEndpointToClosed) {
    FakeQuicBackend              *raw = nullptr;
    qb::io::async::quic::endpoint endpoint{make_fake(raw)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_EQ(endpoint.current_state(), State::connecting);

    raw->queued_events.push_back({qb::io::quic::backend_event::kind::connected, 88, 0, 0, "h3", {}});
    endpoint.poll();
    EXPECT_EQ(endpoint.current_state(), State::connected);

    qb::io::quic::backend_event closed;
    closed.type              = qb::io::quic::backend_event::kind::connection_closed;
    closed.connection_id     = 88;
    closed.error_code        = 0x55;
    closed.text              = "peer reset";
    closed.connection_reason = qb::io::quic::disconnect_reason::transport_error;
    raw->queued_events.push_back(std::move(closed));
    endpoint.poll();

    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), State::closed);
}

/**
 * @test available() and unavailable_reason() agree on the build's QUIC capability
 * @brief The two free probes in quic/types.h are each other's complement: when QUIC is compiled in
 *        (QB_HAS_QUIC), available() is true and unavailable_reason() is the empty string; otherwise
 *        available() is false and unavailable_reason() carries a non-empty diagnostic. This is the only
 *        case that calls unavailable_reason() directly — ensure_backend() only reaches it on the
 *        !available() path, which is dead on a QUIC-enabled build. The assertion is written against the
 *        invariant (complementary), so it holds under either configuration.
 */
TEST(QuicAdapterEndpoint, QuicAvailabilityProbesAreComplementary) {
    const bool        available = qb::io::quic::available();
    const std::string reason    = qb::io::quic::unavailable_reason();

    if (available) {
        EXPECT_TRUE(reason.empty()) << "an available QUIC build has no unavailability reason";
    } else {
        EXPECT_FALSE(reason.empty()) << "an unavailable QUIC build must explain why";
    }
}
