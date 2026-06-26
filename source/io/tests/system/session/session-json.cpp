/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/session/session-json.cpp
 * @brief JSON / MsgPack session protocols over real loopback transports — the qb-io JSON-session system suite.
 *
 * The async session/protocol stack exercised end-to-end: the `qb::io::async` event loop, the
 * `qb::protocol::json` (`\0`-delimited) and `qb::protocol::json_packed` (MsgPack) framing protocols, the
 * `use<>::tcp::client/server` / `tcp::ssl::client/server` / `quic::*` composition helpers, and the
 * io_handler session lifecycle. The pure parser outlier that used to share `test-session-json.cpp`
 * (`JSON_MALFORMED_OVER_QUIC`) now lives in `unit/protocol/json-session-parse.cpp`; this file owns only
 * the cases that move bytes across a transport.
 *
 * De-flake / de-dup work applied (per dossier qbio-c15 §"Improve / add"):
 *   - NO fixed ports — every server binds an EPHEMERAL port (`:0` / `listen_v*(0)`) and the client reads
 *     it back via `local_endpoint().port()`. This kills the 22991-22993 `EADDRINUSE` collisions and
 *     unblocks parallel `ctest -j`.
 *   - `NB_ITERATION` dropped from 4096 to 64 (correctness, not throughput; the bulk loop is a benchmark
 *     concern). Each frame's `["message"]` length is still verified in every handler.
 *   - `JSON_MALFORMED_RESILIENCE` is FIXED from vacuous: the headline "handles malformed JSON without
 *     terminating" claim is now actually asserted. The good frame is parsed and delivered
 *     (`good_messages == 1`); the malformed frame is *discarded by the parser* — `json::onMessage` calls
 *     `not_ok()`, which (a) NEVER delivers a message to `on` (so `bad_messages == 0` — a discarded body
 *     is the framework's "drop it" path, not a "bad message delivered" path) and (b) tears the session
 *     down, which the server observes via `disconnected(uuid)`. The 50 ms / 200 ms `sleep_for` sequencing
 *     is replaced by a write-then-`pump_until` handshake.
 *   - `pump_until` is the shared deadline-bounded loop pump; timeouts fail LOUDLY instead of hanging.
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

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>
#include <qb/json.h>
#include <qb/uuid.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/ssl_fixtures.h"

using namespace qb::io;
using qb::io::test::pump_until;

namespace {

constexpr std::size_t NB_ITERATION    = 64; // correctness count; throughput is owned by the benchmark
constexpr char        STRING_MESSAGE[] = "Here is my content test";
constexpr std::size_t MESSAGE_LEN     = sizeof(STRING_MESSAGE) - 1;

std::atomic<std::size_t> msg_count_server{0};
std::atomic<std::size_t> msg_count_client{0};

} // namespace

// =============================================================================
// JSON OVER PLAIN TCP
// =============================================================================

namespace {

class JsonTcpServer;
class JsonTcpServerSession : public use<JsonTcpServerSession>::tcp::client<JsonTcpServer> {
public:
    using Protocol = qb::protocol::json<JsonTcpServerSession>;
    explicit JsonTcpServerSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(), MESSAGE_LEN);
        publish(msg.json, '\0');
        ++msg_count_server;
    }
};
class JsonTcpServer : public use<JsonTcpServer>::tcp::server<JsonTcpServerSession> {
public:
    std::size_t connections = 0;
    void
    on(IOSession &) {
        ++connections;
    }
};
class JsonTcpClient : public use<JsonTcpClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::json<JsonTcpClient>;
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(), MESSAGE_LEN);
        ++msg_count_client;
    }
};

} // namespace

/**
 * @test JSON frames round-trip over loopback TCP
 * @brief Server on an ephemeral v4 port echoes each `{"message": STRING_MESSAGE}` frame; the client sends
 *        NB_ITERATION of them. Asserts exact counts on both sides (each length-checked in its handler)
 *        and at least one accepted connection.
 */
TEST(SessionJson, JsonOverTcp) {
    async::init();
    msg_count_server.store(0);
    msg_count_client.store(0);

    JsonTcpServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread worker([port] {
        async::init();
        JsonTcpClient client;
        ASSERT_EQ(client.transport().connect(uri{"tcp://127.0.0.1:" + std::to_string(port)}), SocketStatus::Done);
        client.start();

        for (std::size_t i = 0; i < NB_ITERATION; ++i)
            client.publish(qb::json{{"message", STRING_MESSAGE}}, '\0');

        EXPECT_TRUE(pump_until([&] { return msg_count_client.load() == NB_ITERATION; }, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until([&] { return msg_count_server.load() == NB_ITERATION && msg_count_client.load() == NB_ITERATION; },
                           std::chrono::seconds(10)));
    worker.join();

    EXPECT_EQ(msg_count_server.load(), NB_ITERATION);
    EXPECT_EQ(msg_count_client.load(), NB_ITERATION);
    EXPECT_GE(server.connections, 1u);
    async::listener::current.clear();
}

// =============================================================================
// JSON (MSGPACK) OVER SECURE TCP
// =============================================================================

#ifdef QB_HAS_SSL

namespace {

class JsonSecureServer;
class JsonSecureServerSession : public use<JsonSecureServerSession>::tcp::ssl::client<JsonSecureServer> {
public:
    using Protocol = qb::protocol::json_packed<JsonSecureServerSession>;
    explicit JsonSecureServerSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(), MESSAGE_LEN);
        *this << qb::json::to_msgpack(msg.json) << '\0';
        ++msg_count_server;
    }
};
class JsonSecureServer : public use<JsonSecureServer>::tcp::ssl::server<JsonSecureServerSession> {
public:
    std::size_t connections = 0;
    void
    on(IOSession &) {
        ++connections;
    }
};
class JsonSecureClient : public use<JsonSecureClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::json_packed<JsonSecureClient>;
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(), MESSAGE_LEN);
        ++msg_count_client;
    }
};

} // namespace

/**
 * @test MsgPack JSON frames round-trip over loopback TLS
 * @brief Server context built from the shipped local cert/key, bound to an ephemeral v4 port; the client
 *        opts out of peer verification (self-signed) and exchanges NB_ITERATION MsgPack frames. The certs
 *        are REQUIRED in a QB_HAS_SSL build (hard fail, not a silent skip).
 */
TEST(SessionJson, JsonOverSecureTcp) {
    ASSERT_TRUE(qb::io::test::require_ssl_files()) << "the test harness ships its TLS certs; absence is a packaging regression";

    async::init();
    msg_count_server.store(0);
    msg_count_client.store(0);

    JsonSecureServer server;
    server.transport().init(ssl::create_server_context(SSLv23_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                       qb::io::test::ssl_resource_path("key.pem")));
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread worker([port] {
        async::init();
        JsonSecureClient client;
        client.transport().set_insecure();
        ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), 0);
        client.start();

        for (std::size_t i = 0; i < NB_ITERATION; ++i)
            client << qb::json::to_msgpack(qb::json{{"message", STRING_MESSAGE}}) << '\0';

        EXPECT_TRUE(pump_until([&] { return msg_count_client.load() == NB_ITERATION; }, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until([&] { return msg_count_server.load() == NB_ITERATION && msg_count_client.load() == NB_ITERATION; },
                           std::chrono::seconds(10)));
    worker.join();

    EXPECT_EQ(msg_count_server.load(), NB_ITERATION);
    EXPECT_EQ(msg_count_client.load(), NB_ITERATION);
    EXPECT_GE(server.connections, 1u);
    async::listener::current.clear();
}

#endif // QB_HAS_SSL

// =============================================================================
// MALFORMED JSON RESILIENCE (no terminate; parser drops the bad frame)
// =============================================================================

namespace {

// Server-level mirrors of the parse outcome, so the assertions never touch the session object after it
// is destroyed on the not_ok() teardown.
std::atomic<int> good_messages{0};
std::atomic<int> bad_messages{0};

class MalformedJsonServer;
class MalformedJsonSession : public use<MalformedJsonSession>::tcp::client<MalformedJsonServer> {
public:
    using Protocol = qb::protocol::json<MalformedJsonSession>;

    explicit MalformedJsonSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        // json::onMessage only delivers NON-discarded bodies; a discarded frame never reaches here (the
        // parser calls not_ok() and drops it). bad_messages therefore stays 0 — the is_discarded() branch
        // exists only to make that invariant explicit and assertable.
        if (msg.json.is_discarded())
            ++bad_messages;
        else
            ++good_messages;
    }
};

class MalformedJsonServer : public use<MalformedJsonServer>::tcp::server<MalformedJsonSession> {
public:
    std::atomic<bool> session_connected{false};
    std::atomic<bool> session_disconnected{false};

    void
    on(IOSession &) {
        session_connected.store(true);
    }

    // Peer hangup OR a not_ok() protocol teardown removes the session from the io_handler map. This is
    // distinct from tcp::server::on(event::disconnected&&) (acceptor transport only).
    void
    disconnected(qb::uuid ident) {
        session_disconnected.store(true);
        io_handler<MalformedJsonServer, MalformedJsonSession>::disconnected(ident);
    }
};

} // namespace

/**
 * @test The server survives a malformed JSON frame and the parser drops it
 * @brief Strengthened from the original vacuous version (which asserted only connect/disconnect bools).
 *        A raw tcp::socket sends one well-formed `{"key":"value"}\0` frame then one malformed
 *        `{this is not json}\0` frame. The handshake is sequenced by pump (no wall-clock sleeps): wait for
 *        the good frame to be parsed (good_messages==1), then send the bad frame and wait for the server
 *        to observe the session teardown.
 *
 * Assertions (the real ground truth):
 *   - good_messages == 1  — the well-formed frame parsed and was delivered;
 *   - bad_messages  == 0  — a discarded body is dropped by the parser, never delivered to on();
 *   - session_disconnected — the malformed frame's not_ok() tore the session down (the resilience proof:
 *     it disconnects cleanly instead of crashing/terminating);
 *   - session_connected.
 */
TEST(SessionJson, JsonMalformedResilience) {
    async::init();
    good_messages.store(0);
    bad_messages.store(0);

    MalformedJsonServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::atomic<bool> bad_sent{false};
    std::atomic<bool> worker_ok{false};

    std::thread worker([&] {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);

        const char good_json[] = "{\"key\":\"value\"}\0";
        ASSERT_GT(sock.write(good_json, sizeof(good_json) - 1), 0);

        // Wait until the server has parsed the good frame before sending the bad one (no sleep race).
        EXPECT_TRUE(pump_until([&] { return good_messages.load() == 1; }, std::chrono::seconds(5)))
            << "server never parsed the well-formed frame";

        const char bad_json[] = "{this is not json}\0";
        ASSERT_GT(sock.write(bad_json, sizeof(bad_json) - 1), 0);
        bad_sent.store(true);

        // The malformed frame's not_ok() disconnects the session; wait for the server to observe it.
        EXPECT_TRUE(pump_until([&] { return server.session_disconnected.load(); }, std::chrono::seconds(5)))
            << "the malformed frame should tear down the session via not_ok()";

        sock.disconnect();
        worker_ok.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return server.session_disconnected.load() && worker_ok.load(); }, std::chrono::seconds(10)))
        << "server never saw the session teardown after the malformed frame";
    worker.join();

    EXPECT_TRUE(server.session_connected.load());
    EXPECT_EQ(good_messages.load(), 1) << "the well-formed frame must be parsed and delivered";
    EXPECT_EQ(bad_messages.load(), 0) << "a discarded body is dropped by the parser, never delivered to on()";
    EXPECT_TRUE(server.session_disconnected.load()) << "the malformed frame must disconnect the session, not crash the server";
    EXPECT_TRUE(bad_sent.load());
    async::listener::current.clear();
}

// =============================================================================
// JSON OVER LIVE QUIC LOOPBACK (real UDP + local TLS; QB_HAS_QUIC only, certs REQUIRED)
// =============================================================================

#ifdef QB_HAS_QUIC

namespace {

class EchoJsonQuicSession : public use<EchoJsonQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::json<EchoJsonQuicSession>;
    std::size_t messages = 0;
    explicit EchoJsonQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}
    void
    on(Protocol::message &&message) {
        publish(message.json.dump());
        *this << '\0';
        ++messages;
    }
};

class JsonClientQuicSession : public use<JsonClientQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::json<JsonClientQuicSession>;
    qb::json last_json;
    explicit JsonClientQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}
    void
    on(Protocol::message &&message) {
        last_json = std::move(message.json);
    }
};

template <typename StreamSession>
class ProtocolQuicServer : public async::quic::server<ProtocolQuicServer<StreamSession>, StreamSession> {
public:
    int connected = 0;
    void
    on(async::quic::event::connected const &) {
        ++connected;
    }
};

class JsonProtocolQuicClient : public use<JsonProtocolQuicClient>::quic::connector<JsonClientQuicSession> {};

template <typename Server, typename Client>
void
connect_local_quic_pair(Server &server, Client &client) {
    ASSERT_TRUE(server.listen(uri{"quic://127.0.0.1:0"}, qb::io::test::ssl_resource_path("cert.pem"),
                              qb::io::test::ssl_resource_path("key.pem"), {"qb-test"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    const auto endpoint_uri = std::string{"quic://127.0.0.1:"} + std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(client.connect(uri{endpoint_uri}, client_tls, {"qb-test"}));

    ASSERT_TRUE(pump_until(
        [&] {
            return server.current_state() == async::quic::endpoint::state::connected
                   && client.current_state() == async::quic::endpoint::state::connected;
        },
        std::chrono::seconds(5)))
        << "QUIC handshake did not complete";
}

} // namespace

/**
 * @test A JSON message round-trips over a single QUIC stream
 * @brief Establishes a local QUIC pair, opens a bidirectional stream, sends one
 *        `{"message":"hello-json","n":42}\0`, and asserts the echoed JSON reassembles with the exact
 *        values on the client and the server delivered exactly one message.
 */
TEST(SessionJson, JsonOverQuic) {
    ASSERT_TRUE(qb::io::test::require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    async::init();

    ProtocolQuicServer<EchoJsonQuicSession> server;
    JsonProtocolQuicClient                  client;
    connect_local_quic_pair(server, client);

    auto stream  = client.open_bidirectional_stream();
    auto payload = qb::json{{"message", "hello-json"}, {"n", 42}}.dump();
    payload.push_back('\0');
    client.send_stream_data(stream.id(), payload, true);

    EXPECT_TRUE(pump_until(
        [&] {
            auto *response = client.stream_session(stream.id());
            return response && response->last_json.is_object() && response->last_json.value("message", "") == "hello-json";
        },
        std::chrono::seconds(5)));

    auto *server_session = server.stream_session(stream.id());
    auto *client_session = client.stream_session(stream.id());
    ASSERT_NE(server_session, nullptr);
    ASSERT_NE(client_session, nullptr);
    EXPECT_EQ(server_session->messages, 1u);
    EXPECT_EQ(client_session->last_json["message"].get<std::string>(), "hello-json");
    EXPECT_EQ(client_session->last_json["n"].get<int>(), 42);

    client.close();
    server.close();
    async::listener::current.clear();
}

#endif // QB_HAS_QUIC
