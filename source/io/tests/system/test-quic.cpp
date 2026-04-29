#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>
#include <thread>

namespace {

[[maybe_unused]] std::filesystem::path
ssl_resource_path(char const *file_name) {
    return std::filesystem::path(__FILE__).parent_path() / "resources" / "ssl" /
           file_name;
}

class FakeQuicBackend final : public qb::io::quic::backend {
public:
    qb::io::quic::settings last_settings{};
    qb::io::quic::stats stats{};
    std::vector<std::string> last_alpn;
    qb::io::endpoint last_local;
    qb::io::endpoint last_remote;
    qb::io::quic::tls_config last_tls;
    int configure_calls = 0;
    int start_server_calls = 0;
    int start_client_calls = 0;
    int open_bidi_calls = 0;
    int open_uni_calls = 0;
    int close_calls = 0;
    int send_stream_data_calls = 0;
    int extend_stream_credit_calls = 0;
    std::uint64_t extended_stream_id = 0;
    std::uint64_t extended_bytes = 0;
    int send_datagram_calls = 0;
    int reset_stream_calls = 0;
    std::uint64_t reset_stream_id = 0;
    std::uint64_t reset_stream_code = 0;
    std::uint64_t close_code = 0;
    std::string close_reason;
    bool close_on_send_stream_data = false;
    std::vector<qb::io::quic::backend_event> queued_events;
    std::vector<qb::io::quic::packet> queued_packets;

    void configure(qb::io::quic::settings const& config) override {
        ++configure_calls;
        last_settings = config;
    }

    void start_server(qb::io::endpoint const& local,
                      std::vector<std::string> const& alpn_protocols,
                      qb::io::quic::tls_config const& tls) override {
        ++start_server_calls;
        last_local = local;
        last_alpn = alpn_protocols;
        last_tls = tls;
        stats.active_connections = 0;
    }

    void start_client(qb::io::endpoint const& local,
                      qb::io::endpoint const& remote,
                      std::vector<std::string> const& alpn_protocols,
                      qb::io::quic::tls_config const& tls) override {
        ++start_client_calls;
        last_local = local;
        last_alpn = alpn_protocols;
        last_tls = tls;
        last_remote = remote;
        stats.active_connections = 1;
    }

    void on_udp_datagram(qb::io::quic::packet_view) override {}
    void on_timeout(std::chrono::steady_clock::time_point) override {}

    std::chrono::steady_clock::time_point next_timeout() const override {
        return std::chrono::steady_clock::time_point::max();
    }

    bool wants_write() const noexcept override { return !queued_packets.empty(); }

    std::vector<qb::io::quic::packet> drain_packets() override {
        auto out = std::move(queued_packets);
        queued_packets.clear();
        return out;
    }

    std::vector<qb::io::quic::backend_event> drain_events() override {
        auto out = std::move(queued_events);
        queued_events.clear();
        return out;
    }

    std::uint64_t open_stream(qb::io::quic::stream_direction direction) override {
        if (direction == qb::io::quic::stream_direction::bidirectional)
            ++open_bidi_calls;
        else
            ++open_uni_calls;
        ++stats.active_streams;
        return direction == qb::io::quic::stream_direction::bidirectional ? 0 : 2;
    }

    void send_stream_data(std::uint64_t, std::uint64_t, std::span<const std::byte>,
                          bool) override {
        ++send_stream_data_calls;
        if (close_on_send_stream_data) {
            queued_events.push_back({
                qb::io::quic::backend_event::kind::connection_closed, 0, 0, 77,
                "closed while sending stream data", {}});
        }
    }

    void extend_stream_credit(std::uint64_t, std::uint64_t stream_id,
                              std::uint64_t bytes) override {
        ++extend_stream_credit_calls;
        extended_stream_id = stream_id;
        extended_bytes += bytes;
    }

    void send_datagram(std::uint64_t, std::span<const std::byte>) override {
        ++send_datagram_calls;
    }

    void reset_stream(std::uint64_t, std::uint64_t stream_id,
                      std::uint64_t application_error_code) override {
        ++reset_stream_calls;
        reset_stream_id = stream_id;
        reset_stream_code = application_error_code;
        queued_events.push_back({
            qb::io::quic::backend_event::kind::stream_closed, 0, stream_id,
            application_error_code, "stream reset", {},
            qb::io::quic::disconnect_reason::none,
            qb::io::quic::stream_close_reason::reset});
    }

    void close(std::uint64_t application_error_code, std::string_view reason) override {
        ++close_calls;
        close_code = application_error_code;
        close_reason.assign(reason);
        stats.active_connections = 0;
        stats.active_streams = 0;
    }

    qb::io::quic::stats current_stats() const noexcept override {
        return stats;
    }
};

class CallbackQuicClient : public qb::io::async::quic::connector<CallbackQuicClient> {
public:
    int connected = 0;
    int closed = 0;
    int stream_started = 0;
    int stream_closed = 0;
    qb::io::quic::stream_close_reason last_stream_close_reason =
        qb::io::quic::stream_close_reason::none;
    qb::io::quic::disconnect_reason last_close_reason =
        qb::io::quic::disconnect_reason::none;
    std::uint64_t last_error_code = 0;
    std::string last_reason_phrase;

    CallbackQuicClient() = default;

    explicit CallbackQuicClient(std::unique_ptr<qb::io::quic::backend> backend)
        : connector(std::move(backend)) {}

    void on(qb::io::async::quic::event::connected const&) { ++connected; }
    void on(qb::io::async::quic::event::connection_closed const& ev) {
        ++closed;
        last_close_reason = ev.reason;
        last_error_code = ev.error_code;
        last_reason_phrase = ev.reason_phrase;
    }
    void on(qb::io::async::quic::event::stream_started const&) { ++stream_started; }
    void on(qb::io::async::quic::event::stream_closed const& ev) {
        ++stream_closed;
        last_stream_close_reason = ev.reason;
    }
};

struct DummyQuicStreamSession {};

class CallbackQuicServer
    : public qb::io::async::quic::server<CallbackQuicServer, DummyQuicStreamSession> {
public:
    int connected = 0;
    int stream_started = 0;
    std::string received;
    std::string datagram_received;

    CallbackQuicServer() = default;

    explicit CallbackQuicServer(std::unique_ptr<qb::io::quic::backend> backend)
        : server(std::move(backend)) {}

    void on(qb::io::async::quic::event::connected const&) { ++connected; }
    void on(qb::io::async::quic::event::stream_started const&) { ++stream_started; }
    void on(qb::io::async::quic::event::stream_data const& ev) {
        received.append(ev.payload.data(), ev.payload.size());
    }
    void on(qb::io::async::quic::event::datagram const& ev) {
        datagram_received.append(ev.payload.data(), ev.payload.size());
    }
};

} // namespace

TEST(QuicAvailabilityTest, ReportsUnavailableWithoutBackend) {
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

TEST(QuicEndpointTest, UsesNativeBackendWhenNoBackendIsInjected) {
    qb::io::async::quic::endpoint endpoint;

#ifdef QB_HAS_QUIC
    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::connecting);
    ASSERT_NE(endpoint.backend(), nullptr);
#else
    EXPECT_THROW(
        (void)endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}),
        std::runtime_error);
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::idle);
#endif
}

TEST(QuicStreamTest, StreamDescriptorCarriesMetadataOnly) {
    qb::io::async::quic::stream stream{
        0, qb::io::quic::stream_direction::bidirectional, qb::io::quic::stream_origin::local};

    EXPECT_TRUE(stream.is_open());
    EXPECT_EQ(stream.connection_id(), 0u);
    EXPECT_EQ(stream.id(), 0u);
    EXPECT_EQ(stream.direction(), qb::io::quic::stream_direction::bidirectional);
    EXPECT_EQ(stream.origin(), qb::io::quic::stream_origin::local);

    stream.reset(42);
    EXPECT_FALSE(stream.is_open());

    qb::io::async::quic::stream remote_stream{
        9, 4, qb::io::quic::stream_direction::bidirectional,
        qb::io::quic::stream_origin::remote};
    EXPECT_EQ(remote_stream.connection_id(), 9u);
    EXPECT_EQ(remote_stream.id(), 4u);
}

TEST(QuicEndpointTest, DelegatesClientLifecycleToBackend) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    qb::io::quic::settings settings;
    settings.idle_timeout_ms = 1234;
    endpoint.set_settings(settings);

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::connecting);
    EXPECT_EQ(raw_backend->configure_calls, 1);
    EXPECT_EQ(raw_backend->start_client_calls, 1);
    ASSERT_EQ(raw_backend->last_alpn.size(), 1u);
    EXPECT_EQ(raw_backend->last_alpn.front(), "h3");
    EXPECT_EQ(raw_backend->last_settings.idle_timeout_ms, 1234u);
    EXPECT_EQ(raw_backend->last_local.af(), AF_INET);
    EXPECT_EQ(raw_backend->last_remote.port(), 4433);
    EXPECT_EQ(raw_backend->last_tls.server_name, "127.0.0.1");
    EXPECT_EQ(endpoint.stats().active_connections, 1u);

    auto bidi = endpoint.open_bidirectional_stream();
    auto uni = endpoint.open_unidirectional_stream();
    EXPECT_TRUE(bidi.is_open());
    EXPECT_TRUE(uni.is_open());
    EXPECT_EQ(bidi.id(), 0u);
    EXPECT_EQ(uni.id(), 2u);
    EXPECT_EQ(raw_backend->open_bidi_calls, 1);
    EXPECT_EQ(raw_backend->open_uni_calls, 1);
    EXPECT_EQ(endpoint.stats().active_streams, 2u);

    endpoint.close(42, "done");
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::closed);
    EXPECT_EQ(raw_backend->close_calls, 1);
    EXPECT_EQ(raw_backend->close_code, 42u);
    EXPECT_EQ(raw_backend->close_reason, "done");
}

TEST(QuicEndpointTest, DelegatesServerLifecycleToBackend) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(endpoint.listen(qb::io::uri{"quic://0.0.0.0:4433"}, "cert.pem", "key.pem"));
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::listening);
    EXPECT_EQ(raw_backend->configure_calls, 1);
    EXPECT_EQ(raw_backend->start_server_calls, 1);
    EXPECT_EQ(raw_backend->last_local.port(), 4433);
    ASSERT_EQ(raw_backend->last_alpn.size(), 1u);
    EXPECT_EQ(raw_backend->last_alpn.front(), "h3");
    EXPECT_EQ(raw_backend->last_tls.certificate_file, std::filesystem::path{"cert.pem"});
    EXPECT_EQ(raw_backend->last_tls.private_key_file, std::filesystem::path{"key.pem"});
}

TEST(QuicEndpointTest, DispatchesBackendEventsToDerivedCallbacks) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicClient client{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::connected, 0, 0, 0, "h3", {}});
    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_started, 0, 0, 0, {}, {}});
    client.poll();

    EXPECT_EQ(client.current_state(), qb::io::async::quic::endpoint::state::connected);
    EXPECT_EQ(client.connected, 1);
    EXPECT_EQ(client.stream_started, 1);

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::connection_closed, 0, 0, 42, "done", {}});
    client.poll();

    EXPECT_EQ(client.current_state(), qb::io::async::quic::endpoint::state::closed);
    EXPECT_EQ(client.closed, 1);
    EXPECT_EQ(client.last_close_reason, qb::io::quic::disconnect_reason::transport_error);
    EXPECT_EQ(client.last_error_code, 42u);
}

TEST(QuicEndpointTest, PreservesTypedBackendCloseReason) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicClient client{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    qb::io::quic::backend_event closed;
    closed.type = qb::io::quic::backend_event::kind::connection_closed;
    closed.error_code = 7;
    closed.text = "idle";
    closed.connection_reason = qb::io::quic::disconnect_reason::idle_timeout;
    raw_backend->queued_events.push_back(std::move(closed));

    client.poll();

    EXPECT_EQ(client.closed, 1);
    EXPECT_EQ(client.last_close_reason, qb::io::quic::disconnect_reason::idle_timeout);
    EXPECT_EQ(client.last_error_code, 7u);
    EXPECT_EQ(client.last_reason_phrase, "idle");
}

TEST(QuicEndpointTest, UdpTxBudgetKeepsPendingPacketsForNextPoll) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::quic::settings settings;
    settings.udp_tx_batch_size = 1;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend), settings};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    ASSERT_TRUE(endpoint.is_open());

    qb::io::quic::packet first;
    first.remote = raw_backend->last_remote;
    first.local = raw_backend->last_local;
    first.payload.assign(1, std::byte{'a'});
    qb::io::quic::packet second = first;
    second.payload.assign(1, std::byte{'b'});
    raw_backend->queued_packets.push_back(std::move(first));
    raw_backend->queued_packets.push_back(std::move(second));

    endpoint.poll();

    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::connecting);

    endpoint.poll();

    EXPECT_TRUE(endpoint.is_open());
}

TEST(QuicEndpointTest, ResetStreamDelegatesAndPreservesCloseReason) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicClient client{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    client.reset_stream(12, 99);

    EXPECT_EQ(raw_backend->reset_stream_calls, 1);
    EXPECT_EQ(raw_backend->reset_stream_id, 12u);
    EXPECT_EQ(raw_backend->reset_stream_code, 99u);
    EXPECT_EQ(client.stream_closed, 1);
    EXPECT_EQ(client.last_stream_close_reason, qb::io::quic::stream_close_reason::reset);
}

TEST(QuicEndpointTest, DelegatesDatagramSendToBackend) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    endpoint.send_datagram("datagram");

    EXPECT_EQ(raw_backend->send_datagram_calls, 1);
}

TEST(QuicEndpointTest, ListenerClearDoesNotDanglingQuicEndpointWatchers) {
    qb::io::async::init();

    auto *raw_backend = new FakeQuicBackend;
    {
        qb::io::async::quic::endpoint endpoint{
            std::unique_ptr<qb::io::quic::backend>(raw_backend)};

        ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
        ASSERT_TRUE(endpoint.is_open());
        ASSERT_GT(qb::io::async::listener::current.size(), 0u);

        qb::io::async::listener::current.clear();
        EXPECT_EQ(qb::io::async::listener::current.size(), 0u);
    }

    EXPECT_EQ(qb::io::async::listener::current.size(), 0u);
}

TEST(QuicEndpointTest, NativeClientAndServerCompleteLocalHandshake) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    if (!std::filesystem::exists(ssl_resource_path("cert.pem")) ||
        !std::filesystem::exists(ssl_resource_path("key.pem")))
        GTEST_SKIP() << "Test SSL certificates are not available";

    qb::io::async::init();

    qb::io::quic::settings settings;
    settings.enable_datagrams = true;
    settings.max_datagram_frame_size = 1200;

    CallbackQuicServer server;
    server.set_settings(settings);
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"},
                              ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"),
                              {"h3"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    qb::io::async::quic::endpoint client;
    client.set_settings(settings);
    const auto uri = std::string{"quic://127.0.0.1:"} +
                     std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(client.connect(qb::io::uri{uri}, client_tls, {"h3"}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((server.current_state() != qb::io::async::quic::endpoint::state::connected ||
            client.current_state() != qb::io::async::quic::endpoint::state::connected) &&
           std::chrono::steady_clock::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(server.current_state(), qb::io::async::quic::endpoint::state::connected);
    EXPECT_EQ(client.current_state(), qb::io::async::quic::endpoint::state::connected);
    EXPECT_EQ(server.stats().active_connections, 1u);
    EXPECT_EQ(client.stats().active_connections, 1u);
    EXPECT_EQ(server.connected, 1);

    auto stream = client.open_bidirectional_stream();
    client.send_stream_data(stream.id(), "ping", true);

    const auto stream_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (server.received != "ping" && std::chrono::steady_clock::now() < stream_deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(server.received, "ping");
    EXPECT_GE(server.stream_started, 1);

    client.send_datagram("capsule");

    const auto datagram_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (server.datagram_received != "capsule" &&
           std::chrono::steady_clock::now() < datagram_deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(server.datagram_received, "capsule");
    EXPECT_GE(client.stats().datagrams_sent, 1u);
    EXPECT_GE(server.stats().datagrams_received, 1u);

    client.close();
    server.close();
    qb::io::async::listener::current.clear();
#endif
}

TEST(QuicEndpointTest, NativeServerRoutesMultipleClientsByConnectionId) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    if (!std::filesystem::exists(ssl_resource_path("cert.pem")) ||
        !std::filesystem::exists(ssl_resource_path("key.pem")))
        GTEST_SKIP() << "Test SSL certificates are not available";

    qb::io::async::init();

    CallbackQuicServer server;
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"},
                              ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"),
                              {"h3"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    CallbackQuicClient first;
    CallbackQuicClient second;
    const auto uri = std::string{"quic://127.0.0.1:"} +
                     std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(first.connect(qb::io::uri{uri}, client_tls, {"h3"}));
    ASSERT_TRUE(second.connect(qb::io::uri{uri}, client_tls, {"h3"}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((first.current_state() != qb::io::async::quic::endpoint::state::connected ||
            second.current_state() != qb::io::async::quic::endpoint::state::connected ||
            server.connected < 2) &&
           std::chrono::steady_clock::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(first.current_state(), qb::io::async::quic::endpoint::state::connected);
    EXPECT_EQ(second.current_state(), qb::io::async::quic::endpoint::state::connected);
    EXPECT_EQ(server.connected, 2);
    EXPECT_EQ(server.stats().active_connections, 2u);

    auto first_stream = first.open_bidirectional_stream();
    auto second_stream = second.open_bidirectional_stream();
    first.send_stream_data(first_stream.id(), "first-client\n", true);
    second.send_stream_data(second_stream.id(), "second-client\n", true);

    const auto stream_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((server.received.find("first-client") == std::string::npos ||
            server.received.find("second-client") == std::string::npos) &&
           std::chrono::steady_clock::now() < stream_deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_NE(server.received.find("first-client"), std::string::npos);
    EXPECT_NE(server.received.find("second-client"), std::string::npos);

    first.close();

    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!server.is_open() || server.stats().active_connections != 1u) &&
           std::chrono::steady_clock::now() < close_deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(server.is_open());
    EXPECT_EQ(server.stats().active_connections, 1u);

    EXPECT_TRUE(server.is_open());
    EXPECT_NE(server.current_state(), qb::io::async::quic::endpoint::state::closed);
    EXPECT_EQ(second.current_state(), qb::io::async::quic::endpoint::state::connected);

    second.close();
    server.close();
    qb::io::async::listener::current.clear();
#endif
}
