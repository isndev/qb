/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/quic/quic-endpoint-loop.cpp
 * @brief `qb::io::async::quic::endpoint` event-loop body — the live UDP/timer/watcher paths.
 *
 * `unit/quic/quic-adapter.cpp` already proves every facade-to-backend *delegation* and event
 * *dispatch* branch with the in-memory `FakeQuicBackend` (no socket, no loop). `system/quic/
 * quic-handshake.cpp` proves the happy-path loopback handshake end-to-end. What neither covers — and
 * what this file targets — are the `endpoint`'s own *event-loop body* branches that only execute when
 * a REAL libngtcp2 backend produces real packets/timeouts AND the framework's libev watchers actually
 * fire (`endpoint.h:119-185, 499-533`):
 *
 *   - the `on(event::io)` EV_READ receive loop bounded by `settings.udp_rx_batch_size` — the
 *     budget-limited datagram drain (the live handshake leaves the budget unlimited, so the
 *     `budget-- > 0` ceiling and the mid-loop `ret == 0` break are never reached there);
 *   - `flush_udp_packets` over the REAL bound socket throttled by `settings.udp_tx_batch_size`
 *     (multi-poll flush against a live peer — the unit test only throttles the mock);
 *   - `drain_backend_packets` arming EV_WRITE when the backend `wants_write()` and reverting to
 *     EV_READ once the pending queue empties;
 *   - `arm_timer` computing a real future deadline from `backend::next_timeout()` and `on(timer&)`
 *     firing `backend::on_timeout` so an idle connection is torn down by the timer path (not by an
 *     inbound packet);
 *   - the destructor's `close()` + `unregister_watchers()` running on a still-connected live endpoint.
 *
 * Every case here needs the native backend (`QB_HAS_QUIC`), a real UDP loopback socket and the shipped
 * TLS cert (cert absence is a HARD failure per the suite spec, never a silent skip). The handshakes are
 * de-flaked with the shared deadline-bounded `pump_until`. Ephemeral ports only.
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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/quic_test_doubles.h"
#include "../../shared/ssl_fixtures.h"

using qb::io::test::CallbackQuicClient;
using qb::io::test::CallbackQuicServer;
using qb::io::test::pump_until;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

using State = qb::io::async::quic::endpoint::state;

namespace {

// Bring up a server on an ephemeral port and a client connected to it; pump (loudly bounded) until both
// endpoints reach `connected`. ALPN defaults to {"h3"}. Mirrors the establish_loopback in
// quic-handshake.cpp but is duplicated here so this TU is self-contained (the helper is in an anonymous
// namespace there and not shared).
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

    ASSERT_TRUE(pump_until([&] { return server.current_state() == State::connected && client.current_state() == State::connected; },
                           std::chrono::seconds(5)))
        << "QUIC loopback handshake did not complete";
}

} // namespace

// =============================================================================
// RX BATCH BUDGET — the budget-bounded EV_READ datagram drain in on(event::io)
// =============================================================================

/**
 * @test A tiny udp_rx_batch_size still delivers a multi-packet payload across several poll cycles
 * @brief `on(event::io)` reads at most `settings.udp_rx_batch_size` datagrams per EV_READ wakeup
 *        (endpoint.h:504-518). With the budget pinned to 1 on the server, a client payload that the
 *        backend fragments across multiple QUIC packets cannot all be consumed in a single readable
 *        wakeup — the loop hits the `budget-- > 0` ceiling and yields, and the framework re-arms
 *        EV_READ so the remainder is drained on subsequent wakeups. Driving a payload to the server
 *        with rx_batch==1 therefore EXERCISES the budget ceiling (the live handshake leaves the budget
 *        unlimited, so that branch is otherwise never taken) while still delivering the whole message.
 *        Asserted on the delivered bytes, not on internal counters.
 */
TEST(QuicEndpointLoop, RxBatchBudgetDeliversMultiPacketPayloadAcrossPolls) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    qb::io::quic::settings server_settings;
    server_settings.udp_rx_batch_size = 1; // one datagram per readable wakeup -> forces the budget ceiling

    CallbackQuicServer            server;
    qb::io::async::quic::endpoint client;
    server.set_settings(server_settings);

    establish_loopback(server, client);

    // A payload large enough that ngtcp2 splits it across more than one QUIC packet, so the server's
    // rx_batch==1 read loop must span several EV_READ wakeups to consume it all.
    const std::string payload(48 * 1024, 'q');
    auto              stream = client.open_bidirectional_stream();
    client.send_stream_data(stream.id(), payload, true);

    EXPECT_TRUE(pump_until([&] { return server.received.size() == payload.size(); }, std::chrono::seconds(10)))
        << "the rx-batch-limited server never assembled the full multi-packet payload";
    EXPECT_EQ(server.received, payload);
    EXPECT_GE(server.stream_started, 1);

    client.close();
    server.close();
    qb::io::async::listener::current.clear();
}

// =============================================================================
// TX BATCH BUDGET — flush_udp_packets over the live socket, throttled per poll
// =============================================================================

/**
 * @test A tiny udp_tx_batch_size still flushes a large response across several poll cycles
 * @brief `flush_udp_packets` sends at most `settings.udp_tx_batch_size` datagrams per call before
 *        leaving the remainder queued for the next EV_WRITE/poll (endpoint.h:150-167). With the budget
 *        pinned to 1 on the server, a large server-initiated payload cannot drain in a single flush:
 *        `drain_backend_packets` must observe the still-non-empty pending queue, arm EV_WRITE
 *        (endpoint.h:180-181), and the subsequent writable wakeups must flush the rest. The client
 *        receiving the whole payload proves the throttled multi-poll flush over the REAL socket (the
 *        unit test only throttles the mock backend; here the bytes travel a live loopback datagram
 *        path and the EV_WRITE re-arm is genuinely taken).
 */
TEST(QuicEndpointLoop, TxBatchBudgetFlushesLargePayloadAcrossPolls) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    // A server that records the connection id and can push a large stream to the client.
    struct PushServer : qb::io::async::quic::server<PushServer, qb::io::test::DummyQuicStreamSession> {
        std::uint64_t connection_id = 0;
        void
        on(qb::io::async::quic::event::connected const &ev) {
            connection_id = ev.connection_id;
        }
    };
    struct RecvClient : qb::io::async::quic::connector<RecvClient, qb::io::test::DummyQuicStreamSession> {
        std::string received;
        void
        on(qb::io::async::quic::event::stream_data const &ev) {
            received.append(ev.payload.data(), ev.payload.size());
        }
    };

    qb::io::quic::settings server_settings;
    server_settings.udp_tx_batch_size = 1; // one datagram per flush -> forces EV_WRITE re-arm + multi-poll drain

    PushServer server;
    RecvClient client;
    server.set_settings(server_settings);

    establish_loopback(server, client);

    ASSERT_TRUE(pump_until([&] { return server.connection_id != 0; }, std::chrono::seconds(5))) << "server never learned the connection id";

    const std::string payload(48 * 1024, 'z');
    auto             *session = server.open_bidirectional_stream_session(server.connection_id);
    ASSERT_NE(session, nullptr);
    session->publish(std::string_view{payload.data(), payload.size()});
    ASSERT_TRUE(server.flush_stream_session(server.connection_id, session->id()));

    EXPECT_TRUE(pump_until([&] { return client.received.size() == payload.size(); }, std::chrono::seconds(10)))
        << "the tx-batch-limited server never flushed the full large payload";
    EXPECT_EQ(client.received, payload);

    client.close();
    server.close();
    qb::io::async::listener::current.clear();
}

// =============================================================================
// TIMER PATH — arm_timer / on(timer&) drive an idle-timeout teardown
// =============================================================================

/**
 * @test The endpoint's timer watcher tears down an idle connection without any inbound packet
 * @brief `arm_timer` programs the libev timer from `backend::next_timeout()` and `on(event::timer&)`
 *        feeds `backend::on_timeout(now)` (endpoint.h:119-131, 526-533). With a very short idle_timeout
 *        on the client and the server then stopped (no more packets ever arrive), the ONLY thing that
 *        can close the client is its own timer firing: the connection must transition to `closed`
 *        purely through the timer path. This is the one teardown the packet-driven tests never reach
 *        (every other close is triggered by an inbound datagram or an explicit close() call). A loud,
 *        generous deadline guards against a hang if the timer never fires.
 */
TEST(QuicEndpointLoop, IdleTimeoutClosesConnectionViaTimerPath) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    qb::io::quic::settings client_settings;
    client_settings.idle_timeout = std::chrono::milliseconds(300); // short, so the idle timer fires quickly

    CallbackQuicServer server;
    CallbackQuicClient client;
    client.set_settings(client_settings);

    establish_loopback(server, client);
    EXPECT_EQ(client.current_state(), State::connected);

    // Stop the server so no further packets reach the client. From here the client can only learn it is
    // dead through its OWN idle timer (arm_timer -> on(timer&) -> backend::on_timeout), which must drive
    // it to closed. The deadline is comfortably above idle_timeout but still bounded.
    server.close();

    EXPECT_TRUE(pump_until([&] { return client.current_state() == State::closed; }, std::chrono::seconds(8)))
        << "the idle client was never closed by its timer path";
    EXPECT_EQ(client.current_state(), State::closed);
    EXPECT_GE(client.closed, 1) << "the timer-driven teardown must dispatch a connection_closed";

    client.close();
    qb::io::async::listener::current.clear();
}

// =============================================================================
// DESTRUCTOR TEARDOWN — close() + unregister_watchers() on a live, connected endpoint
// =============================================================================

/**
 * @test Destroying a still-connected live endpoint closes it and drops its watchers cleanly
 * @brief The endpoint destructor runs `close()` then `unregister_watchers()` (endpoint.h:250-253). The
 *        unit-tier ListenerClearDoesNotDangleEndpointWatchers proves this for a MOCK-backed endpoint
 *        torn down by an explicit listener clear; here a NATIVE-backed client is brought to `connected`
 *        over the live loopback and then destroyed while still open, so the destructor's close() runs
 *        against a real libngtcp2 connection (draining + socket close) and unregister_watchers() drops
 *        the real libev IO + timer watchers. The listener returning to its pre-connect size proves no
 *        watcher leaked; running cleanly under the suite's ASan config proves no dangling handle.
 */
TEST(QuicEndpointLoop, DestructorClosesAndUnregistersWatchersOnLiveEndpoint) {
    ASSERT_TRUE(require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    qb::io::async::init();

    CallbackQuicServer server;
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"), {"h3"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    const auto baseline = qb::io::async::listener::current.size();

    {
        qb::io::quic::tls_config client_tls;
        client_tls.server_name = "localhost";
        client_tls.verify_peer = false;

        CallbackQuicClient client;
        const auto         uri = std::string{"quic://127.0.0.1:"} + std::to_string(server.local_endpoint().port());
        ASSERT_TRUE(client.connect(qb::io::uri{uri}, client_tls, {"h3"}));

        ASSERT_TRUE(pump_until([&] { return client.current_state() == State::connected && server.connected >= 1; }, std::chrono::seconds(5)))
            << "the client never connected";
        EXPECT_TRUE(client.is_open());

        // client goes out of scope here: ~endpoint() must close() the live connection and
        // unregister_watchers() must drop both registered libev watchers.
    }

    // The two watchers the live client registered (IO + timer) are gone; the listener is back to the
    // size it had before the client connected (the server's own two watchers remain).
    EXPECT_EQ(qb::io::async::listener::current.size(), baseline) << "the destructor leaked a watcher";

    server.close();
    qb::io::async::listener::current.clear();
}
