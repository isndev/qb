/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/quic/quic-handshake.cpp
 * @brief Native QUIC backend validation + real loopback handshakes — the qb-io QUIC system suite.
 *
 * The genuinely system-tier half harvested out of `system/test-quic.cpp`: every case here needs the real
 * libngtcp2 backend (`make_native_backend()`, behind `QB_HAS_QUIC`), and most open a real UDP loopback
 * socket and complete a real TLS handshake with the shipped local certificate. The mock-driven
 * delegation/dispatch tests are gone — they are pure unit and live in `unit/quic/quic-adapter.cpp`.
 *
 * Two policies from the restructure spec are applied throughout:
 *   - CERT ABSENCE IS A HARD FAILURE in a QUIC build, not a silent skip. The harness ships
 *     `resources/ssl/cert.pem` + `key.pem`; if they are missing the build/packaging is broken and these
 *     tests must say so (`ASSERT_TRUE(require_ssl_files())`), never report green while testing nothing.
 *     Whether QUIC itself is compiled in is still a legitimate capability gate (`QB_HAS_QUIC`).
 *   - DE-FLAKE the loopback handshakes: the fixed `seconds(3)` + 1 ms busy-sleep loops are replaced by the
 *     shared deadline-bounded `qb::io::test::pump_until` (drives the framework's own `run_for`), which
 *     fails LOUDLY on timeout instead of asserting in a hand-rolled wall-clock loop. Ephemeral ports
 *     everywhere (`quic://127.0.0.1:0`), no fixed ports.
 *
 * Coverage:
 *   - availability / native gating;
 *   - native backend input validation (ALPN, server TLS files) and the pre-connection no-op stability +
 *     send-queue / datagram cap enforcement (typed disconnect_reason + human-text substring);
 *   - the two-backend in-process handshake that hand-delivers packets and drives a child connection;
 *   - real loopback handshakes: client→server stream + datagram, ALPN mismatch never connects, one server
 *     demuxing two clients by connection id;
 *   - ADDED per dossier qbio-c16 §"Missing cases": a SERVER-INITIATED stream over the live loopback
 *     (server opens a stream session and pushes to the client), and a GRACEFUL-CLOSE handshake that
 *     asserts the client observes the server's application close end-to-end (typed disconnect_reason).
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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>

#ifdef QB_HAS_QUIC
#include <ngtcp2/ngtcp2.h>
#endif

#include "../../shared/coroutine_test_support.h"
#include "../../shared/quic_test_doubles.h"
#include "../../shared/ssl_fixtures.h"

using qb::io::test::CallbackQuicClient;
using qb::io::test::CallbackQuicServer;
using qb::io::test::pump_until;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

// =============================================================================
// AVAILABILITY / NATIVE GATING (always compiled — asserts the build's QUIC state)
// =============================================================================

/**
 * @test The library reports its QUIC availability consistently with the build flag
 * @brief Under QB_HAS_QUIC: available(), backend name "libngtcp2", non-empty version, crypto
 *        initialised, native_backend_ready(). Otherwise: the negative path with a non-empty reason.
 */
TEST(QuicHandshakeAvailability, ReportsBuildQuicState) {
#ifdef QB_HAS_QUIC
    EXPECT_TRUE(qb::io::quic::available());
    const auto backend = qb::io::quic::native_backend_info();
    EXPECT_EQ(backend.name, "libngtcp2");
    EXPECT_FALSE(backend.version.empty());
    EXPECT_TRUE(backend.crypto_initialized);
    EXPECT_TRUE(qb::io::quic::native_backend_ready());
#else
    EXPECT_FALSE(qb::io::quic::available());
    EXPECT_FALSE(qb::io::quic::unavailable_reason().empty());
    EXPECT_FALSE(qb::io::quic::native_backend_ready());
#endif
}

/**
 * @test A backend-less endpoint connect() behaves per build
 * @brief With QUIC, a default endpoint lazily creates the native backend and moves to connecting; without
 *        QUIC, connect() throws and the endpoint stays idle.
 */
TEST(QuicHandshakeAvailability, EndpointUsesNativeBackendWhenAvailable) {
    qb::io::async::quic::endpoint endpoint;

#ifdef QB_HAS_QUIC
    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::connecting);
    ASSERT_NE(endpoint.backend(), nullptr);
#else
    EXPECT_THROW({ [[maybe_unused]] auto &&discarded_ = endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}); }, std::runtime_error);
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::idle);
#endif
}

#ifdef QB_HAS_QUIC

using qb::io::test::deliver_quic_packets;
using qb::io::test::quic_payload;

namespace quic_handshake_pending_writer_test {

// A direct driver drains when ready, then captures the fresh SDK expiry.
// Receive counters are immediate; encoded TX counters belong to the writer phase.
struct DrainObservation {
    qb::io::quic::stats                   before;
    qb::io::quic::stats                   after;
    std::chrono::steady_clock::time_point expiry_before;
    std::chrono::steady_clock::time_point expiry_after;
    bool                                  drained        = false;
    bool                                  writable_after = false;
};

DrainObservation
deliver_ready_quic_packets(qb::io::quic::backend &from, qb::io::quic::backend &to) {
    // Readiness takes precedence over the existing SDK timer. A deadline sampled
    // before a writer is an observation only; it is never armed for a ready source.
    if (!from.wants_write()) {
        const auto expiry = from.next_timeout();
        const auto now    = std::chrono::steady_clock::now();
        if (expiry <= now)
            from.on_timeout(now);
    }
    DrainObservation observation;
    observation.before        = from.current_stats();
    observation.expiry_before = from.next_timeout();
    observation.drained       = from.wants_write();
    if (observation.drained)
        qb::io::test::deliver_quic_packets(from, to);
    observation.after          = from.current_stats();
    observation.expiry_after   = from.next_timeout();
    observation.writable_after = from.wants_write();
    EXPECT_EQ(observation.after.packets_received, observation.before.packets_received);
    EXPECT_EQ(observation.after.bytes_received, observation.before.bytes_received);
    EXPECT_GE(observation.after.packets_sent, observation.before.packets_sent);
    EXPECT_GE(observation.after.bytes_sent, observation.before.bytes_sent);
    return observation;
}

} // namespace quic_handshake_pending_writer_test

using quic_handshake_pending_writer_test::deliver_ready_quic_packets;

// =============================================================================
// NATIVE BACKEND INPUT VALIDATION
// =============================================================================

/**
 * @test The native backend rejects invalid ALPN before opening native resources
 * @brief Empty, blank, and oversize ALPN entries each throw std::invalid_argument from start_client.
 */
TEST(QuicHandshakeNativeBackend, RejectsInvalidAlpnBeforeOpeningNativeResources) {
    auto                     backend = qb::io::quic::make_native_backend();
    qb::io::quic::tls_config tls;
    tls.verify_peer = false;
    const qb::io::endpoint local{"127.0.0.1", 0};
    const qb::io::endpoint remote{"127.0.0.1", 4433};

    EXPECT_THROW(backend->start_client(local, remote, {}, tls), std::invalid_argument);
    EXPECT_THROW(backend->start_client(local, remote, {""}, tls), std::invalid_argument);
    EXPECT_THROW(backend->start_client(local, remote, {std::string(256, 'x')}, tls), std::invalid_argument);
}

/**
 * @test The native backend validates server TLS configuration
 * @brief Missing cert/key fields throw invalid_argument; present-but-unreadable paths throw runtime_error.
 */
TEST(QuicHandshakeNativeBackend, ValidatesServerTlsConfiguration) {
    auto                   backend = qb::io::quic::make_native_backend();
    const qb::io::endpoint local{"127.0.0.1", 0};

    qb::io::quic::tls_config missing_files;
    EXPECT_THROW(backend->start_server(local, {"h3"}, missing_files), std::invalid_argument);

    qb::io::quic::tls_config unreadable_files;
    unreadable_files.certificate_file = ssl_resource_path("missing-cert.pem");
    unreadable_files.private_key_file = ssl_resource_path("missing-key.pem");
    EXPECT_THROW(backend->start_server(local, {"h3"}, unreadable_files), std::runtime_error);
}

/**
 * @test Pre-connection operations are stable and report failures
 * @brief Before any connection, the query methods are inert no-ops, open_stream throws, the synthetic
 *        reset/stop events carry the right stream ids/reasons, and close_connection emits an
 *        application_close with the supplied error code.
 */
TEST(QuicHandshakeNativeBackend, PreConnectionOperationsAreStableAndReportFailures) {
    auto                   backend = qb::io::quic::make_native_backend();
    const qb::io::endpoint local{"127.0.0.1", 0};
    const qb::io::endpoint remote{"127.0.0.1", 4433};
    auto                   payload = quic_payload();

    EXPECT_FALSE(backend->wants_write());
    EXPECT_EQ(backend->next_timeout(), std::chrono::steady_clock::time_point::max());
    EXPECT_TRUE(backend->drain_packets().empty());
    EXPECT_TRUE(backend->drain_events().empty());
    EXPECT_EQ(backend->current_stats().active_connections, 0u);
    EXPECT_THROW(
        { [[maybe_unused]] auto &&discarded_ = backend->open_stream(qb::io::quic::stream_direction::bidirectional); }, std::runtime_error);

    qb::io::quic::packet_view datagram{remote, local, std::span<const std::byte>{payload.data(), payload.size()}};
    backend->on_udp_datagram(datagram);
    backend->on_timeout(std::chrono::steady_clock::now());
    backend->extend_stream_credit(0, 4, 0);
    backend->extend_stream_credit(0, 4, 64);

    backend->reset_stream(0, 7, 11);
    backend->stop_stream(0, 8, 12);
    auto events = backend->drain_events();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, qb::io::quic::backend_event::kind::stream_closed);
    EXPECT_EQ(events[0].stream_id, 7u);
    EXPECT_EQ(events[0].stream_reason, qb::io::quic::stream_close_reason::reset);
    EXPECT_EQ(events[1].type, qb::io::quic::backend_event::kind::stream_closed);
    EXPECT_EQ(events[1].stream_id, 8u);
    EXPECT_EQ(events[1].stream_reason, qb::io::quic::stream_close_reason::stop_sending);

    backend->close_connection(42, 99, "closed before connect");
    events = backend->drain_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().type, qb::io::quic::backend_event::kind::connection_closed);
    EXPECT_EQ(events.front().error_code, 99u);
    EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::application_close);
}

/**
 * @test Pre-connection send queue and datagram caps are enforced with typed reasons
 * @brief Exhausts each cap (stream byte queue, stream frame queue, datagrams-disabled, configured-max
 *        datagram size, datagram frame queue, datagram byte queue) and asserts both the typed
 *        disconnect_reason AND a substring of the human-readable text.
 */
TEST(QuicHandshakeNativeBackend, EnforcesPreConnectionSendQueueLimits) {
    auto payload = quic_payload();

    {
        auto                   backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.max_pending_stream_frames = 0;
        settings.max_pending_stream_bytes  = 3;
        backend->configure(settings);
        backend->send_stream_data(0, 1, std::span<const std::byte>{payload}, false);

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("byte queue"), std::string::npos);
    }

    {
        auto                   backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.max_pending_stream_frames = 0;
        settings.max_pending_stream_bytes  = 64;
        backend->configure(settings);
        backend->send_stream_data(0, 1, std::span<const std::byte>{payload}, false);
        EXPECT_TRUE(backend->drain_events().empty()) << "the first frame fits the queue";
        EXPECT_FALSE(backend->wants_write()) << "queued data with no connection to carry it is not a due writer attempt";

        settings.max_pending_stream_frames = 1;
        backend->configure(settings);
        backend->send_stream_data(0, 1, std::span<const std::byte>{payload}, false);

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("frame queue"), std::string::npos);
    }

    {
        auto backend = qb::io::quic::make_native_backend();
        backend->send_datagram(0, {});
        EXPECT_TRUE(backend->drain_events().empty());

        backend->send_datagram(0, std::span<const std::byte>{payload});
        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::protocol_error);
    }

    {
        auto                   backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.enable_datagrams        = true;
        settings.max_datagram_frame_size = 3;
        backend->configure(settings);
        backend->send_datagram(0, std::span<const std::byte>{payload});

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("configured maximum"), std::string::npos);
    }

    {
        auto                   backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.enable_datagrams            = true;
        settings.max_datagram_frame_size     = 64;
        settings.max_pending_datagram_frames = 1;
        backend->configure(settings);
        backend->send_datagram(0, std::span<const std::byte>{payload});
        backend->send_datagram(0, std::span<const std::byte>{payload});

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("frame queue"), std::string::npos);
    }

    {
        auto                   backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.enable_datagrams            = true;
        settings.max_datagram_frame_size     = 64;
        settings.max_pending_datagram_frames = 0;
        settings.max_pending_datagram_bytes  = 3;
        backend->configure(settings);
        backend->send_datagram(0, std::span<const std::byte>{payload});

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("byte queue"), std::string::npos);
    }
}

/**
 * @test A started server stays open across no-connection traffic and a final close emits application_close
 * @brief Drives empty/malformed datagrams, timeouts, and child-connection operations against an
 *        unknown connection id (all inert), then closes and asserts the application_close event.
 *        Requires the shipped certs (loud fail).
 */
TEST(QuicHandshakeNativeBackend, ServerParentNoConnectionPathsRemainOpen) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    auto                     backend = qb::io::quic::make_native_backend();
    const qb::io::endpoint   local{"127.0.0.1", 0};
    qb::io::quic::tls_config tls;
    tls.certificate_file = ssl_resource_path("cert.pem");
    tls.private_key_file = ssl_resource_path("key.pem");
    backend->start_server(local, {"h3"}, tls);

    auto                      payload = quic_payload();
    qb::io::quic::packet_view empty_datagram{local, local, std::span<const std::byte>{}};
    qb::io::quic::packet_view malformed_datagram{local, local, std::span<const std::byte>{payload.data(), payload.size()}};

    backend->on_udp_datagram(empty_datagram);
    backend->on_udp_datagram(malformed_datagram);
    backend->on_timeout(std::chrono::steady_clock::now());
    EXPECT_EQ(backend->next_timeout(), std::chrono::steady_clock::time_point::max());
    EXPECT_FALSE(backend->wants_write());
    EXPECT_TRUE(backend->drain_packets().empty());
    EXPECT_TRUE(backend->drain_events().empty());
    EXPECT_EQ(backend->current_stats().active_connections, 0u);

    backend->send_stream_data(99, 1, std::span<const std::byte>{payload}, false);
    backend->send_datagram(99, std::span<const std::byte>{payload});
    backend->extend_stream_credit(99, 1, 8);
    backend->reset_stream(99, 1, 2);
    backend->stop_stream(99, 1, 3);
    backend->close_connection(99, 4, "missing");
    EXPECT_TRUE(backend->drain_events().empty());
    EXPECT_THROW(
        { [[maybe_unused]] auto &&discarded_ = backend->open_stream(99, qb::io::quic::stream_direction::bidirectional); }, std::runtime_error);

    backend->close(5, "server closed");
    auto events = backend->drain_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().connection_reason, qb::io::quic::disconnect_reason::application_close);
    EXPECT_EQ(events.front().error_code, 5u);
}

// =============================================================================
// TWO-BACKEND IN-PROCESS HANDSHAKE (no event loop; hand-delivered packets)
// =============================================================================

/**
 * @test Two native backends complete a handshake and route child-connection operations
 * @brief A server and client backend driven against each other by hand-delivering drained packets;
 *        within a bounded deadline both reach `connected`, then the server opens streams on the child
 *        connection, sends data/datagram/reset/stop/credit, and a close_connection emits the typed
 *        application_close carrying the child connection id and error code.
 */
TEST(QuicHandshakeNativeBackend, DirectBackendsRouteServerChildOperations) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::quic::settings settings;
    settings.enable_datagrams        = true;
    settings.max_datagram_frame_size = 1200;

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();
    EXPECT_FALSE(server->smoothed_rtt(0).has_value());
    EXPECT_FALSE(client->smoothed_rtt(0).has_value());
    EXPECT_FALSE(server->set_accepting_connections(false));
    server->configure(settings);
    client->configure(settings);

    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint client_endpoint{"127.0.0.1", 54321};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    bool          client_connected     = false;
    std::uint64_t server_connection_id = 0;
    const auto    deadline             = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_connected || server_connection_id == 0) && std::chrono::steady_clock::now() < deadline) {
        deliver_ready_quic_packets(*client, *server);
        deliver_ready_quic_packets(*server, *client);

        for (auto const &event : client->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connected)
                client_connected = true;
        for (auto const &event : server->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connected)
                server_connection_id = event.connection_id;
    }

    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);
    EXPECT_EQ(server->current_stats().active_connections, 1u);

    ASSERT_TRUE(server->smoothed_rtt(server_connection_id).has_value());
    ASSERT_TRUE(client->smoothed_rtt(0).has_value());
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(*server->smoothed_rtt(server_connection_id)).count(),
              server->current_stats().smoothed_rtt_us);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(*client->smoothed_rtt(0)).count(), client->current_stats().smoothed_rtt_us);
    EXPECT_FALSE(server->smoothed_rtt(0).has_value());
    EXPECT_FALSE(server->smoothed_rtt(server_connection_id + 1).has_value());
    EXPECT_FALSE(client->smoothed_rtt(1).has_value());
    EXPECT_FALSE(client->set_accepting_connections(false));
    ASSERT_TRUE(server->set_accepting_connections(false)); // the established child below remains routable

    auto explicit_stream = server->open_stream(server_connection_id, qb::io::quic::stream_direction::bidirectional);
    auto implicit_stream = server->open_stream(0, qb::io::quic::stream_direction::unidirectional);
    EXPECT_NE(explicit_stream, implicit_stream);

    auto payload = quic_payload();
    server->send_stream_data(server_connection_id, explicit_stream, std::span<const std::byte>{payload}, false);
    server->send_datagram(server_connection_id, std::span<const std::byte>{payload});
    server->reset_stream(server_connection_id, explicit_stream, 17);
    server->stop_stream(server_connection_id, implicit_stream, 18);
    server->extend_stream_credit(server_connection_id, explicit_stream, 32);

    server->close_connection(server_connection_id, 19, "server child closed");
    EXPECT_FALSE(server->smoothed_rtt(server_connection_id).has_value());
    auto events = server->drain_events();
    auto closed = std::find_if(events.begin(), events.end(),
                               [](auto const &event) { return event.type == qb::io::quic::backend_event::kind::connection_closed; });
    ASSERT_NE(closed, events.end());
    EXPECT_EQ(closed->connection_id, server_connection_id);
    EXPECT_EQ(closed->error_code, 19u);
    EXPECT_EQ(closed->connection_reason, qb::io::quic::disconnect_reason::application_close);
}

// =============================================================================
// Address-validation Retry (RFC 9000 §8.1). With enable_stateless_retry on (the default), the
// server answers a first, untokened Initial with a Retry and allocates NO connection state; the
// client must re-send its Initial carrying the token (proving it can receive at its claimed source
// address) before the handshake proceeds. An off-path spoofed-Initial flood cannot make the server
// allocate state. With the setting off, the server accepts the Initial directly. Both must handshake.
// =============================================================================
namespace {
void
drive_two_backend_handshake(qb::io::quic::backend &client, qb::io::quic::backend &server, bool &client_connected,
                            std::uint64_t &server_connection_id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_connected || server_connection_id == 0) && std::chrono::steady_clock::now() < deadline) {
        deliver_ready_quic_packets(server, client);
        deliver_ready_quic_packets(client, server);
        for (auto const &event : client.drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connected)
                client_connected = true;
        for (auto const &event : server.drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connected)
                server_connection_id = event.connection_id;
    }
}
} // namespace

namespace quic_admission_test {
class Refusal : public ::testing::TestWithParam<int> {};
} // namespace quic_admission_test

using quic_admission_test::Refusal;

TEST_P(Refusal, InitialIsClosedWithoutAConnectionAndAdmissionCanResume) {
    ASSERT_TRUE(require_ssl_files());
    const qb::io::endpoint   server_address{"127.0.0.1", 4433};
    const qb::io::endpoint   client_address{"127.0.0.1", 54321};
    qb::io::quic::tls_config tls;
    tls.certificate_file          = ssl_resource_path("cert.pem");
    tls.private_key_file          = ssl_resource_path("key.pem");
    auto                   server = qb::io::quic::make_native_backend();
    qb::io::quic::settings settings;
    settings.enable_stateless_retry = GetParam() != 0;
    server->configure(settings);
    server->start_server(server_address, {"h3"}, tls);
    tls.verify_peer = false;
    tls.server_name = "localhost";
    auto client     = qb::io::quic::make_native_backend();
    client->start_client(client_address, server_address, {"h3"}, tls);
    if (GetParam() == 2) {
        deliver_quic_packets(*client, *server);
        ASSERT_EQ(server->current_stats().active_connections, 0u);
        ASSERT_TRUE(server->wants_write());
        deliver_quic_packets(*server, *client); // obtain Retry before admission is disabled
    }
    auto initials = client->drain_packets();
    ASSERT_FALSE(initials.empty());
    ASSERT_TRUE(server->set_accepting_connections(false));
    ASSERT_TRUE(server->set_accepting_connections(false));
    for (auto const &initial : initials) {
        server->on_udp_datagram({initial.remote, initial.local, initial.payload});
        ASSERT_TRUE(server->wants_write());
        auto refusal = server->drain_packets();
        ASSERT_EQ(refusal.size(), 1u);
        EXPECT_LE(refusal.front().payload.size(), initial.payload.size());
        EXPECT_FALSE(server->wants_write());
        EXPECT_EQ(server->next_timeout(), std::chrono::steady_clock::time_point::max());
        client->on_udp_datagram({refusal.front().remote, refusal.front().local, refusal.front().payload});
    }
    EXPECT_EQ(server->current_stats().active_connections, 0u);
    EXPECT_TRUE(server->drain_events().empty()) << "refusal must not close the listener or publish a child";
    const auto events = client->drain_events();
    EXPECT_TRUE(std::none_of(events.begin(), events.end(),
                             [](auto const &event) { return event.type == qb::io::quic::backend_event::kind::connected; }));
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](auto const &event) {
        return event.type == qb::io::quic::backend_event::kind::connection_closed
               && event.connection_reason == qb::io::quic::disconnect_reason::application_close;
    })) << "the native peer must decode the refusal as CONNECTION_CLOSE, not a timeout or unreadable packet";

    ASSERT_TRUE(server->set_accepting_connections(true));
    auto fresh = qb::io::quic::make_native_backend();
    fresh->start_client(client_address, server_address, {"h3"}, tls);
    bool          connected = false;
    std::uint64_t id        = 0;
    drive_two_backend_handshake(*fresh, *server, connected, id);
    EXPECT_TRUE(connected);
    EXPECT_NE(id, 0u);
    EXPECT_EQ(server->current_stats().active_connections, 1u);
}

INSTANTIATE_TEST_SUITE_P(QuicAdmission, Refusal, ::testing::Values(0, 1, 2));

TEST(QuicHandshakeNativeBackend, RefusalDropsMalformedInputAndBoundsCoalescedOutput) {
    ASSERT_TRUE(require_ssl_files());
    qb::io::quic::tls_config tls;
    tls.certificate_file = ssl_resource_path("cert.pem");
    tls.private_key_file = ssl_resource_path("key.pem");
    auto server          = qb::io::quic::make_native_backend();
    server->start_server(qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, tls);
    ASSERT_TRUE(server->set_accepting_connections(false));
    tls.verify_peer = false;
    tls.server_name = "localhost";
    auto client     = qb::io::quic::make_native_backend();
    client->start_client(qb::io::endpoint{"127.0.0.1", 54321}, qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, tls);
    auto initial = client->drain_packets();
    ASSERT_FALSE(initial.empty());
    const auto &packet = initial.front();
    ASSERT_GE(packet.payload.size(), 1200u);
    ngtcp2_pkt_hd hd{};
    const auto    header_size =
        ngtcp2_pkt_decode_hd_long(&hd, reinterpret_cast<const std::uint8_t *>(packet.payload.data()), packet.payload.size());
    ASSERT_GT(header_size, 0);
    ASSERT_EQ(hd.type, NGTCP2_PKT_INITIAL);
    // The decoder stops BEFORE the protected packet number. Length is a varint, not necessarily two bytes
    // (ngtcp2 currently reserves four). Locate it from the preceding CID and token fields instead of backwards.
    const auto token_offset = std::size_t{7} + hd.dcid.datalen + hd.scid.datalen;
    ASSERT_LT(token_offset, static_cast<std::size_t>(header_size));
    const auto token_length_width = std::size_t{1} << (std::to_integer<unsigned>(packet.payload[token_offset]) >> 6);
    const auto length_offset      = token_offset + token_length_width + hd.tokenlen;
    ASSERT_LT(length_offset, static_cast<std::size_t>(header_size));
    const auto length_width = std::size_t{1} << (std::to_integer<unsigned>(packet.payload[length_offset]) >> 6);
    ASSERT_EQ(length_offset + length_width, static_cast<std::size_t>(header_size));
    const auto with_length = [&](std::array<std::byte, 8> const &encoded) {
        std::vector<std::byte> changed(packet.payload.begin(), packet.payload.begin() + length_offset);
        changed.insert(changed.end(), encoded.begin(), encoded.end());
        changed.insert(changed.end(), packet.payload.begin() + header_size, packet.payload.end());
        return changed;
    };
    std::vector<std::vector<std::byte>> malformed;
    malformed.emplace_back(packet.payload.begin(), packet.payload.begin() + 1199);
    std::array<std::byte, 8> encoded_length;
    encoded_length.fill(std::byte{0xff}); // maximum QUIC varint: declared Length exceeds the complete datagram
    malformed.push_back(with_length(encoded_length));
    encoded_length.fill(std::byte{0});
    encoded_length.front() = std::byte{0xc0}; // eight-byte encoding keeps the datagram at least 1200 bytes
    encoded_length.back()  = std::byte{0x01}; // declared Length has no room for the packet-number sample
    malformed.push_back(with_length(encoded_length));
    for (std::size_t i = 1; i <= 2; ++i) {
        SCOPED_TRACE(i);
        ngtcp2_pkt_hd mutated{};
        const auto   *bytes = reinterpret_cast<const std::uint8_t *>(malformed[i].data());
        ASSERT_GE(malformed[i].size(), 1200u);
        ASSERT_EQ(ngtcp2_accept(&mutated, bytes, malformed[i].size()), 0)
            << "the targeted Length defect must reach the additional refusal validation";
        ASSERT_EQ(ngtcp2_pkt_decode_hd_long(&mutated, bytes, malformed[i].size()), static_cast<ngtcp2_ssize>(length_offset + 8));
        if (i == 1) {
            ASSERT_GT(mutated.len, malformed[i].size() - length_offset - 8);
        } else {
            ASSERT_EQ(mutated.len, 1u);
        }
    }
    malformed.push_back(packet.payload);
    std::fill(malformed.back().begin() + 1, malformed.back().begin() + 5, std::byte{0xff}); // unsupported version
    malformed.emplace_back(1200, std::byte{0});                                             // unknown short-header packet
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        SCOPED_TRACE(i);
        server->on_udp_datagram({packet.remote, packet.local, malformed[i]});
        EXPECT_FALSE(server->wants_write());
        EXPECT_TRUE(server->drain_packets().empty());
        EXPECT_TRUE(server->drain_events().empty());
        EXPECT_EQ(server->current_stats().active_connections, 0u);
    }
    auto coalesced = packet.payload;
    coalesced.insert(coalesced.end(), packet.payload.begin(), packet.payload.end());
    server->on_udp_datagram({packet.remote, packet.local, coalesced});
    const auto response = server->drain_packets();
    ASSERT_EQ(response.size(), 1u) << "one received datagram funds at most one stateless refusal";
    EXPECT_LE(response.front().payload.size(), coalesced.size());
    EXPECT_EQ(server->current_stats().active_connections, 0u);
}

TEST(QuicHandshakeNativeBackend, StatelessRetryChallengesBeforeAllocatingConnection) {
    ASSERT_TRUE(require_ssl_files());

    qb::io::quic::settings settings;
    settings.enable_stateless_retry = true; // the default, made explicit

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();
    server->configure(settings);
    client->configure(settings);

    const qb::io::endpoint   server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint   client_endpoint{"127.0.0.1", 54321};
    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);
    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    // First (untokened) Initial reaches the server: it answers with a Retry and allocates nothing.
    ASSERT_TRUE(client->wants_write());
    const auto initial = deliver_ready_quic_packets(*client, *server);
    EXPECT_TRUE(initial.drained);
    EXPECT_EQ(server->current_stats().active_connections, 0u)
        << "a Retry-enabled server must not allocate a connection for an unvalidated (untokened) Initial";

    // Retry is parent-owned encoded control output: no child is needed for readiness.
    ASSERT_TRUE(server->wants_write());
    const auto retry = deliver_ready_quic_packets(*server, *client);
    EXPECT_TRUE(retry.drained);
    EXPECT_FALSE(retry.writable_after) << "consumed control output must not stay writable";
    EXPECT_EQ(retry.expiry_after, server->next_timeout());
    EXPECT_EQ(retry.expiry_after, std::chrono::steady_clock::time_point::max());

    bool          client_connected     = false;
    std::uint64_t server_connection_id = 0;
    drive_two_backend_handshake(*client, *server, client_connected, server_connection_id);
    EXPECT_TRUE(client_connected) << "the Retry round-trip must complete the handshake";
    EXPECT_NE(server_connection_id, 0u);
    EXPECT_EQ(server->current_stats().active_connections, 1u);
}

#ifdef QB_IO_QUIC_TEST_HOOKS
/**
 * @test A CSPRNG failure while generating the Retry secret fails closed, then recovers
 * @brief The listener's Retry secret keys every token it mints and verifies. With the first CSPRNG fill
 *        failing, the first Initial datagram must leave the secret NOT ready (never a zeroed key
 *        cached as valid), produce no Retry and admit no connection. Once the CSPRNG works again, the
 *        same Initial meets a freshly generated, non-zero secret, a Retry goes out and the handshake
 *        completes (Huly QB-334: the zeroed secret was marked ready for the life of the listener).
 */
TEST(QuicHandshakeNativeBackend, StatelessRetrySecretFailureFailsClosedThenRecovers) {
    ASSERT_TRUE(require_ssl_files());

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();

    const qb::io::endpoint   server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint   client_endpoint{"127.0.0.1", 54321};
    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);
    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    ASSERT_TRUE(client->wants_write());
    const auto initial = client->drain_packets();
    ASSERT_FALSE(initial.empty());
    const auto deliver = [&](qb::io::quic::packet const &datagram) {
        server->on_udp_datagram(
            {datagram.remote, datagram.local, std::span<const std::byte>{datagram.payload.data(), datagram.payload.size()}});
    };
    EXPECT_FALSE(qb::io::quic::test::retry_secret_ready(*server));

    // The first fill of the first datagram is the secret's.
    qb::io::quic::test::fail_next_random_fills(1);
    deliver(initial.front());
    EXPECT_FALSE(qb::io::quic::test::retry_secret_ready(*server)) << "a failed CSPRNG fill must not leave a secret ready";
    EXPECT_FALSE(server->wants_write()) << "no Retry may be minted without a secret";
    EXPECT_TRUE(server->drain_packets().empty());
    EXPECT_EQ(server->current_stats().active_connections, 0u) << "an Initial must not be admitted unvalidated either";

    for (auto const &datagram : initial)
        deliver(datagram);
    EXPECT_TRUE(qb::io::quic::test::retry_secret_ready(*server));
    EXPECT_FALSE(qb::io::quic::test::retry_secret_zeroed(*server)) << "the recovered secret must be fresh CSPRNG output";
    ASSERT_TRUE(server->wants_write()) << "the recovered listener answers with a Retry";

    bool          client_connected     = false;
    std::uint64_t server_connection_id = 0;
    drive_two_backend_handshake(*client, *server, client_connected, server_connection_id);
    EXPECT_TRUE(client_connected) << "the Retry round-trip with the recovered secret must complete the handshake";
    EXPECT_NE(server_connection_id, 0u);
}
#endif

TEST(QuicHandshakeNativeBackend, DisabledStatelessRetryAcceptsInitialWithoutChallenge) {
    ASSERT_TRUE(require_ssl_files());

    qb::io::quic::settings settings;
    settings.enable_stateless_retry = false;

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();
    server->configure(settings);
    client->configure(settings);

    const qb::io::endpoint   server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint   client_endpoint{"127.0.0.1", 54321};
    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);
    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    // Retry disabled: the first Initial is accepted directly — a connection exists immediately.
    const auto before_read = server->current_stats();
    ASSERT_TRUE(client->wants_write());
    const auto initial = deliver_ready_quic_packets(*client, *server);
    EXPECT_TRUE(initial.drained);
    const auto after_read = server->current_stats();
    EXPECT_GT(after_read.packets_received, before_read.packets_received);
    EXPECT_GT(after_read.bytes_received, before_read.bytes_received);
    EXPECT_EQ(server->current_stats().active_connections, 1u)
        << "with Retry disabled the server accepts the first Initial without a challenge round-trip";

    // One inbound Initial must expose a control-only writer opportunity.
    // Observe the encoded TX counters again after packet drainage.
    ASSERT_TRUE(server->wants_write()) << "Initial control output is invisible to a readiness-gated driver";
    const auto control = deliver_ready_quic_packets(*server, *client);
    EXPECT_TRUE(control.drained);
    EXPECT_FALSE(control.writable_after) << "one consumed read must not leave an always-writable source";
    EXPECT_GT(control.after.packets_sent, 0u) << "encoded TX at read/drain=" << std::to_string(after_read.packets_sent) << "/"
                                              << std::to_string(control.after.packets_sent);
    EXPECT_EQ(control.after.packets_received, after_read.packets_received);
    EXPECT_EQ(control.after.bytes_received, after_read.bytes_received);
    EXPECT_EQ(control.expiry_after, server->next_timeout())
        << "expiry before/after ticks=" << std::to_string(control.expiry_before.time_since_epoch().count()) << "/"
        << std::to_string(control.expiry_after.time_since_epoch().count());
    EXPECT_NE(control.expiry_after, std::chrono::steady_clock::time_point::max());

    bool          client_connected     = false;
    std::uint64_t server_connection_id = 0;
    drive_two_backend_handshake(*client, *server, client_connected, server_connection_id);
    EXPECT_TRUE(client_connected);
    EXPECT_EQ(server->current_stats().active_connections, 1u);
}

// =============================================================================
// REAL LOOPBACK HANDSHAKES (event loop + UDP socket + TLS)
// =============================================================================

namespace {

// A server stream-session that records inbound text and can echo on demand — used for the live tests.
class HandshakeStreamSession : public qb::io::use<HandshakeStreamSession>::quic::session {
public:
    using Base = qb::io::use<HandshakeStreamSession>::quic::session;
    using Base::Base;
};

// Bring up a server on an ephemeral port and a client connected to it; pump (loudly bounded) until both
// endpoints reach `connected`. ALPN defaults to {"h3"}.
template <typename Server, typename Client>
void
establish_loopback(Server &server, Client &client, std::vector<std::string> const &alpn = {"h3"}) {
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"), alpn));
    ASSERT_GT(server.local_endpoint().port(), 0);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    const auto uri = std::string{"quic://127.0.0.1:"} + std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(client.connect(qb::io::uri{uri}, client_tls, alpn));

    ASSERT_TRUE(pump_until(
        [&] {
            return server.current_state() == qb::io::async::quic::endpoint::state::connected
                   && client.current_state() == qb::io::async::quic::endpoint::state::connected;
        },
        std::chrono::seconds(5)))
        << "QUIC loopback handshake did not complete";
}

} // namespace

/**
 * @test A native client and server complete a real loopback handshake and exchange a stream + datagram
 * @brief Both endpoints reach `connected`; a client stream "ping" arrives server-side and a "capsule"
 *        datagram is received; the datagram stats are non-zero on both ends.
 */
TEST(QuicHandshakeLoopback, ClientAndServerCompleteHandshakeAndExchangeData) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    qb::io::quic::settings settings;
    settings.enable_datagrams        = true;
    settings.max_datagram_frame_size = 1200;

    CallbackQuicServer server;
    server.set_settings(settings);
    qb::io::async::quic::endpoint client;
    client.set_settings(settings);

    establish_loopback(server, client);

    EXPECT_EQ(server.stats().active_connections, 1u);
    EXPECT_EQ(client.stats().active_connections, 1u);
    EXPECT_EQ(server.connected, 1);

    auto stream = client.open_bidirectional_stream();
    client.send_stream_data(stream.id(), "ping", true);

    EXPECT_TRUE(pump_until([&] { return server.received == "ping"; }, std::chrono::seconds(5))) << "server never received the stream payload";
    EXPECT_GE(server.stream_started, 1);

    client.send_datagram("capsule");
    EXPECT_TRUE(pump_until([&] { return server.datagram_received == "capsule"; }, std::chrono::seconds(5)))
        << "server never received the datagram";
    EXPECT_GE(client.stats().datagrams_sent, 1u);
    EXPECT_GE(server.stats().datagrams_received, 1u);

    client.close();
    server.close();
    qb::io::async::listener::current.clear();
}

/**
 * @test An ALPN mismatch never establishes a connection
 * @brief A server advertising {"custom-quic"} and a client offering {"h3"} drives the client to `closed`
 *        and the server never observes `connected`.
 */
TEST(QuicHandshakeLoopback, AlpnMismatchDoesNotEstablishConnection) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    CallbackQuicServer server;
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"), {"custom-quic"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    CallbackQuicClient client;
    const auto         uri = std::string{"quic://127.0.0.1:"} + std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(client.connect(qb::io::uri{uri}, client_tls, {"h3"}));

    EXPECT_TRUE(pump_until([&] { return client.current_state() == qb::io::async::quic::endpoint::state::closed; }, std::chrono::seconds(5)))
        << "the ALPN-mismatched client should be driven to closed";
    EXPECT_EQ(client.current_state(), qb::io::async::quic::endpoint::state::closed);
    EXPECT_EQ(server.connected, 0);

    client.close();
    server.close();
    qb::io::async::listener::current.clear();
}

/**
 * @test One server demultiplexes two clients by connection id
 * @brief Two clients connect to one server; both reach `connected` and the server sees two connections.
 *        Each client sends a distinct payload and the server receives both. Closing one client keeps the
 *        listener open with exactly one remaining active connection and the other client still connected.
 */
TEST(QuicHandshakeLoopback, ServerRoutesMultipleClientsByConnectionId) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    CallbackQuicServer server;
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"), {"h3"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    CallbackQuicClient first;
    CallbackQuicClient second;
    const auto         uri = std::string{"quic://127.0.0.1:"} + std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(first.connect(qb::io::uri{uri}, client_tls, {"h3"}));
    ASSERT_TRUE(second.connect(qb::io::uri{uri}, client_tls, {"h3"}));

    EXPECT_TRUE(pump_until(
        [&] {
            return first.current_state() == qb::io::async::quic::endpoint::state::connected
                   && second.current_state() == qb::io::async::quic::endpoint::state::connected && server.connected >= 2;
        },
        std::chrono::seconds(5)))
        << "both clients should connect";
    EXPECT_EQ(server.connected, 2);
    EXPECT_EQ(server.stats().active_connections, 2u);

    auto first_stream  = first.open_bidirectional_stream();
    auto second_stream = second.open_bidirectional_stream();
    first.send_stream_data(first_stream.id(), "first-client\n", true);
    second.send_stream_data(second_stream.id(), "second-client\n", true);

    EXPECT_TRUE(pump_until(
        [&] { return server.received.find("first-client") != std::string::npos && server.received.find("second-client") != std::string::npos; },
        std::chrono::seconds(5)))
        << "server should demux both client payloads";

    first.close();
    EXPECT_TRUE(pump_until([&] { return server.is_open() && server.stats().active_connections == 1u; }, std::chrono::seconds(5)))
        << "closing one client should leave the listener open with one connection";
    EXPECT_TRUE(server.is_open());
    EXPECT_EQ(server.stats().active_connections, 1u);
    EXPECT_NE(server.current_state(), qb::io::async::quic::endpoint::state::closed);
    EXPECT_EQ(second.current_state(), qb::io::async::quic::endpoint::state::connected);

    second.close();
    server.close();
    qb::io::async::listener::current.clear();
}

/**
 * @test Closing one connection must not disturb how the server routes the survivors.
 * @brief The parent retires a closed connection's entries from `_server_cid_index` by the key set
 *        the child records (its reconciled SCIDs plus the accept path's local/original CID keys)
 *        rather than by sweeping the index for its id — the sweep costs O(active connections) per
 *        close, which is a permanent tax under churn and a multi-second stall when many
 *        connections close at once. The routing hazard of a keyed retirement is dropping one entry
 *        too many, and the case above never checks it: it asserts the survivor is still `connected`
 *        but never makes it speak again, and a connection whose CIDs were evicted stays `connected`
 *        while its datagrams silently miss the index and get mis-accepted as brand-new. Here the
 *        two survivors each send NEW payloads AFTER the middle connection is gone, so a
 *        misrouted CID fails instead of passing quietly.
 */
TEST(QuicHandshakeLoopback, ClosingOneConnectionLeavesTheSurvivorsRoutable) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    CallbackQuicServer server;
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"), {"h3"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    CallbackQuicClient left;
    CallbackQuicClient middle;
    CallbackQuicClient right;
    const auto         uri = std::string{"quic://127.0.0.1:"} + std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(left.connect(qb::io::uri{uri}, client_tls, {"h3"}));
    ASSERT_TRUE(middle.connect(qb::io::uri{uri}, client_tls, {"h3"}));
    ASSERT_TRUE(right.connect(qb::io::uri{uri}, client_tls, {"h3"}));

    ASSERT_TRUE(pump_until([&] { return server.connected >= 3 && server.stats().active_connections == 3u; }, std::chrono::seconds(5)))
        << "all three clients should connect";

    // Make every connection speak once, so each one owns index entries before the close.
    auto left_stream   = left.open_bidirectional_stream();
    auto middle_stream = middle.open_bidirectional_stream();
    auto right_stream  = right.open_bidirectional_stream();
    left.send_stream_data(left_stream.id(), "left-before\n", true);
    middle.send_stream_data(middle_stream.id(), "middle-before\n", true);
    right.send_stream_data(right_stream.id(), "right-before\n", true);

    ASSERT_TRUE(pump_until(
        [&] {
            return server.received.find("left-before") != std::string::npos && server.received.find("middle-before") != std::string::npos
                   && server.received.find("right-before") != std::string::npos;
        },
        std::chrono::seconds(5)))
        << "server should demux all three clients before the close";

    // Retire the middle connection: drain_events() runs the keyed retirement for its id.
    middle.close();
    ASSERT_TRUE(pump_until([&] { return server.stats().active_connections == 2u; }, std::chrono::seconds(5)))
        << "the middle connection should be retired, leaving two";

    // Two rounds of post-close traffic are needed, and both are load-bearing.
    //
    // `active_connections` subtracts the DEFERRED close list, so it reaches 2 while the retirement
    // is still only queued, and the retirement itself runs in drain_events() — which is driven by
    // traffic, not by wall time. So round 1 below is what actually flushes it: that datagram is
    // still routed through the pre-retirement index, and the retirement runs after it. Only round 2
    // exercises the index the retirement left behind.
    //
    // Both were verified against a retirement deliberately wiping every survivor entry from the
    // index: with a single round (or with a timed settle instead of round 1) this case stayed
    // GREEN through a total index wipe. It fails, as it should, only with round 2 present.
    auto left_settle = left.open_bidirectional_stream();
    left.send_stream_data(left_settle.id(), "left-settle\n", true);
    ASSERT_TRUE(pump_until([&] { return server.received.find("left-settle") != std::string::npos; }, std::chrono::seconds(5)))
        << "round 1 should reach the server and let drain_events() run the retirement";

    // The survivors must still route. New payloads, so nothing can be satisfied by what the
    // server already received above.
    auto left_after  = left.open_bidirectional_stream();
    auto right_after = right.open_bidirectional_stream();
    left.send_stream_data(left_after.id(), "left-after\n", true);
    right.send_stream_data(right_after.id(), "right-after\n", true);

    EXPECT_TRUE(pump_until(
        [&] { return server.received.find("left-after") != std::string::npos && server.received.find("right-after") != std::string::npos; },
        std::chrono::seconds(5)))
        << "after one connection closed, the survivors' datagrams no longer reach their sessions: "
           "retiring the closed connection's CID entries evicted index entries that still belonged "
           "to a live connection, so its packets miss the index and are treated as new connections";

    EXPECT_EQ(server.stats().active_connections, 2u) << "a misrouted survivor would be re-accepted as a fresh connection";
    EXPECT_EQ(left.current_state(), qb::io::async::quic::endpoint::state::connected);
    EXPECT_EQ(right.current_state(), qb::io::async::quic::endpoint::state::connected);

    left.close();
    right.close();
    server.close();
    qb::io::async::listener::current.clear();
}

enum class QuicServerStreamConnection { explicit_id, sole_peer };

class QuicServerInitiatedStreamTest : public ::testing::TestWithParam<QuicServerStreamConnection> {};

/**
 * @test A server-initiated stream is delivered to the client over the live loopback (ADDED — dossier §"Missing")
 * @brief After the handshake, the SERVER opens a bidirectional stream session on the connection and pushes
 *        "from-server\n"; the client (a connector carrying HandshakeStreamSession) registers the inbound
 *        stream and receives the bytes. The prior native tests only pushed client→server — this proves the
 *        reverse direction end-to-end. The session is opened with the connection id from `connected`, and
 *        with the default connection 0, which must address the sole peer by its real id (Huly QB-330: the
 *        session kept the id 0, and the native backend dropped its output).
 */
TEST_P(QuicServerInitiatedStreamTest, ReachesClient) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    // Server that can open a local stream session and push to it; client that records inbound stream bytes.
    struct ServerProbe : qb::io::async::quic::server<ServerProbe, HandshakeStreamSession> {
        int connected = 0;
        void
        on(qb::io::async::quic::event::connected const &ev) {
            ++connected;
            connection_id = ev.connection_id;
        }
        std::uint64_t connection_id = 0;
    };
    struct ClientProbe : qb::io::async::quic::connector<ClientProbe, HandshakeStreamSession> {
        std::string received;
        void
        on(qb::io::async::quic::event::stream_data const &ev) {
            received.append(ev.payload.data(), ev.payload.size());
        }
    };

    ServerProbe server;
    ClientProbe client;
    establish_loopback(server, client);

    ASSERT_TRUE(pump_until([&] { return server.connection_id != 0; }, std::chrono::seconds(5))) << "server never learned the connection id";

    auto *session = GetParam() == QuicServerStreamConnection::explicit_id ? server.open_bidirectional_stream_session(server.connection_id)
                                                                          : server.open_bidirectional_stream_session();
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->connection_id(), server.connection_id) << "the session must carry the real connection id";
    session->publish(std::string_view{"from-server\n", 12});
    server.flush_stream_session(*session);

    EXPECT_TRUE(pump_until([&] { return client.received.find("from-server") != std::string::npos; }, std::chrono::seconds(5)))
        << "client never received the server-initiated stream data";

    client.close();
    server.close();
    qb::io::async::listener::current.clear();
}

INSTANTIATE_TEST_SUITE_P(QuicHandshakeLoopback, QuicServerInitiatedStreamTest,
                         ::testing::Values(QuicServerStreamConnection::explicit_id, QuicServerStreamConnection::sole_peer),
                         [](::testing::TestParamInfo<QuicServerStreamConnection> const &info) {
                             return info.param == QuicServerStreamConnection::explicit_id ? std::string{"ExplicitConnection"}
                                                                                          : std::string{"SolePeerConnection"};
                         });

/**
 * @test A graceful server close is observed end-to-end with a typed disconnect reason (ADDED — dossier §"Missing")
 * @brief After the handshake the server closes the connection with an application error code; the client
 *        observes a connection_closed callback carrying disconnect_reason::application_close (the prior
 *        close tests only checked the listener stayed open, never the client-side reason).
 */
TEST(QuicHandshakeLoopback, GracefulCloseDeliversTypedDisconnectReasonToClient) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    CallbackQuicServer server;
    CallbackQuicClient client;
    establish_loopback(server, client);

    // The client opens a stream so the server learns a routable connection, then the client closes
    // gracefully with an application error code; the peer must observe the close.
    auto stream = client.open_bidirectional_stream();
    client.send_stream_data(stream.id(), "bye\n", true);
    EXPECT_TRUE(pump_until([&] { return server.received == "bye\n"; }, std::chrono::seconds(5)));

    client.close(0x1234, "client graceful close");

    EXPECT_TRUE(pump_until([&] { return server.closed >= 1; }, std::chrono::seconds(5))) << "server never observed the client's graceful close";
    EXPECT_NE(server.last_close_reason, qb::io::quic::disconnect_reason::none) << "the close must carry a typed disconnect reason";

    server.close();
    qb::io::async::listener::current.clear();
}

// =============================================================================
// ADDED COVERAGE — peer-side native callbacks driven by the two-backend pump
//
// The existing DirectBackendsRouteServerChildOperations test exercises the
// *originating* side of stream/datagram operations (server calls send/reset/stop
// and observes its own synthetic events) but never pumps the resulting wire
// frames to the peer, so the receiving-side ngtcp2 callbacks — recv_datagram_cb,
// acked_stream_data_offset_cb / stream_data_acked, ack_datagram_cb /
// release_inflight_datagram, stream_reset_cb, stream_stop_sending_cb — stayed
// uncovered. These tests hand-deliver packets in BOTH directions until those
// peer callbacks fire (or a loud deadline trips), exercising src/quic.cpp's
// receive-and-acknowledge machinery deterministically without an event loop.
// =============================================================================

namespace {

// Bring two native backends to `connected` by hand-delivering packets; collects
// the server child connection id observed on the server's `connected` event.
// Returns the server child connection id (0 == handshake did not complete).
std::uint64_t
direct_handshake(qb::io::quic::backend &client, qb::io::quic::backend &server, bool &client_connected) {
    std::uint64_t server_connection_id = 0;
    const auto    deadline             = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_connected || server_connection_id == 0) && std::chrono::steady_clock::now() < deadline) {
        deliver_ready_quic_packets(client, server);
        deliver_ready_quic_packets(server, client);
        for (auto const &event : client.drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connected)
                client_connected = true;
        for (auto const &event : server.drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connected)
                server_connection_id = event.connection_id;
    }
    return server_connection_id;
}

void
deliver_quota_packets(qb::io::quic::backend &client, qb::io::quic::backend &server) {
    deliver_ready_quic_packets(client, server);
    deliver_ready_quic_packets(server, client);
}

bool
wait_for_quota_event(qb::io::quic::backend &client, qb::io::quic::backend &server, qb::io::quic::backend_event::kind kind,
                     std::uint64_t stream_id, bool require_close_control = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        deliver_ready_quic_packets(client, server);
        bool found = false;
        for (auto const &event : server.drain_events())
            if (event.type == kind && event.stream_id == stream_id)
                found = true;
        // Only the full-close quota owner requests this witness; reset/stop
        // notifications in neighbouring cases are not treated as quota credit.
        if (found && require_close_control) {
            EXPECT_TRUE(server.wants_write()) << "full close credit is hidden before its existing final drain";
        }
        const auto control = deliver_ready_quic_packets(server, client);
        (void) client.drain_events();
        if (found) {
            if (require_close_control) {
                EXPECT_TRUE(control.drained);
                EXPECT_FALSE(control.writable_after) << "MAX_STREAMS/control consumption left readiness sticky";
                EXPECT_EQ(control.expiry_after, server.next_timeout())
                    << "expiry before/after ticks=" << std::to_string(control.expiry_before.time_since_epoch().count()) << "/"
                    << std::to_string(control.expiry_after.time_since_epoch().count());
            }
            return true;
        }
    }
    return false;
}

} // namespace

TEST(QuicHandshakeNativeBackend, RemoteStreamQuotaRenewsForBothDirectionsAtOneSlot) {
    ASSERT_TRUE(require_ssl_files());

    for (auto direction : {qb::io::quic::stream_direction::unidirectional, qb::io::quic::stream_direction::bidirectional}) {
        auto                   server = qb::io::quic::make_native_backend();
        auto                   client = qb::io::quic::make_native_backend();
        qb::io::quic::settings limits;
        limits.max_streams_uni  = 1;
        limits.max_streams_bidi = 1;
        server->configure(limits);

        qb::io::quic::tls_config server_tls;
        server_tls.certificate_file = ssl_resource_path("cert.pem");
        server_tls.private_key_file = ssl_resource_path("key.pem");
        server->start_server(qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, server_tls);
        qb::io::quic::tls_config client_tls;
        client_tls.server_name = "localhost";
        client_tls.verify_peer = false;
        client->start_client(qb::io::endpoint{"127.0.0.1", 54321}, qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, client_tls);
        bool       client_connected     = false;
        const auto server_connection_id = direct_handshake(*client, *server, client_connected);
        ASSERT_TRUE(client_connected);
        ASSERT_NE(server_connection_id, 0u);

        for (int i = 0; i < 4; ++i) {
            std::uint64_t stream_id = 0;
            bool          opened    = false;
            const auto    deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!opened && std::chrono::steady_clock::now() < deadline) {
                deliver_quota_packets(*client, *server);
                try {
                    stream_id = client->open_stream(0, direction);
                    opened    = true;
                } catch (std::runtime_error const &) {
                    (void) client->drain_events();
                    (void) server->drain_events();
                }
            }
            ASSERT_TRUE(opened) << "remote stream quota was not renewed at slot " << i;
            EXPECT_THROW((void) client->open_stream(0, direction), std::runtime_error)
                << "the single advertised slot must be exhausted before its full close";
            client->send_stream_data(0, stream_id, std::span<const std::byte>{}, true);
            if (direction == qb::io::quic::stream_direction::bidirectional) {
                ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_started, stream_id));
                server->send_stream_data(server_connection_id, stream_id, std::span<const std::byte>{}, true);
            }
            ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_closed, stream_id, true));
        }
    }
}

TEST(QuicHandshakeNativeBackend, HigherRemoteStreamIdBeforeLowerRenewsExactlyTwoSlots) {
    ASSERT_TRUE(require_ssl_files());
    auto                   server = qb::io::quic::make_native_backend();
    auto                   client = qb::io::quic::make_native_backend();
    qb::io::quic::settings limits;
    limits.max_streams_uni = 2;
    server->configure(limits);
    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, server_tls);
    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(qb::io::endpoint{"127.0.0.1", 54321}, qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, client_tls);
    bool client_connected = false;
    ASSERT_NE(direct_handshake(*client, *server, client_connected), 0u);
    ASSERT_TRUE(client_connected);

    const auto first  = client->open_stream(0, qb::io::quic::stream_direction::unidirectional);
    const auto second = client->open_stream(0, qb::io::quic::stream_direction::unidirectional);
    ASSERT_EQ(second, first + 4);
    // The higher ID reaches the peer first. The lower ID is materialized only
    // when its own data arrives; it must then trigger stream_open before close.
    client->send_stream_data(0, second, std::span<const std::byte>{}, true);
    ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_closed, second));
    constexpr std::array<std::byte, 1> lower_payload{std::byte{0x61}};
    client->send_stream_data(0, first, lower_payload, false);
    bool       lower_started = false;
    bool       lower_closed  = false;
    const auto deadline      = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!lower_started && std::chrono::steady_clock::now() < deadline) {
        deliver_quota_packets(*client, *server);
        for (auto const &event : server->drain_events()) {
            if (event.stream_id != first)
                continue;
            lower_started |= event.type == qb::io::quic::backend_event::kind::stream_started;
            lower_closed |= event.type == qb::io::quic::backend_event::kind::stream_closed;
        }
    }
    ASSERT_TRUE(lower_started) << "referencing the lower ID must invoke stream_open";
    EXPECT_FALSE(lower_closed) << "the lower stream remains open until its FIN";
    client->send_stream_data(0, first, std::span<const std::byte>{}, true);
    ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_closed, first));
    for (int i = 0; i < 4; ++i)
        deliver_quota_packets(*client, *server);
    EXPECT_NO_THROW((void) client->open_stream(0, qb::io::quic::stream_direction::unidirectional));
    EXPECT_NO_THROW((void) client->open_stream(0, qb::io::quic::stream_direction::unidirectional));
    EXPECT_THROW((void) client->open_stream(0, qb::io::quic::stream_direction::unidirectional), std::runtime_error)
        << "both streams invoked stream_open, so their closes must renew exactly two slots";
}

TEST(QuicHandshakeNativeBackend, RemoteStreamQuotaRenewsAfterResetAndStop) {
    ASSERT_TRUE(require_ssl_files());
    auto                   server = qb::io::quic::make_native_backend();
    auto                   client = qb::io::quic::make_native_backend();
    qb::io::quic::settings limits;
    limits.max_streams_uni = 1;
    server->configure(limits);
    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, server_tls);
    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(qb::io::endpoint{"127.0.0.1", 54321}, qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, client_tls);
    bool       client_connected     = false;
    const auto server_connection_id = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);

    constexpr std::array<std::byte, 1> payload{std::byte{0x61}};
    const auto                         first = client->open_stream(0, qb::io::quic::stream_direction::unidirectional);
    client->send_stream_data(0, first, payload, false);
    ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_started, first));
    client->reset_stream(0, first, 0x123);
    // Native mutations require the explicit packet drain used by endpoint wrappers.
    deliver_quic_packets(*client, *server);
    ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_closed, first));
    for (int i = 0; i < 4; ++i)
        deliver_quota_packets(*client, *server);
    const auto second = client->open_stream(0, qb::io::quic::stream_direction::unidirectional);
    client->send_stream_data(0, second, payload, false);
    ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_started, second));
    server->stop_stream(server_connection_id, second, 0x456);
    deliver_quic_packets(*server, *client);
    ASSERT_TRUE(wait_for_quota_event(*client, *server, qb::io::quic::backend_event::kind::stream_closed, second));
    for (int i = 0; i < 4; ++i)
        deliver_quota_packets(*client, *server);
    EXPECT_NO_THROW((void) client->open_stream(0, qb::io::quic::stream_direction::unidirectional));
}

/**
 * @test The peer ACKs server stream data, driving acked_stream_data_offset_cb / stream_data_acked
 * @brief After a direct handshake the server sends a bidirectional stream payload; pumping in both
 *        directions delivers the bytes to the client (recv_stream_data_cb -> stream_data event) and
 *        carries the client's ACK back so the server fires acked_stream_data_offset_cb, emitting a
 *        stream_data_acked event whose byte count matches the sent payload. Covers quic.cpp:1591-1617.
 */
TEST(QuicHandshakeNativeBackend, ServerStreamDataIsAckedByPeer) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();

    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint client_endpoint{"127.0.0.1", 54321};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    bool client_connected     = false;
    auto server_connection_id = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);

    const std::string message = "acked-stream-payload";
    const auto        stream  = server->open_stream(server_connection_id, qb::io::quic::stream_direction::bidirectional);
    server->send_stream_data(server_connection_id, stream,
                             std::span<const std::byte>{reinterpret_cast<const std::byte *>(message.data()), message.size()}, true);

    bool          client_got_data = false;
    bool          server_got_ack  = false;
    std::uint64_t acked_bytes     = 0;
    const auto    deadline        = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_got_data || !server_got_ack) && std::chrono::steady_clock::now() < deadline) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);
        for (auto const &event : client->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_data && !event.payload.empty())
                client_got_data = true;
        }
        for (auto const &event : server->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_data_acked) {
                server_got_ack = true;
                acked_bytes += event.error_code; // src/quic.cpp packs the acked datalen into error_code
            }
        }
    }

    EXPECT_TRUE(client_got_data) << "client never received the server's stream payload";
    ASSERT_TRUE(server_got_ack) << "server never observed the peer ACK (acked_stream_data_offset_cb did not fire)";
    // ACKs may coalesce or arrive in pieces; the cumulative acked datalen must total the sent payload.
    EXPECT_EQ(acked_bytes, message.size());
}

/**
 * @test The peer ACKs a server DATAGRAM, driving ack_datagram_cb / release_inflight_datagram(acked)
 * @brief With DATAGRAM enabled on both ends, the server sends a datagram after the handshake; pumping
 *        delivers it to the client (recv_datagram_cb increments datagrams_received) and carries the
 *        ACK back so the server's ack_datagram_cb runs release_inflight_datagram(acked=true),
 *        incrementing datagrams_acked. Covers quic.cpp:1560-1573 (acked branch) + 1551-1556.
 */
TEST(QuicHandshakeNativeBackend, ServerDatagramIsAckedByPeer) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::quic::settings settings;
    settings.enable_datagrams        = true;
    settings.max_datagram_frame_size = 1200;

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();
    server->configure(settings);
    client->configure(settings);

    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint client_endpoint{"127.0.0.1", 54321};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    bool client_connected     = false;
    auto server_connection_id = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);

    auto payload = quic_payload();
    server->send_datagram(server_connection_id, std::span<const std::byte>{payload});

    bool       client_got_datagram = false;
    const auto deadline            = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_got_datagram || server->current_stats().datagrams_acked == 0) && std::chrono::steady_clock::now() < deadline) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);
        for (auto const &event : client->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::datagram)
                client_got_datagram = true;
        }
        (void) server->drain_events();
    }

    EXPECT_TRUE(client_got_datagram) << "client never received the server datagram";
    EXPECT_GE(client->current_stats().datagrams_received, 1u);
    EXPECT_GE(server->current_stats().datagrams_acked, 1u) << "server never saw the datagram ACK (ack_datagram_cb did not fire)";
}

enum class QuicTxCredit { stream, connection };

class QuicCreditBlockedHeadTest : public ::testing::TestWithParam<QuicTxCredit> {};

/**
 * @test A head stream without flow-control credit holds back neither other work nor the writer
 * @brief The server advertises 16 bytes of credit: per stream, or for the whole connection. The client
 *        queues 64 bytes on a head stream, then 6 bytes on a second stream and a DATAGRAM. The head sends
 *        its 16 bytes and stops; the DATAGRAM (never flow controlled) must still arrive, and with
 *        per-stream credit so must the second stream's bytes. Once only refused data is left, the client
 *        must not report itself writable: a writer attempt would produce nothing until credit arrives,
 *        and an endpoint would keep EV_WRITE armed on it. When the server grants credit, the rest of
 *        the head and the second stream follow (Huly QB-333: the writer only ever tried the head chunk,
 *        broke out on its refusal, and wants_write() stayed true while any data was queued).
 */
TEST_P(QuicCreditBlockedHeadTest, BlockedHeadLeavesOtherWorkSendableAndTheWriterIdle) {
    ASSERT_TRUE(require_ssl_files());

    const bool             per_stream = GetParam() == QuicTxCredit::stream;
    qb::io::quic::settings server_settings;
    qb::io::quic::settings client_settings;
    server_settings.enable_datagrams = client_settings.enable_datagrams = true;
    server_settings.max_datagram_frame_size = client_settings.max_datagram_frame_size = 1200;
    if (per_stream)
        server_settings.max_stream_data_bidi_remote = 16;
    else
        server_settings.connection_recv_window = 16;

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();
    server->configure(server_settings);
    client->configure(client_settings);
    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, server_tls);
    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(qb::io::endpoint{"127.0.0.1", 54321}, qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, client_tls);
    bool       client_connected     = false;
    const auto server_connection_id = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);

    const auto bytes = [](std::string const &text) {
        return std::span<const std::byte>{reinterpret_cast<const std::byte *>(text.data()), text.size()};
    };
    const std::string head_payload(64, 'h');
    const std::string other_payload = "other!";
    const auto        head          = client->open_stream(0, qb::io::quic::stream_direction::bidirectional);
    client->send_stream_data(0, head, bytes(head_payload), false);
    const auto other = client->open_stream(0, qb::io::quic::stream_direction::bidirectional);
    client->send_stream_data(0, other, bytes(other_payload), false);
    client->send_datagram(0, bytes("dgram"));

    std::size_t head_bytes  = 0;
    std::size_t other_bytes = 0;
    std::size_t stray_bytes = 0;
    std::string datagrams;
    const auto  pump = [&](auto &&done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            deliver_ready_quic_packets(*client, *server);
            deliver_ready_quic_packets(*server, *client);
            (void) client->drain_events();
            for (auto const &event : server->drain_events()) {
                if (event.type == qb::io::quic::backend_event::kind::stream_data)
                    (event.stream_id == head ? head_bytes : event.stream_id == other ? other_bytes : stray_bytes) += event.payload.size();
                else if (event.type == qb::io::quic::backend_event::kind::datagram)
                    datagrams.append(reinterpret_cast<const char *>(event.payload.data()), event.payload.size());
            }
        }
    };

    pump([&] { return head_bytes == 16 && datagrams == "dgram" && (!per_stream || other_bytes == other_payload.size()); });
    EXPECT_EQ(head_bytes, 16u) << "the head sends exactly its credit";
    EXPECT_EQ(datagrams, "dgram") << "a DATAGRAM must not wait behind a stream without credit";
    EXPECT_EQ(other_bytes, per_stream ? other_payload.size() : 0u) << "another stream with credit must not wait behind the head";

    // Drain the attempt the last read made due: then only refused data is left, and only a read, a
    // credit grant or the SDK timer (next_timeout) can make another attempt due.
    deliver_quic_packets(*client, *server);
    EXPECT_FALSE(client->wants_write()) << "data refused for lack of credit must not keep the writer due";

    server->extend_stream_credit(server_connection_id, head, head_payload.size());
    pump([&] { return head_bytes == head_payload.size() && other_bytes == other_payload.size(); });
    EXPECT_EQ(head_bytes, head_payload.size()) << "credit must resume the head stream";
    EXPECT_EQ(other_bytes, other_payload.size());
    EXPECT_EQ(stray_bytes, 0u);
}

INSTANTIATE_TEST_SUITE_P(QuicHandshakeNativeBackend, QuicCreditBlockedHeadTest,
                         ::testing::Values(QuicTxCredit::stream, QuicTxCredit::connection),
                         [](::testing::TestParamInfo<QuicTxCredit> const &info) {
                             return info.param == QuicTxCredit::stream ? std::string{"StreamCredit"} : std::string{"ConnectionCredit"};
                         });

namespace quic_handshake_credit_test {

// A connected native client/server pair whose server grants `stream_window` bytes per client stream and
// `connection_window` bytes per connection, with a pump that collects the server's bytes per stream.
struct CreditPair {
    std::unique_ptr<qb::io::quic::backend>   server        = qb::io::quic::make_native_backend();
    std::unique_ptr<qb::io::quic::backend>   client        = qb::io::quic::make_native_backend();
    std::uint64_t                            connection_id = 0;
    std::map<std::uint64_t, std::string>     received;
    std::vector<qb::io::quic::backend_event> client_events;

    CreditPair(std::uint64_t stream_window, std::uint64_t connection_window, qb::io::quic::settings client_settings = {}) {
        qb::io::quic::settings server_settings;
        server_settings.max_stream_data_bidi_remote = stream_window;
        server_settings.connection_recv_window      = connection_window;
        server->configure(server_settings);
        client->configure(client_settings);
        qb::io::quic::tls_config server_tls;
        server_tls.certificate_file = ssl_resource_path("cert.pem");
        server_tls.private_key_file = ssl_resource_path("key.pem");
        server->start_server(qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, server_tls);
        qb::io::quic::tls_config client_tls;
        client_tls.server_name = "localhost";
        client_tls.verify_peer = false;
        client->start_client(qb::io::endpoint{"127.0.0.1", 54321}, qb::io::endpoint{"127.0.0.1", 4433}, {"h3"}, client_tls);
        bool client_connected = false;
        connection_id         = direct_handshake(*client, *server, client_connected);
    }

    void
    send(std::uint64_t stream, std::string const &text) {
        client->send_stream_data(0, stream, std::span<const std::byte>{reinterpret_cast<const std::byte *>(text.data()), text.size()}, false);
    }

    void
    step() {
        deliver_ready_quic_packets(*client, *server);
        deliver_ready_quic_packets(*server, *client);
        for (auto &event : client->drain_events())
            client_events.push_back(std::move(event));
        for (auto const &event : server->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::stream_data)
                received[event.stream_id].append(reinterpret_cast<const char *>(event.payload.data()), event.payload.size());
    }

    // Enough exchanges for anything sendable to arrive and be acknowledged.
    void
    rounds(int count) {
        for (int i = 0; i < count; ++i)
            step();
    }

    template <typename Done>
    bool
    pump(Done &&done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done() && std::chrono::steady_clock::now() < deadline)
            step();
        return done();
    }
};

} // namespace quic_handshake_credit_test

/**
 * @test A stream refused for its own credit with another stream queued behind it parks, in order
 * @brief The server grants 16 bytes per stream and 17 for the whole connection. A grant stream takes 1
 *        byte and the head 16, which spends both credits at once: the next attempt is refused for the
 *        CONNECTION and nothing is parked, so a second stream's 6 bytes queue behind the head. Crediting
 *        the grant stream returns connection credit only: the head is now refused for its OWN credit
 *        with the second stream behind it, so parking moves the head out from in front of it and the
 *        second stream's bytes go out at once. Bytes sent on the parked head wait behind its refused
 *        ones; once the head is credited, all of them arrive in order (Huly QB-333).
 */
TEST(QuicHandshakeNativeBackend, CreditBlockedStreamParksBehindQueuedWorkInOrder) {
    ASSERT_TRUE(require_ssl_files());
    quic_handshake_credit_test::CreditPair pair{16, 17};
    ASSERT_NE(pair.connection_id, 0u);
    auto &received = pair.received;

    const auto grant = pair.client->open_stream(0, qb::io::quic::stream_direction::bidirectional);
    pair.send(grant, "g");
    ASSERT_TRUE(pair.pump([&] { return received[grant] == "g"; }));

    const std::string head_payload(64, 'h');
    const auto        head = pair.client->open_stream(0, qb::io::quic::stream_direction::bidirectional);
    pair.send(head, head_payload);
    ASSERT_TRUE(pair.pump([&] { return received[head].size() == 16; })) << "16 bytes of stream (and connection) credit";
    const auto other = pair.client->open_stream(0, qb::io::quic::stream_direction::bidirectional);
    pair.send(other, "other!");
    pair.rounds(20);
    EXPECT_TRUE(received[other].empty()) << "no connection credit is left for the second stream";

    pair.server->extend_stream_credit(pair.connection_id, grant, 8);
    ASSERT_TRUE(pair.pump([&] { return received[other] == "other!"; }))
        << "the returned connection credit must reach the second stream past the head refused for its own credit";
    EXPECT_EQ(received[head].size(), 16u);

    pair.send(head, "TAIL");
    pair.rounds(20);
    EXPECT_EQ(received[head].size(), 16u) << "bytes sent on a parked stream wait behind its refused ones";

    pair.server->extend_stream_credit(pair.connection_id, head, head_payload.size());
    ASSERT_TRUE(pair.pump([&] { return received[head].size() == head_payload.size() + 4; }));
    EXPECT_EQ(received[head], head_payload + "TAIL") << "the parked stream must resume in order";
    for (auto const &event : pair.client_events)
        EXPECT_NE(event.type, qb::io::quic::backend_event::kind::connection_closed) << event.text;
}

/**
 * @test Resetting a stream parked for lack of credit releases its queued data from the send caps
 * @brief The client may queue 3 frames. A stream refused for its own credit is parked with its first
 *        chunk and two more; a reset must drop all three from the queue accounting, so three new frames
 *        on another stream are accepted (no buffer_overflow close) and delivered (Huly QB-333).
 */
TEST(QuicHandshakeNativeBackend, ResetOfACreditBlockedStreamReleasesItsQueuedData) {
    ASSERT_TRUE(require_ssl_files());
    qb::io::quic::settings client_settings;
    client_settings.max_pending_stream_frames = 3;
    quic_handshake_credit_test::CreditPair pair{16, 1024 * 1024, client_settings};
    ASSERT_NE(pair.connection_id, 0u);
    auto &received = pair.received;

    const auto head = pair.client->open_stream(0, qb::io::quic::stream_direction::bidirectional);
    pair.send(head, std::string(64, 'h'));
    ASSERT_TRUE(pair.pump([&] { return received[head].size() == 16; }));
    pair.send(head, "more");
    pair.send(head, "most");
    pair.client->reset_stream(0, head, 0x7);
    pair.rounds(20);

    const auto next = pair.client->open_stream(0, qb::io::quic::stream_direction::bidirectional);
    pair.send(next, "a");
    pair.send(next, "b");
    pair.send(next, "c");
    ASSERT_TRUE(pair.pump([&] { return received[next] == "abc"; })) << "the reset stream's frames must no longer count against the cap";
    for (auto const &event : pair.client_events)
        EXPECT_NE(event.connection_reason, qb::io::quic::disconnect_reason::buffer_overflow) << event.text;
    EXPECT_EQ(received[head].size(), 16u) << "nothing of the reset stream may be sent after the reset";
}

class QuicStreamResetTest : public ::testing::TestWithParam<std::uint64_t> {};

/**
 * @test The peer observes a RESET_STREAM frame, and both full closes keep it a reset
 * @brief The server opens a stream, sends data, then resets it; pumping carries the RESET_STREAM frame
 *        to the client whose stream_reset_cb fires and queues a stream_closed event with
 *        stream_close_reason::reset and the application error code. Pumping on until ngtcp2 closes the
 *        stream on BOTH peers, each stream_close_cb must report reset with that code and neither may
 *        synthesise a FIN nobody sent. Application error code 0 is a valid reset code: ngtcp2 flags it
 *        as set, and it must not read as a clean finish (Huly QB-335; the 0x42 case is the control).
 */
TEST_P(QuicStreamResetTest, PeerObservesStreamResetFromServer) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();

    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint client_endpoint{"127.0.0.1", 54321};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    bool client_connected     = false;
    auto server_connection_id = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);

    const std::string message = "to-be-reset";
    const auto        stream  = server->open_stream(server_connection_id, qb::io::quic::stream_direction::bidirectional);
    server->send_stream_data(server_connection_id, stream,
                             std::span<const std::byte>{reinterpret_cast<const std::byte *>(message.data()), message.size()}, false);

    // Let the stream be created on the peer before resetting it.
    bool       client_saw_stream = false;
    const auto open_deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!client_saw_stream && std::chrono::steady_clock::now() < open_deadline) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);
        for (auto const &event : client->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::stream_started || event.type == qb::io::quic::backend_event::kind::stream_data)
                client_saw_stream = true;
        (void) server->drain_events();
    }
    ASSERT_TRUE(client_saw_stream) << "peer never saw the server stream open";

    const std::uint64_t code = GetParam();
    server->reset_stream(server_connection_id, stream, code);

    // stream_reset_cb's event carries a text; stream_close_cb's, the full close, carries none.
    struct Observed {
        bool                              peer_reset      = false;
        std::uint64_t                     peer_reset_code = ~std::uint64_t{0};
        bool                              closed          = false;
        qb::io::quic::stream_close_reason close_reason    = qb::io::quic::stream_close_reason::none;
        std::uint64_t                     close_code      = ~std::uint64_t{0};
        int                               fins            = 0;
    };
    const auto observe = [stream](qb::io::quic::backend &backend, Observed &seen) {
        for (auto const &event : backend.drain_events()) {
            if (event.stream_id != stream)
                continue;
            if (event.type == qb::io::quic::backend_event::kind::stream_data && event.error_code == 1)
                ++seen.fins;
            if (event.type != qb::io::quic::backend_event::kind::stream_closed)
                continue;
            if (event.text == "stream reset by peer") {
                seen.peer_reset      = true;
                seen.peer_reset_code = event.error_code;
            } else if (event.text.empty()) {
                seen.closed       = true;
                seen.close_reason = event.stream_reason;
                seen.close_code   = event.error_code;
            }
        }
    };
    Observed   client_seen;
    Observed   server_seen;
    const auto reset_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_seen.closed || !server_seen.closed) && std::chrono::steady_clock::now() < reset_deadline) {
        deliver_ready_quic_packets(*server, *client);
        deliver_ready_quic_packets(*client, *server);
        observe(*client, client_seen);
        observe(*server, server_seen);
    }

    ASSERT_TRUE(client_seen.peer_reset) << "peer never observed the RESET_STREAM (stream_reset_cb did not fire)";
    EXPECT_EQ(client_seen.peer_reset_code, code);
    ASSERT_TRUE(client_seen.closed) << "the client never fully closed the reset stream";
    EXPECT_EQ(client_seen.close_reason, qb::io::quic::stream_close_reason::reset) << "a reset must not close as finished";
    EXPECT_EQ(client_seen.close_code, code);
    EXPECT_EQ(client_seen.fins, 0) << "the server never sent a FIN: none may be synthesised";
    ASSERT_TRUE(server_seen.closed) << "the server never fully closed the stream it reset";
    EXPECT_EQ(server_seen.close_reason, qb::io::quic::stream_close_reason::reset);
    EXPECT_EQ(server_seen.close_code, code);
    EXPECT_EQ(server_seen.fins, 0);
}

INSTANTIATE_TEST_SUITE_P(QuicHandshakeNativeBackend, QuicStreamResetTest, ::testing::Values(std::uint64_t{0x42}, std::uint64_t{0}),
                         [](::testing::TestParamInfo<std::uint64_t> const &info) {
                             return info.param == 0 ? std::string{"ApplicationCodeZero"} : std::string{"ApplicationCode0x42"};
                         });

/**
 * @test A malformed UDP datagram against a started parent server is rejected, not accepted
 * @brief The parent rejects this malformed datagram while decoding its connection ID/version in
 *        find_or_accept_server_connection, before creating a child. The test checks zero active
 *        connections and no connected event, then verifies that closing the parent still emits
 *        application_close. It does not require a child handshake_failed event.
 */
TEST(QuicHandshakeNativeBackend, MalformedInitialDatagramIsRejectedByServer) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    auto                     server = qb::io::quic::make_native_backend();
    const qb::io::endpoint   local{"127.0.0.1", 0};
    const qb::io::endpoint   remote{"127.0.0.1", 4433};
    qb::io::quic::tls_config tls;
    tls.certificate_file = ssl_resource_path("cert.pem");
    tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(local, {"h3"}, tls);

    // This 64-byte long-header-shaped payload carries an unsupported QUIC version.
    // The parent's CID/version checks reject it before any child connection or handshake is created.
    std::array<std::byte, 64> junk{};
    junk[0] = std::byte{0xC0}; // long header form bit set, but no valid QUIC initial follows
    for (std::size_t i = 1; i < junk.size(); ++i)
        junk[i] = static_cast<std::byte>(i);

    qb::io::quic::packet_view bad_initial{remote, local, std::span<const std::byte>{junk.data(), junk.size()}};
    server->on_udp_datagram(bad_initial);

    // The parent must not have admitted a connection, and the rejection surfaces no connected event.
    EXPECT_EQ(server->current_stats().active_connections, 0u);
    bool saw_connected = false;
    for (auto const &event : server->drain_events())
        if (event.type == qb::io::quic::backend_event::kind::connected)
            saw_connected = true;
    EXPECT_FALSE(saw_connected) << "a malformed initial must never produce a connected event";

    // The parent remains usable: a clean close still emits the typed application_close.
    server->close(7, "after rejecting junk");
    auto events = server->drain_events();
    auto closed = std::find_if(events.begin(), events.end(),
                               [](auto const &event) { return event.type == qb::io::quic::backend_event::kind::connection_closed; });
    ASSERT_NE(closed, events.end());
    EXPECT_EQ(closed->connection_reason, qb::io::quic::disconnect_reason::application_close);
    EXPECT_EQ(closed->error_code, 7u);
}

/**
 * @test The server observes its own STOP_SENDING when it stops reading a stream (stream_stop_sending_cb).
 * @brief The client opens a bidi stream and writes; the server stops reading it via stop_stream()
 *        (ngtcp2_conn_shutdown_stream_read). Covers quic.cpp:618-628 (the synchronous stop_sending
 *        event queued by stop_stream) AND quic.cpp:1689-1696 (stream_stop_sending_cb, fired by
 *        ngtcp2 when the local endpoint drains its transport after shutting down the read side).
 *
 * QUIC/ngtcp2 SEMANTICS — important correction over the prior version of this test, which expected
 * the CLIENT (the peer that RECEIVES the STOP_SENDING frame) to observe it via stream_stop_sending_cb:
 * that is impossible. Per the ngtcp2 docs, ngtcp2_stream_stop_sending "is invoked when a stream is no
 * longer read by a LOCAL endpoint before it receives all stream data" — i.e. it fires on the endpoint
 * that CALLS shutdown_stream_read (here the SERVER), NOT on the peer. The peer that receives a
 * STOP_SENDING frame gets no stream_stop_sending callback at all (verified empirically: the client
 * only ever sees stream_data_acked). So we assert the stop_sending observation on the SERVER side,
 * which is the only side where it can occur. Pumps are bounded; no unbounded waits.
 */
TEST(QuicHandshakeNativeBackend, ServerObservesStopSendingWhenItStopsReading) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();

    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint client_endpoint{"127.0.0.1", 54321};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    bool client_connected     = false;
    auto server_connection_id = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);

    // Client opens a stream and sends data so the server learns a routable stream id.
    const std::string message = "client-writes";
    const auto        stream  = client->open_stream(qb::io::quic::stream_direction::bidirectional);
    client->send_stream_data(0, stream, std::span<const std::byte>{reinterpret_cast<const std::byte *>(message.data()), message.size()}, false);

    std::uint64_t server_stream_id = 0;
    bool          server_saw_data  = false;
    const auto    open_deadline    = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server_saw_data && std::chrono::steady_clock::now() < open_deadline) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);
        for (auto const &event : server->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::stream_data && !event.payload.empty()) {
                server_saw_data  = true;
                server_stream_id = event.stream_id;
            }
        (void) client->drain_events();
    }
    ASSERT_TRUE(server_saw_data) << "server never received the client stream data";
    EXPECT_EQ(server_stream_id, stream);

    constexpr std::uint64_t app_error = 0x9;
    server->stop_stream(server_connection_id, server_stream_id, app_error);

    // stop_stream() synchronously queues a stop_sending stream_closed event (quic.cpp:627) and arms
    // ngtcp2 to send STOP_SENDING; the next transport drain fires stream_stop_sending_cb (quic.cpp:1690)
    // for the SAME stream/reason. Pump a bounded number of times and collect every stop_sending event
    // the server observes.
    int                               server_stop_sending_count = 0;
    qb::io::quic::stream_close_reason observed_reason           = qb::io::quic::stream_close_reason::none;
    std::uint64_t                     observed_error            = 0;
    const auto                        stop_deadline             = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (server_stop_sending_count == 0 && std::chrono::steady_clock::now() < stop_deadline) {
        for (auto const &event : server->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_closed
                && event.stream_reason == qb::io::quic::stream_close_reason::stop_sending) {
                ++server_stop_sending_count;
                observed_reason = event.stream_reason;
                observed_error  = event.error_code;
            }
        }
        deliver_quic_packets(*server, *client);
        deliver_quic_packets(*client, *server);
        (void) client->drain_events();
    }

    ASSERT_GT(server_stop_sending_count, 0) << "server never observed its own STOP_SENDING (stop_stream queue + stream_stop_sending_cb)";
    EXPECT_EQ(observed_reason, qb::io::quic::stream_close_reason::stop_sending);
    EXPECT_EQ(observed_error, app_error) << "stop_sending must carry the application error code passed to stop_stream";
    EXPECT_EQ(server_stream_id, stream);
}

/**
 * @test A server at its connection cap silently drops a second client's initial without a server DoS
 * @brief The parent server is configured with max_connections == 1. A first client completes the
 *        two-backend handshake (one admitted connection). A SECOND client, with a distinct local
 *        endpoint and connection id, then drives its own initial datagrams at the parent; every one hits
 *        the connection-cap arm of accept_new_server_connection which DROPS the
 *        datagram — it must NOT queue_close_event on the parent (that would shut the whole listener down
 *        via the sentinel connection_id 0). So: the second client never reaches `connected`, the server
 *        keeps exactly one active connection, the server emits no parent connection_closed event, and the
 *        first connection stays usable (a clean close still surfaces the typed application_close).
 */
TEST(QuicHandshakeNativeBackend, ServerAtConnectionCapDropsSecondClientWithoutSelfClosing) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::quic::settings server_settings;
    server_settings.max_connections = 1;

    auto server = qb::io::quic::make_native_backend();
    server->configure(server_settings);

    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint first_endpoint{"127.0.0.1", 54321};
    const qb::io::endpoint second_endpoint{"127.0.0.1", 54322};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    auto first = qb::io::quic::make_native_backend();
    first->start_client(first_endpoint, server_endpoint, {"h3"}, client_tls);

    bool first_connected     = false;
    auto first_connection_id = direct_handshake(*first, *server, first_connected);
    ASSERT_TRUE(first_connected) << "the first client must connect to fill the single connection slot";
    ASSERT_NE(first_connection_id, 0u);
    EXPECT_EQ(server->current_stats().active_connections, 1u);

    // A second, distinct client now contends for a slot the cap forbids. Pump bounded; the parent must
    // keep dropping its initials and never admit it.
    auto second = qb::io::quic::make_native_backend();
    second->start_client(second_endpoint, server_endpoint, {"h3"}, client_tls);

    bool       second_connected   = false;
    bool       server_self_closed = false;
    const auto deadline           = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!second_connected && std::chrono::steady_clock::now() < deadline) {
        deliver_quic_packets(*second, *server);
        deliver_quic_packets(*server, *second);
        // Keep the admitted first connection serviced so it does not idle out under the pump.
        deliver_quic_packets(*first, *server);
        deliver_quic_packets(*server, *first);
        for (auto const &event : second->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connected)
                second_connected = true;
        for (auto const &event : server->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::connection_closed && event.connection_id == 0)
                server_self_closed = true;
        (void) first->drain_events();
    }

    EXPECT_FALSE(second_connected) << "a server at its connection cap must not admit a second client";
    EXPECT_FALSE(server_self_closed) << "the cap-drop must NOT close the whole listener (sentinel connection_id 0)";
    EXPECT_EQ(server->current_stats().active_connections, 1u) << "the cap holds the server at exactly one connection";

    // The first (admitted) connection is still routable: a clean parent close emits the typed event.
    server->close(13, "cap test done");
    auto events = server->drain_events();
    auto closed = std::find_if(events.begin(), events.end(),
                               [](auto const &event) { return event.type == qb::io::quic::backend_event::kind::connection_closed; });
    ASSERT_NE(closed, events.end());
    EXPECT_EQ(closed->connection_reason, qb::io::quic::disconnect_reason::application_close);
    EXPECT_EQ(closed->error_code, 13u);
}

/**
 * @test A clean stream close synthesises a trailing FIN on the LOCAL endpoint's un-FIN'd read half
 * @brief The server opens a bidirectional stream, writes a payload, then FINs its write half with a
 *        zero-length final frame. The client receives that FIN through recv_stream_data_cb (so the
 *        CLIENT records `_stream_fin_seen` and is barred from synthesising) and replies with a bare
 *        zero-length FIN of its own. ngtcp2 delivers the client's zero-length FIN to the SERVER without
 *        any recv_stream_data_cb call, so the server's `_stream_fin_seen[id]` stays false; when ngtcp2
 *        fully closes the stream without NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET, the server's
 *        stream_close_cb synthesises a trailing stream_data carrying error_code == 1 (the FIN
 *        marker, empty payload), runs erase_queued_stream_data / _next_stream_offsets cleanup,
 *        then queues stream_closed(finished). A numeric app_error_code of zero alone is not the gate.
 *
 * SEMANTICS — like STOP_SENDING earlier in this file, the FIN-synthesis is a LOCAL teardown artefact:
 * it fires on the endpoint that sent a FIN and never received an inbound data+FIN (here the SERVER),
 * NOT on the peer. The original form of this test wrongly expected the CLIENT to synthesise after a
 * graceful CONNECTION close — doubly wrong, because (a) a connection close transitions ngtcp2 to
 * draining and never fires per-stream stream_close_cb (only a connection_closed event is produced on
 * each side), and (b) the client learned the FIN through recv_stream_data_cb and therefore suppresses
 * synthesis. This corrected form observes the synthesis on the SERVER and asserts the client does NOT
 * synthesise.
 *
 * Determinism: bounded two-backend pumps that FAIL LOUD on a deadline; no event loop, no sleeps.
 */
TEST(QuicHandshakeNativeBackend, CleanStreamCloseSynthesisesFinOnLocalUnFinnedReadHalf) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();

    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint client_endpoint{"127.0.0.1", 54321};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    bool client_connected     = false;
    auto server_connection_id = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);

    // Server opens a bidi stream and writes a payload, then FINs its write half with a zero-length
    // final frame. The server's read half is never fed an inbound data+FIN (the client only replies
    // with a bare zero-length FIN), so the server's _stream_fin_seen[id] stays false and its
    // stream_close_cb takes the SYNTHESIS arm when the stream is fully torn down.
    const std::string message = "fin-payload";
    const auto        stream  = server->open_stream(server_connection_id, qb::io::quic::stream_direction::bidirectional);
    server->send_stream_data(server_connection_id, stream,
                             std::span<const std::byte>{reinterpret_cast<const std::byte *>(message.data()), message.size()}, false);

    // Let the payload land on the peer first (so the stream exists there).
    bool       client_got_payload = false;
    const auto open_deadline      = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!client_got_payload && std::chrono::steady_clock::now() < open_deadline) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);
        for (auto const &event : client->drain_events())
            if (event.type == qb::io::quic::backend_event::kind::stream_data && !event.payload.empty()
                && event.error_code == 0) // a non-FIN data frame
                client_got_payload = true;
        (void) server->drain_events();
    }
    ASSERT_TRUE(client_got_payload) << "client never received the server's stream payload";

    // Clean bidirectional close. The server FINs its write half (zero-length final frame); the client
    // receives that FIN through recv_stream_data_cb (so the CLIENT records _stream_fin_seen and will
    // NOT synthesise), then FINs its own half with a bare zero-length frame. ngtcp2 delivers the
    // client's zero-length FIN to the server WITHOUT a recv_stream_data_cb call, so the server's
    // _stream_fin_seen stays false: stream_close_cb without NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET
    // then takes the synthesis arm and emits a trailing FIN-marked stream_data (error_code == 1,
    // empty payload) followed by stream_closed(finished). Numeric app_error_code alone is not the gate.
    //
    // SEMANTICS — like stop_sending/STOP_SENDING earlier in this file, the FIN-synthesis is a LOCAL
    // teardown artefact: it fires on the endpoint that sent a FIN and never received an inbound data+FIN
    // (here the SERVER), NOT on the peer. A peer that learned the FIN via recv_stream_data_cb (the
    // CLIENT) suppresses synthesis and only observes stream_closed(finished). A graceful CONNECTION
    // close does NOT reach this arm at all: ngtcp2 transitions the connection to draining and never
    // fires per-stream stream_close_cb, so only a connection_closed event is produced on each side.
    server->send_stream_data(server_connection_id, stream, std::span<const std::byte>{}, true);

    // Let the server's FIN reach the client (so the client's read half completes via recv_stream_data_cb,
    // recording _stream_fin_seen on the client) before the client closes its own write half.
    const auto server_fin_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool       client_received_fin = false;
    while (!client_received_fin && std::chrono::steady_clock::now() < server_fin_deadline) {
        deliver_quic_packets(*server, *client);
        deliver_quic_packets(*client, *server);
        for (auto const &event : client->drain_events()) {
            // The client learns the FIN as real stream data (error_code == 1 via recv_stream_data_cb).
            if (event.type == qb::io::quic::backend_event::kind::stream_data && event.error_code == 1)
                client_received_fin = true;
        }
        (void) server->drain_events();
    }
    ASSERT_TRUE(client_received_fin) << "client never received the server's FIN as stream data";

    // Client FINs its own half (bare zero-length frame). Both halves are now closing, so the stream
    // fully closes on both endpoints and ngtcp2 fires stream_close_cb on each side.
    client->send_stream_data(server_connection_id, stream, std::span<const std::byte>{}, true);

    bool       server_saw_synthetic_fin    = false;
    bool       server_saw_finished         = false;
    bool       synthetic_fin_payload_empty = true;
    bool       client_saw_finished         = false;
    bool       client_synthesised          = false;
    const auto close_deadline              = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!server_saw_synthetic_fin || !server_saw_finished || !client_saw_finished) && std::chrono::steady_clock::now() < close_deadline) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);
        for (auto const &event : server->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_data && event.error_code == 1) {
                server_saw_synthetic_fin    = true;
                synthetic_fin_payload_empty = event.payload.empty();
            }
            if (event.type == qb::io::quic::backend_event::kind::stream_closed
                && event.stream_reason == qb::io::quic::stream_close_reason::finished)
                server_saw_finished = true;
        }
        for (auto const &event : client->drain_events()) {
            // The client already recorded the inbound FIN, so its stream_close_cb must NOT synthesise.
            if (event.type == qb::io::quic::backend_event::kind::stream_data && event.error_code == 1)
                client_synthesised = true;
            if (event.type == qb::io::quic::backend_event::kind::stream_closed
                && event.stream_reason == qb::io::quic::stream_close_reason::finished)
                client_saw_finished = true;
        }
    }

    EXPECT_TRUE(server_saw_synthetic_fin)
        << "the local FIN-and-clean-close of an un-FIN'd read half must synthesise a trailing FIN-marked stream_data (error_code == 1)";
    EXPECT_TRUE(synthetic_fin_payload_empty) << "the synthesised FIN event must carry an empty payload";
    EXPECT_TRUE(server_saw_finished) << "the gracefully torn-down stream must also produce stream_closed(finished)";
    EXPECT_TRUE(client_saw_finished) << "the peer (which learned the FIN through recv_stream_data_cb) must observe stream_closed(finished)";
    EXPECT_FALSE(client_synthesised) << "the peer must NOT synthesise a FIN: it already received one via recv_stream_data_cb";
}

enum class QuicReadHalfAction { none, local_stop, peer_reset };

class QuicReadHalfRetransmissionTest : public ::testing::TestWithParam<QuicReadHalfAction> {};

/**
 * @test A lost packet is retransmitted intact after a local read stop or a peer write reset
 * @brief The open-read case is the packet-loss control. The two read-end cases must keep
 *        unacknowledged TX storage alive until ngtcp2 retransmits and acknowledges it.
 */
TEST_P(QuicReadHalfRetransmissionTest, UnackedBidiWriteSurvivesPacketLoss) {
    ASSERT_TRUE(require_ssl_files());

    auto                   server = qb::io::quic::make_native_backend();
    auto                   client = qb::io::quic::make_native_backend();
    const qb::io::endpoint server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint client_endpoint{"127.0.0.1", 54321};

    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = ssl_resource_path("cert.pem");
    server_tls.private_key_file = ssl_resource_path("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);
    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;
    client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);

    bool       client_connected = false;
    const auto connection_id    = direct_handshake(*client, *server, client_connected);
    ASSERT_TRUE(client_connected);
    ASSERT_NE(connection_id, 0u);

    const auto        stream = client->open_stream(qb::io::quic::stream_direction::bidirectional);
    const std::string opener = "open";
    client->send_stream_data(0, stream, std::span<const std::byte>{reinterpret_cast<const std::byte *>(opener.data()), opener.size()}, false);
    bool server_saw_opener = false;
    for (int i = 0; i < 64 && !server_saw_opener; ++i) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);
        for (auto const &event : server->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_data && event.stream_id == stream && !event.payload.empty())
                server_saw_opener = true;
        }
        (void) client->drain_events();
    }
    ASSERT_TRUE(server_saw_opener);

    const std::string payload(512, 'Q');
    server->send_stream_data(connection_id, stream,
                             std::span<const std::byte>{reinterpret_cast<const std::byte *>(payload.data()), payload.size()}, false);
    auto lost_packets = server->drain_packets();
    ASSERT_FALSE(lost_packets.empty()) << "the TX witness needs an unacknowledged packet to lose";
    if (GetParam() == QuicReadHalfAction::local_stop)
        server->stop_stream(connection_id, stream, 0x29);
#ifdef QB_IO_QUIC_TEST_HOOKS
    if (GetParam() == QuicReadHalfAction::peer_reset) {
        // The peer only resets its write half. A full reset would also send STOP_SENDING,
        // legitimately cancelling the server's write and masking the TX lifetime bug.
        qb::io::quic::test::reset_stream_write(*client, stream, 0x2a);
        deliver_quic_packets(*client, *server);
        std::size_t reset_events = 0;
        for (auto const &event : server->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_closed && event.stream_id == stream
                && event.text == "stream reset by peer" && event.error_code == 0x2a)
                ++reset_events;
        }
        ASSERT_EQ(reset_events, 1u) << "the peer RESET_STREAM must reach stream_reset_cb with server TX unacknowledged";
    }
#endif

    std::string   received;
    std::uint64_t acked_bytes = 0;
    std::size_t   ack_events  = 0;
    const auto    deadline    = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int i = 0; i < 12 && (received.size() < payload.size() || acked_bytes < payload.size()) && std::chrono::steady_clock::now() < deadline;
         ++i) {
        const auto expiry = server->next_timeout();
        ASSERT_NE(expiry, std::chrono::steady_clock::time_point::max());
        if (expiry > deadline)
            break;
        if (expiry > std::chrono::steady_clock::now())
            std::this_thread::sleep_until(expiry);
        server->on_timeout(std::chrono::steady_clock::now());
        deliver_quic_packets(*server, *client);
        deliver_quic_packets(*client, *server);
        for (auto const &event : client->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_data && event.stream_id == stream) {
                received.append(reinterpret_cast<const char *>(event.payload.data()), event.payload.size());
            }
        }
        for (auto const &event : server->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::stream_data_acked && event.stream_id == stream) {
                acked_bytes += event.error_code;
                ++ack_events;
            }
        }
    }

    EXPECT_EQ(received.size(), payload.size());
    EXPECT_TRUE(received == payload) << "retransmission used bytes from a retired TX buffer";
    EXPECT_EQ(acked_bytes, payload.size()) << "the retained TX must release when the peer ACKs it";
    EXPECT_EQ(ack_events, 1u) << "a single retransmitted frame must be acknowledged once";
}

#ifdef QB_IO_QUIC_TEST_HOOKS
INSTANTIATE_TEST_SUITE_P(ReadOpenStoppedAndPeerReset, QuicReadHalfRetransmissionTest,
                         ::testing::Values(QuicReadHalfAction::none, QuicReadHalfAction::local_stop, QuicReadHalfAction::peer_reset));
#else
INSTANTIATE_TEST_SUITE_P(ReadOpenAndStopped, QuicReadHalfRetransmissionTest,
                         ::testing::Values(QuicReadHalfAction::none, QuicReadHalfAction::local_stop));
#endif

// =============================================================================
// Hostname binding of a verify_peer client. The native client context trusts OpenSSL's default verify
// paths and nothing else, so each client is built with SSL_CERT_FILE pointing at the committed
// localhost certificate the server presents: the chain then verifies, and the name the client binds
// is the only thing left to decide the handshake.
// =============================================================================
namespace quic_handshake_hostname_test {

// The committed localhost pair (system/resources/ssl), not ssl_resource_path(): which pair that search
// returns depends on the working directory, and only this one carries the name the positive case asks for.
std::filesystem::path
localhost_fixture(const char *name) {
    return std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path() / "resources" / "ssl" / name;
}

// SSL_CERT_FILE for the guard's lifetime; the previous value, or its absence, is restored after.
class scoped_ssl_cert_file {
public:
    explicit scoped_ssl_cert_file(std::string const &file) {
#ifdef _WIN32
        char       *previous = nullptr; // _dupenv_s, not getenv: MSVC deprecates the latter (C4996)
        std::size_t length   = 0;
        if (_dupenv_s(&previous, &length, "SSL_CERT_FILE") == 0 && previous) {
            _had_previous = true;
            _previous     = previous;
        }
        std::free(previous);
#else
        if (char const *previous = std::getenv("SSL_CERT_FILE")) {
            _had_previous = true;
            _previous     = previous;
        }
#endif
        assign(file.c_str());
    }
    ~scoped_ssl_cert_file() {
        assign(_had_previous ? _previous.c_str() : nullptr);
    }
    scoped_ssl_cert_file(scoped_ssl_cert_file const &)            = delete;
    scoped_ssl_cert_file &operator=(scoped_ssl_cert_file const &) = delete;

private:
    static void
    assign(char const *value) {
#ifdef _WIN32
        _putenv_s("SSL_CERT_FILE", value ? value : "");
#else
        if (value)
            ::setenv("SSL_CERT_FILE", value, 1);
        else
            ::unsetenv("SSL_CERT_FILE");
#endif
    }
    bool        _had_previous = false;
    std::string _previous;
};

// Whether a verify_peer client asking for `server_name` completes its handshake with a server presenting
// the localhost certificate the client's trust store holds.
enum class handshake_outcome { connected, refused, undecided };

std::ostream &
operator<<(std::ostream &os, handshake_outcome outcome) {
    return os << (outcome == handshake_outcome::connected ? "connected" : outcome == handshake_outcome::refused ? "refused" : "undecided");
}

handshake_outcome
verified_client_handshake(std::string const &server_name) {
    auto server = qb::io::quic::make_native_backend();
    auto client = qb::io::quic::make_native_backend();
    server->configure(qb::io::quic::settings{});
    client->configure(qb::io::quic::settings{});

    const qb::io::endpoint   server_endpoint{"127.0.0.1", 4433};
    const qb::io::endpoint   client_endpoint{"127.0.0.1", 54321};
    qb::io::quic::tls_config server_tls;
    server_tls.certificate_file = localhost_fixture("cert.pem");
    server_tls.private_key_file = localhost_fixture("key.pem");
    server->start_server(server_endpoint, {"h3"}, server_tls);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = server_name;
    client_tls.verify_peer = true;
    {
        scoped_ssl_cert_file trust{localhost_fixture("cert.pem").string()}; // read when the client context is built
        client->start_client(client_endpoint, server_endpoint, {"h3"}, client_tls);
    }

    // The client's own verdict: `connected`, or `connection_closed` when its handshake fails -- a refusal
    // observed, not inferred from a deadline passing quietly.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        deliver_ready_quic_packets(*server, *client);
        deliver_ready_quic_packets(*client, *server);
        for (auto const &event : client->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::connected)
                return handshake_outcome::connected;
            if (event.type == qb::io::quic::backend_event::kind::connection_closed)
                return handshake_outcome::refused;
        }
        [[maybe_unused]] auto const server_events = server->drain_events();
    }
    return handshake_outcome::undecided;
}

} // namespace quic_handshake_hostname_test

/**
 * @test A verify_peer client binds its server name, and a name OpenSSL refuses to bind fails closed
 * @brief Against a trusted localhost certificate: `localhost` connects (the trust setup works, so the
 *        refusals are the name's), `wrong.example.com` does not (the name is checked, not only the chain),
 *        and neither does `x`. OpenSSL 4.0 validates a name before installing it as the target and refuses
 *        a single character; the client called `SSL_set1_host` and ignored its result, so under OpenSSL 4 it
 *        verified the chain alone and accepted the certificate for `x` -- a certificate for any host. The
 *        target is installed through `X509_VERIFY_PARAM` now and a refused one fails the handshake, as the
 *        TCP transport's does (Huly QB-1076). Under OpenSSL 3 `x` is installed and simply does not match.
 */
TEST(QuicHandshakeNativeBackend, VerifiedClientBindsItsServerNameAndFailsClosedOnARefusedOne) {
    using quic_handshake_hostname_test::handshake_outcome;
    using quic_handshake_hostname_test::localhost_fixture;
    using quic_handshake_hostname_test::verified_client_handshake;
    ASSERT_TRUE(std::filesystem::exists(localhost_fixture("cert.pem"))) << localhost_fixture("cert.pem");
    ASSERT_TRUE(std::filesystem::exists(localhost_fixture("key.pem"))) << localhost_fixture("key.pem");

    EXPECT_EQ(verified_client_handshake("localhost"), handshake_outcome::connected)
        << "the trusted-certificate setup itself is broken, so the two refusals below would prove nothing";
    EXPECT_EQ(verified_client_handshake("wrong.example.com"), handshake_outcome::refused)
        << "a certificate issued for localhost was accepted for wrong.example.com: only the chain was checked";
    EXPECT_EQ(verified_client_handshake("x"), handshake_outcome::refused)
        << "a server name OpenSSL refused to bind left the chain check alone: any trusted certificate was accepted";
}

#endif // QB_HAS_QUIC
