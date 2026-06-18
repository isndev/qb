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
    int close_connection_calls = 0;
    std::uint64_t closed_connection_id = 0;
    std::uint64_t last_open_connection_id = 0;
    int send_stream_data_calls = 0;
    std::uint64_t sent_connection_id = 0;
    std::uint64_t sent_stream_id = 0;
    std::size_t sent_stream_bytes = 0;
    bool sent_stream_fin = false;
    int extend_stream_credit_calls = 0;
    std::uint64_t extended_stream_id = 0;
    std::uint64_t extended_bytes = 0;
    int send_datagram_calls = 0;
    std::uint64_t sent_datagram_connection_id = 0;
    std::size_t sent_datagram_bytes = 0;
    int reset_stream_calls = 0;
    int stop_stream_calls = 0;
    std::uint64_t reset_stream_id = 0;
    std::uint64_t reset_stream_code = 0;
    std::uint64_t stop_stream_id = 0;
    std::uint64_t stop_stream_code = 0;
    std::uint64_t close_code = 0;
    std::string close_reason;
    bool close_on_send_stream_data = false;
    int timeout_calls = 0;
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
    void on_timeout(std::chrono::steady_clock::time_point) override { ++timeout_calls; }

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
        return open_stream(0, direction);
    }

    std::uint64_t open_stream(std::uint64_t connection_id,
                              qb::io::quic::stream_direction direction) override {
        last_open_connection_id = connection_id;
        if (direction == qb::io::quic::stream_direction::bidirectional)
            ++open_bidi_calls;
        else
            ++open_uni_calls;
        ++stats.active_streams;
        return direction == qb::io::quic::stream_direction::bidirectional ? 0 : 2;
    }

    void send_stream_data(std::uint64_t connection_id, std::uint64_t stream_id,
                          std::span<const std::byte> data, bool fin) override {
        ++send_stream_data_calls;
        sent_connection_id = connection_id;
        sent_stream_id = stream_id;
        sent_stream_bytes += data.size();
        sent_stream_fin = fin;
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

    void send_datagram(std::uint64_t connection_id, std::span<const std::byte> data) override {
        ++send_datagram_calls;
        sent_datagram_connection_id = connection_id;
        sent_datagram_bytes += data.size();
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

    void stop_stream(std::uint64_t, std::uint64_t stream_id,
                     std::uint64_t application_error_code) override {
        ++stop_stream_calls;
        stop_stream_id = stream_id;
        stop_stream_code = application_error_code;
        queued_events.push_back({
            qb::io::quic::backend_event::kind::stream_closed, 0, stream_id,
            application_error_code, "stream stopped", {},
            qb::io::quic::disconnect_reason::none,
            qb::io::quic::stream_close_reason::stop_sending});
    }

    void close(std::uint64_t application_error_code, std::string_view reason) override {
        ++close_calls;
        close_code = application_error_code;
        close_reason.assign(reason);
        stats.active_connections = 0;
        stats.active_streams = 0;
    }

    void close_connection(std::uint64_t connection_id, std::uint64_t application_error_code,
                          std::string_view reason) override {
        ++close_connection_calls;
        closed_connection_id = connection_id;
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
    int stream_data = 0;
    int stream_acked = 0;
    int stream_closed = 0;
    int datagrams = 0;
    std::string received;
    std::string datagram_received;
    qb::io::quic::stream_close_reason last_stream_close_reason =
        qb::io::quic::stream_close_reason::none;
    qb::io::quic::disconnect_reason last_close_reason =
        qb::io::quic::disconnect_reason::none;
    std::uint64_t last_error_code = 0;
    std::string last_reason_phrase;
    qb::io::quic::stream_direction last_stream_direction =
        qb::io::quic::stream_direction::bidirectional;
    qb::io::quic::stream_origin last_stream_origin = qb::io::quic::stream_origin::local;

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
    void on(qb::io::async::quic::event::stream_started const& ev) {
        ++stream_started;
        last_stream_direction = ev.direction;
        last_stream_origin = ev.origin;
    }
    void on(qb::io::async::quic::event::stream_data const& ev) {
        ++stream_data;
        received.append(ev.payload.data(), ev.payload.size());
    }
    void on(qb::io::async::quic::event::stream_data_acked const& ev) {
        ++stream_acked;
        last_error_code = ev.bytes;
    }
    void on(qb::io::async::quic::event::stream_closed const& ev) {
        ++stream_closed;
        last_stream_close_reason = ev.reason;
    }
    void on(qb::io::async::quic::event::datagram const& ev) {
        ++datagrams;
        datagram_received.append(ev.payload.data(), ev.payload.size());
    }
};

class DummyQuicStreamSession
    : public qb::io::use<DummyQuicStreamSession>::quic::session {
public:
    using Base = qb::io::use<DummyQuicStreamSession>::quic::session;
    using Base::Base;
};

class EchoQuicStreamSession
    : public qb::io::use<EchoQuicStreamSession>::quic::session {
public:
    using Base = qb::io::use<EchoQuicStreamSession>::quic::session;
    using Base::Base;

    int messages = 0;
    std::string received;
};

class EchoQuicProtocol : public qb::io::async::AProtocol<EchoQuicStreamSession> {
public:
    explicit EchoQuicProtocol(EchoQuicStreamSession& session) noexcept
        : AProtocol(session) {}

    std::size_t getMessageSize() noexcept final;
    void onMessage(std::size_t size) noexcept final;
    void reset() noexcept final {}
};

std::size_t
EchoQuicProtocol::getMessageSize() noexcept {
    return _io.pendingRead() >= 4 ? 4 : 0;
}

void
EchoQuicProtocol::onMessage(std::size_t size) noexcept {
    _io.received.append(_io.in().begin(), size);
    ++_io.messages;
    _io.publish("ack!", std::size_t{4});
}

class CallbackQuicServer
    : public qb::io::async::quic::server<CallbackQuicServer, DummyQuicStreamSession> {
public:
    int connected = 0;
    int closed = 0;
    int stream_started = 0;
    int stream_sessions = 0;
    std::uint64_t acked_bytes = 0;
    std::string received;
    std::string datagram_received;
    qb::io::quic::disconnect_reason last_close_reason =
        qb::io::quic::disconnect_reason::none;
    qb::io::quic::stream_direction last_stream_direction =
        qb::io::quic::stream_direction::bidirectional;
    qb::io::quic::stream_origin last_stream_origin = qb::io::quic::stream_origin::local;

    CallbackQuicServer() = default;

    explicit CallbackQuicServer(std::unique_ptr<qb::io::quic::backend> backend)
        : server(std::move(backend)) {}

    void on(qb::io::async::quic::event::connected const&) { ++connected; }
    void on(qb::io::async::quic::event::connection_closed const& ev) {
        ++closed;
        last_close_reason = ev.reason;
    }
    void on(qb::io::async::quic::event::stream_started const& ev) {
        ++stream_started;
        last_stream_direction = ev.direction;
        last_stream_origin = ev.origin;
    }
    void on(qb::io::async::quic::event::stream_data const& ev) {
        received.append(ev.payload.data(), ev.payload.size());
    }
    void on(qb::io::async::quic::event::stream_data_acked const& ev) {
        acked_bytes += ev.bytes;
    }
    void on(qb::io::async::quic::event::datagram const& ev) {
        datagram_received.append(ev.payload.data(), ev.payload.size());
    }
    void on(DummyQuicStreamSession&) { ++stream_sessions; }
};

class EchoQuicServer
    : public qb::io::async::quic::server<EchoQuicServer, EchoQuicStreamSession> {
public:
    using server::server;

    int sessions = 0;
    int stream_data_events = 0;

    void on(EchoQuicStreamSession& session) {
        ++sessions;
        session.switch_protocol<EchoQuicProtocol>(session);
    }
    void on(qb::io::async::quic::event::stream_data const&) { ++stream_data_events; }
};

class SessionQuicClient
    : public qb::io::async::quic::connector<SessionQuicClient, DummyQuicStreamSession> {
public:
    using connector::connector;
};

#ifdef QB_HAS_QUIC
std::array<std::byte, 4>
quic_payload() {
    return {std::byte{'q'}, std::byte{'u'}, std::byte{'i'}, std::byte{'c'}};
}

void
deliver_quic_packets(qb::io::quic::backend& from, qb::io::quic::backend& to) {
    for (auto& packet : from.drain_packets()) {
        qb::io::quic::packet_view view{
            packet.remote,
            packet.local,
            std::span<const std::byte>{packet.payload.data(), packet.payload.size()}};
        to.on_udp_datagram(view);
    }
}
#endif

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

TEST(QuicNativeBackendTest, RejectsInvalidAlpnBeforeOpeningNativeResources) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    auto backend = qb::io::quic::make_native_backend();
    qb::io::quic::tls_config tls;
    tls.verify_peer = false;
    const qb::io::endpoint local{"127.0.0.1", 0};
    const qb::io::endpoint remote{"127.0.0.1", 4433};

    EXPECT_THROW(backend->start_client(local, remote, {}, tls), std::invalid_argument);
    EXPECT_THROW(backend->start_client(local, remote, {""}, tls), std::invalid_argument);
    EXPECT_THROW(
        backend->start_client(local, remote, {std::string(256, 'x')}, tls),
        std::invalid_argument);
#endif
}

TEST(QuicNativeBackendTest, ValidatesServerTlsConfiguration) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    auto backend = qb::io::quic::make_native_backend();
    const qb::io::endpoint local{"127.0.0.1", 0};

    qb::io::quic::tls_config missing_files;
    EXPECT_THROW(
        backend->start_server(local, {"h3"}, missing_files),
        std::invalid_argument);

    qb::io::quic::tls_config unreadable_files;
    unreadable_files.certificate_file = ssl_resource_path("missing-cert.pem");
    unreadable_files.private_key_file = ssl_resource_path("missing-key.pem");
    EXPECT_THROW(
        backend->start_server(local, {"h3"}, unreadable_files),
        std::runtime_error);
#endif
}

TEST(QuicNativeBackendTest, PreConnectionOperationsAreStableAndReportFailures) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    auto backend = qb::io::quic::make_native_backend();
    const qb::io::endpoint local{"127.0.0.1", 0};
    const qb::io::endpoint remote{"127.0.0.1", 4433};
    auto payload = quic_payload();

    EXPECT_FALSE(backend->wants_write());
    EXPECT_EQ(backend->next_timeout(), std::chrono::steady_clock::time_point::max());
    EXPECT_TRUE(backend->drain_packets().empty());
    EXPECT_TRUE(backend->drain_events().empty());
    EXPECT_EQ(backend->current_stats().active_connections, 0u);
    EXPECT_THROW(
        {
            auto stream_id =
                backend->open_stream(qb::io::quic::stream_direction::bidirectional);
            (void)stream_id;
        },
        std::runtime_error);

    qb::io::quic::packet_view datagram{
        remote, local, std::span<const std::byte>{payload.data(), payload.size()}};
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
    EXPECT_EQ(events.front().connection_reason,
              qb::io::quic::disconnect_reason::application_close);
#endif
}

TEST(QuicNativeBackendTest, EnforcesPreConnectionSendQueueLimits) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    auto payload = quic_payload();

    {
        auto backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.max_pending_stream_frames = 0;
        settings.max_pending_stream_bytes = 3;
        backend->configure(settings);
        backend->send_stream_data(0, 1, std::span<const std::byte>{payload}, false);

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason,
                  qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("byte queue"), std::string::npos);
    }

    {
        auto backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.max_pending_stream_frames = 0;
        settings.max_pending_stream_bytes = 64;
        backend->configure(settings);
        backend->send_stream_data(0, 1, std::span<const std::byte>{payload}, false);
        EXPECT_TRUE(backend->wants_write());

        settings.max_pending_stream_frames = 1;
        backend->configure(settings);
        backend->send_stream_data(0, 1, std::span<const std::byte>{payload}, false);

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason,
                  qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("frame queue"), std::string::npos);
    }

    {
        auto backend = qb::io::quic::make_native_backend();
        backend->send_datagram(0, {});
        EXPECT_TRUE(backend->drain_events().empty());

        backend->send_datagram(0, std::span<const std::byte>{payload});
        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason,
                  qb::io::quic::disconnect_reason::protocol_error);
    }

    {
        auto backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.enable_datagrams = true;
        settings.max_datagram_frame_size = 3;
        backend->configure(settings);
        backend->send_datagram(0, std::span<const std::byte>{payload});

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason,
                  qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("configured maximum"), std::string::npos);
    }

    {
        auto backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.enable_datagrams = true;
        settings.max_datagram_frame_size = 64;
        settings.max_pending_datagram_frames = 1;
        backend->configure(settings);
        backend->send_datagram(0, std::span<const std::byte>{payload});
        backend->send_datagram(0, std::span<const std::byte>{payload});

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason,
                  qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("frame queue"), std::string::npos);
    }

    {
        auto backend = qb::io::quic::make_native_backend();
        qb::io::quic::settings settings;
        settings.enable_datagrams = true;
        settings.max_datagram_frame_size = 64;
        settings.max_pending_datagram_frames = 0;
        settings.max_pending_datagram_bytes = 3;
        backend->configure(settings);
        backend->send_datagram(0, std::span<const std::byte>{payload});

        auto events = backend->drain_events();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().connection_reason,
                  qb::io::quic::disconnect_reason::buffer_overflow);
        EXPECT_NE(events.front().text.find("byte queue"), std::string::npos);
    }

#endif
}

TEST(QuicNativeBackendTest, ServerParentNoConnectionPathsRemainOpen) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    if (!std::filesystem::exists(ssl_resource_path("cert.pem")) ||
        !std::filesystem::exists(ssl_resource_path("key.pem")))
        GTEST_SKIP() << "Test SSL certificates are not available";

    auto backend = qb::io::quic::make_native_backend();
    const qb::io::endpoint local{"127.0.0.1", 0};
    qb::io::quic::tls_config tls;
    tls.certificate_file = ssl_resource_path("cert.pem");
    tls.private_key_file = ssl_resource_path("key.pem");
    backend->start_server(local, {"h3"}, tls);

    auto payload = quic_payload();
    qb::io::quic::packet_view empty_datagram{
        local, local, std::span<const std::byte>{}};
    qb::io::quic::packet_view malformed_datagram{
        local, local, std::span<const std::byte>{payload.data(), payload.size()}};

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
        {
            auto stream_id =
                backend->open_stream(99, qb::io::quic::stream_direction::bidirectional);
            (void)stream_id;
        },
        std::runtime_error);

    backend->close(5, "server closed");
    auto events = backend->drain_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().connection_reason,
              qb::io::quic::disconnect_reason::application_close);
    EXPECT_EQ(events.front().error_code, 5u);
#endif
}

TEST(QuicNativeBackendTest, DirectBackendsRouteServerChildOperations) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    if (!std::filesystem::exists(ssl_resource_path("cert.pem")) ||
        !std::filesystem::exists(ssl_resource_path("key.pem")))
        GTEST_SKIP() << "Test SSL certificates are not available";

    qb::io::quic::settings settings;
    settings.enable_datagrams = true;
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

    bool client_connected = false;
    std::uint64_t server_connection_id = 0;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_connected || server_connection_id == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        deliver_quic_packets(*client, *server);
        deliver_quic_packets(*server, *client);

        for (auto const& event : client->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::connected)
                client_connected = true;
        }
        for (auto const& event : server->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::connected)
                server_connection_id = event.connection_id;
        }
    }

    ASSERT_TRUE(client_connected);
    ASSERT_NE(server_connection_id, 0u);
    EXPECT_EQ(server->current_stats().active_connections, 1u);

    auto explicit_stream =
        server->open_stream(server_connection_id,
                            qb::io::quic::stream_direction::bidirectional);
    auto implicit_stream =
        server->open_stream(0, qb::io::quic::stream_direction::unidirectional);
    EXPECT_NE(explicit_stream, implicit_stream);

    auto payload = quic_payload();
    server->send_stream_data(
        server_connection_id, explicit_stream, std::span<const std::byte>{payload}, false);
    server->send_datagram(server_connection_id, std::span<const std::byte>{payload});
    server->reset_stream(server_connection_id, explicit_stream, 17);
    server->stop_stream(server_connection_id, implicit_stream, 18);
    server->extend_stream_credit(server_connection_id, explicit_stream, 32);

    server->close_connection(server_connection_id, 19, "server child closed");
    auto events = server->drain_events();
    auto closed = std::find_if(events.begin(), events.end(), [](auto const& event) {
        return event.type == qb::io::quic::backend_event::kind::connection_closed;
    });
    ASSERT_NE(closed, events.end());
    EXPECT_EQ(closed->connection_id, server_connection_id);
    EXPECT_EQ(closed->error_code, 19u);
    EXPECT_EQ(closed->connection_reason,
              qb::io::quic::disconnect_reason::application_close);
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
    settings.idle_timeout = std::chrono::milliseconds(1234);
    endpoint.set_settings(settings);

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));
    EXPECT_TRUE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::connecting);
    EXPECT_EQ(raw_backend->configure_calls, 1);
    EXPECT_EQ(raw_backend->start_client_calls, 1);
    ASSERT_EQ(raw_backend->last_alpn.size(), 1u);
    EXPECT_EQ(raw_backend->last_alpn.front(), "h3");
    EXPECT_EQ(raw_backend->last_settings.idle_timeout, std::chrono::milliseconds(1234));
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

TEST(QuicEndpointTest, PassesBackpressureAndLifecycleSettingsToBackend) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::quic::settings settings;
    settings.handshake_timeout = std::chrono::milliseconds(111);
    settings.idle_timeout = std::chrono::milliseconds(222);
    settings.stream_recv_window = 333;
    settings.connection_recv_window = 444;
    settings.max_stream_data_bidi_local = 555;
    settings.max_stream_data_bidi_remote = 666;
    settings.max_stream_data_uni = 777;
    settings.max_streams_bidi = 8;
    settings.max_streams_uni = 9;
    settings.max_datagram_frame_size = 1200;
    settings.max_connections = 10;
    settings.max_pending_stream_bytes = 11;
    settings.max_pending_stream_frames = 12;
    settings.max_pending_datagram_bytes = 13;
    settings.max_pending_datagram_frames = 14;
    settings.udp_rx_batch_size = 15;
    settings.udp_tx_batch_size = 16;
    settings.enable_stateless_retry = false;
    settings.enable_datagrams = true;
    settings.enable_keylog = true;

    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend), settings};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    EXPECT_EQ(raw_backend->configure_calls, 1);
    EXPECT_EQ(raw_backend->last_settings.handshake_timeout, std::chrono::milliseconds(111));
    EXPECT_EQ(raw_backend->last_settings.idle_timeout, std::chrono::milliseconds(222));
    EXPECT_EQ(raw_backend->last_settings.stream_recv_window, 333u);
    EXPECT_EQ(raw_backend->last_settings.connection_recv_window, 444u);
    EXPECT_EQ(raw_backend->last_settings.max_stream_data_bidi_local, 555u);
    EXPECT_EQ(raw_backend->last_settings.max_stream_data_bidi_remote, 666u);
    EXPECT_EQ(raw_backend->last_settings.max_stream_data_uni, 777u);
    EXPECT_EQ(raw_backend->last_settings.max_streams_bidi, 8u);
    EXPECT_EQ(raw_backend->last_settings.max_streams_uni, 9u);
    EXPECT_EQ(raw_backend->last_settings.max_datagram_frame_size, 1200u);
    EXPECT_EQ(raw_backend->last_settings.max_connections, 10u);
    EXPECT_EQ(raw_backend->last_settings.max_pending_stream_bytes, 11u);
    EXPECT_EQ(raw_backend->last_settings.max_pending_stream_frames, 12u);
    EXPECT_EQ(raw_backend->last_settings.max_pending_datagram_bytes, 13u);
    EXPECT_EQ(raw_backend->last_settings.max_pending_datagram_frames, 14u);
    EXPECT_EQ(raw_backend->last_settings.udp_rx_batch_size, 15u);
    EXPECT_EQ(raw_backend->last_settings.udp_tx_batch_size, 16u);
    EXPECT_FALSE(raw_backend->last_settings.enable_stateless_retry);
    EXPECT_TRUE(raw_backend->last_settings.enable_datagrams);
    EXPECT_TRUE(raw_backend->last_settings.enable_keylog);
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

TEST(QuicEndpointTest, BaseEndpointAcceptsAllBackendEventsAsNoopCallbacks) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::connected, 3, 0, 0, "h3", {}});
    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_started, 3, 0, 0, {}, {}});

    qb::io::quic::backend_event data;
    data.type = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 3;
    data.stream_id = 0;
    data.payload = {std::byte{'o'}, std::byte{'k'}};
    raw_backend->queued_events.push_back(std::move(data));

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_data_acked, 3, 0, 2, {}, {}});
    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_closed, 3, 0, 9, {}, {},
        qb::io::quic::disconnect_reason::none,
        qb::io::quic::stream_close_reason::finished});

    qb::io::quic::backend_event datagram;
    datagram.type = qb::io::quic::backend_event::kind::datagram;
    datagram.connection_id = 3;
    datagram.payload = {std::byte{'d'}};
    raw_backend->queued_events.push_back(std::move(datagram));

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::connection_closed, 3, 0, 0, "closed", {},
        qb::io::quic::disconnect_reason::application_close});

    endpoint.poll();

    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::closed);
}

TEST(QuicEndpointTest, StreamStartedEventsExposeDirectionAndOriginForClientRole) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicClient client{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_started, 9, 0, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::bidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::local);

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_started, 9, 1, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::bidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::remote);

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_started, 9, 2, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::unidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::local);

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_started, 9, 3, 0, {}, {}});
    client.poll();
    EXPECT_EQ(client.last_stream_direction, qb::io::quic::stream_direction::unidirectional);
    EXPECT_EQ(client.last_stream_origin, qb::io::quic::stream_origin::remote);
}

TEST(QuicEndpointTest, ConnectorWithoutStreamSessionDispatchesAllEventKinds) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicClient client{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    qb::io::quic::backend_event data;
    data.type = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 9;
    data.stream_id = 4;
    data.payload = {std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
    raw_backend->queued_events.push_back(std::move(data));

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_data_acked, 9, 4, 123, {}, {}});

    qb::io::quic::backend_event datagram;
    datagram.type = qb::io::quic::backend_event::kind::datagram;
    datagram.connection_id = 9;
    datagram.payload = {std::byte{'d'}, std::byte{'g'}};
    raw_backend->queued_events.push_back(std::move(datagram));

    client.poll();

    EXPECT_EQ(client.stream_data, 1);
    EXPECT_EQ(client.received, "ping");
    EXPECT_EQ(client.stream_acked, 1);
    EXPECT_EQ(client.last_error_code, 123u);
    EXPECT_EQ(client.datagrams, 1);
    EXPECT_EQ(client.datagram_received, "dg");
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

TEST(QuicEndpointTest, EndpointNoBackendOperationsAreStableNoops) {
    qb::io::async::quic::endpoint endpoint;

    EXPECT_EQ(endpoint.backend(), nullptr);
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::idle);

    endpoint.poll();
    endpoint.extend_stream_credit(1, 2, 3);
    endpoint.close_connection(1, 2, "ignored");
    endpoint.close(7, "closed");

    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(endpoint.current_state(), qb::io::async::quic::endpoint::state::closed);
}

TEST(QuicEndpointTest, InvalidRemoteUriFailsBeforeStartingBackend) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    EXPECT_FALSE(endpoint.connect(qb::io::uri{"quic:///missing-host"}));
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(raw_backend->configure_calls, 0);
    EXPECT_EQ(raw_backend->start_client_calls, 0);
}

TEST(QuicEndpointTest, ListenReturnsFalseWhenBindPortIsUnavailable) {
    qb::io::udp::socket occupied;
    ASSERT_EQ(occupied.bind_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = occupied.local_endpoint().port();
    ASSERT_NE(port, 0);

    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    EXPECT_FALSE(endpoint.listen(
        qb::io::uri{"quic://127.0.0.1:" + std::to_string(port)}, "cert.pem", "key.pem"));
    EXPECT_FALSE(endpoint.is_open());
    EXPECT_EQ(raw_backend->configure_calls, 0);
    EXPECT_EQ(raw_backend->start_server_calls, 0);
}

TEST(QuicEndpointTest, ExplicitTlsConnectFillsServerNameWhenMissing) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    qb::io::quic::tls_config tls;
    tls.verify_peer = false;

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}, tls, {"qb"}));
    EXPECT_EQ(raw_backend->last_tls.server_name, "127.0.0.1");
    ASSERT_EQ(raw_backend->last_alpn.size(), 1u);
    EXPECT_EQ(raw_backend->last_alpn.front(), "qb");
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

TEST(QuicEndpointTest, StopStreamDelegatesAndPreservesCloseReason) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicClient client{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    client.stop_stream(12, 99);

    EXPECT_EQ(raw_backend->stop_stream_calls, 1);
    EXPECT_EQ(raw_backend->stop_stream_id, 12u);
    EXPECT_EQ(raw_backend->stop_stream_code, 99u);
    EXPECT_EQ(client.stream_closed, 1);
    EXPECT_EQ(client.last_stream_close_reason, qb::io::quic::stream_close_reason::stop_sending);
}

TEST(QuicEndpointTest, DelegatesDatagramSendToBackend) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    endpoint.send_datagram("datagram");

    EXPECT_EQ(raw_backend->send_datagram_calls, 1);
}

TEST(QuicEndpointTest, DatagramSendCanTargetExplicitConnection) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(endpoint.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    endpoint.send_datagram(42, "datagram");

    EXPECT_EQ(raw_backend->send_datagram_calls, 1);
    EXPECT_EQ(raw_backend->sent_datagram_connection_id, 42u);
    EXPECT_EQ(raw_backend->sent_datagram_bytes, 8u);
}

TEST(QuicEndpointTest, EndpointMutationOverloadsDelegateToBackend) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    endpoint.send_stream_data(5, "abc", true);
    endpoint.send_stream_data(7, 8, "de", false);

    std::array<std::byte, 3> datagram{std::byte{'x'}, std::byte{'y'}, std::byte{'z'}};
    endpoint.send_datagram(std::span<const std::byte>{datagram});
    endpoint.send_datagram(99, std::span<const std::byte>{datagram});

    endpoint.extend_stream_credit(1, 2, 0);
    EXPECT_EQ(raw_backend->extend_stream_credit_calls, 0);

    endpoint.extend_stream_credit(1, 2, 4);

    EXPECT_EQ(raw_backend->send_stream_data_calls, 2);
    EXPECT_EQ(raw_backend->sent_connection_id, 7u);
    EXPECT_EQ(raw_backend->sent_stream_id, 8u);
    EXPECT_EQ(raw_backend->sent_stream_bytes, 5u);
    EXPECT_FALSE(raw_backend->sent_stream_fin);
    EXPECT_EQ(raw_backend->send_datagram_calls, 2);
    EXPECT_EQ(raw_backend->sent_datagram_connection_id, 99u);
    EXPECT_EQ(raw_backend->sent_datagram_bytes, 6u);
    EXPECT_EQ(raw_backend->extend_stream_credit_calls, 1);
    EXPECT_EQ(raw_backend->extended_stream_id, 2u);
    EXPECT_EQ(raw_backend->extended_bytes, 4u);
}

TEST(QuicEndpointTest, LocalStreamSessionCanPublishAndFlush) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *session = server.open_bidirectional_stream_session(42);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->connection_id(), 42u);
    EXPECT_EQ(session->id(), 0u);

    session->publish(std::string_view{"hello", 5});
    EXPECT_TRUE(server.flush_stream_session(42, session->id()));

    EXPECT_EQ(raw_backend->open_bidi_calls, 1);
    EXPECT_EQ(raw_backend->send_stream_data_calls, 1);
    EXPECT_EQ(raw_backend->sent_connection_id, 42u);
    EXPECT_EQ(raw_backend->sent_stream_id, session->id());
    EXPECT_EQ(raw_backend->sent_stream_bytes, 5u);
    EXPECT_FALSE(raw_backend->sent_stream_fin);
    EXPECT_EQ(session->pendingWrite(), 0u);
}

TEST(QuicEndpointTest, ConnectorLocalStreamSessionCanPublishAndFlush) {
    auto *raw_backend = new FakeQuicBackend;
    SessionQuicClient client{std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    auto *session = client.open_unidirectional_stream_session();
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->connection_id(), 0u);
    EXPECT_EQ(session->id(), 2u);

    session->publish(std::string_view{"client-data", 11});
    EXPECT_TRUE(client.flush_stream_session(session->id()));

    EXPECT_EQ(raw_backend->open_uni_calls, 1);
    EXPECT_EQ(raw_backend->send_stream_data_calls, 1);
    EXPECT_EQ(raw_backend->sent_connection_id, 0u);
    EXPECT_EQ(raw_backend->sent_stream_id, session->id());
    EXPECT_EQ(raw_backend->sent_stream_bytes, 11u);
    EXPECT_FALSE(raw_backend->sent_stream_fin);
    EXPECT_EQ(session->pendingWrite(), 0u);
}

TEST(QuicEndpointTest, ConnectorStreamSessionConvenienceOverloadsUseExplicitConnection) {
    auto *raw_backend = new FakeQuicBackend;
    SessionQuicClient client{std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(client.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    auto *session = client.open_bidirectional_stream_session(42);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(raw_backend->last_open_connection_id, 42u);
    EXPECT_EQ(session->connection_id(), 42u);
    EXPECT_EQ(session->id(), 0u);
    EXPECT_EQ(client.stream_session(session->id()), session);
    EXPECT_EQ(client.stream_session(42, session->id()), session);

    session->publish(std::string_view{"client", 6});
    EXPECT_TRUE(client.flush_stream_session(42, session->id()));
    EXPECT_EQ(raw_backend->send_stream_data_calls, 1);
    EXPECT_EQ(raw_backend->sent_connection_id, 42u);
    EXPECT_EQ(raw_backend->sent_stream_id, session->id());
    EXPECT_EQ(raw_backend->sent_stream_bytes, 6u);
    EXPECT_FALSE(raw_backend->sent_stream_fin);

    EXPECT_TRUE(client.finish_stream_session(session->id()));
    EXPECT_EQ(raw_backend->send_stream_data_calls, 2);
    EXPECT_EQ(raw_backend->sent_connection_id, 42u);
    EXPECT_EQ(raw_backend->sent_stream_id, session->id());
    EXPECT_TRUE(raw_backend->sent_stream_fin);

    EXPECT_FALSE(client.flush_stream_session(99, session->id()));
    EXPECT_FALSE(client.finish_stream_session(99, session->id()));
}

TEST(QuicEndpointTest, ServerStreamSessionConvenienceOverloadsUseExplicitConnection) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *session = server.open_unidirectional_stream_session(77);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(raw_backend->last_open_connection_id, 77u);
    EXPECT_EQ(raw_backend->open_uni_calls, 1);
    EXPECT_EQ(session->connection_id(), 77u);
    EXPECT_EQ(session->id(), 2u);
    EXPECT_EQ(server.stream_session(session->id()), session);
    EXPECT_EQ(server.stream_session(77, session->id()), session);

    session->publish(std::string_view{"server", 6});
    server.flush_stream_session(*session);
    EXPECT_EQ(raw_backend->send_stream_data_calls, 1);
    EXPECT_EQ(raw_backend->sent_connection_id, 77u);
    EXPECT_EQ(raw_backend->sent_stream_id, session->id());
    EXPECT_EQ(raw_backend->sent_stream_bytes, 6u);
    EXPECT_FALSE(raw_backend->sent_stream_fin);

    server.finish_stream_session(*session);
    EXPECT_EQ(raw_backend->send_stream_data_calls, 2);
    EXPECT_EQ(raw_backend->sent_connection_id, 77u);
    EXPECT_EQ(raw_backend->sent_stream_id, session->id());
    EXPECT_TRUE(raw_backend->sent_stream_fin);
}

TEST(QuicEndpointTest, FinishLocalStreamSessionFlushesAndSendsFin) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *session = server.open_bidirectional_stream_session(42);
    ASSERT_NE(session, nullptr);
    session->publish(std::string_view{"done", 4});

    EXPECT_TRUE(server.finish_stream_session(42, session->id()));

    EXPECT_EQ(raw_backend->send_stream_data_calls, 2);
    EXPECT_EQ(raw_backend->sent_connection_id, 42u);
    EXPECT_EQ(raw_backend->sent_stream_id, session->id());
    EXPECT_EQ(raw_backend->sent_stream_bytes, 4u);
    EXPECT_TRUE(raw_backend->sent_stream_fin);
    EXPECT_EQ(session->pendingWrite(), 0u);
}

TEST(QuicEndpointTest, FinishMissingStreamSessionReturnsFalse) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    EXPECT_FALSE(server.finish_stream_session(777, 888));
    EXPECT_EQ(raw_backend->send_stream_data_calls, 0);
}

TEST(QuicEndpointTest, FlushMissingStreamSessionReturnsFalse) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    EXPECT_FALSE(server.flush_stream_session(777, 888));
    EXPECT_EQ(raw_backend->send_stream_data_calls, 0);
}

TEST(QuicEndpointTest, AmbiguousStreamIdLookupReturnsNullUntilConnectionIsSpecified) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *first = server.register_stream_session(1, 10);
    auto *second = server.register_stream_session(2, 10);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(server.stream_session(10), nullptr);
    EXPECT_EQ(server.stream_session(1, 10), first);
    EXPECT_EQ(server.stream_session(2, 10), second);
    EXPECT_FALSE(server.flush_stream_session(10));
    EXPECT_TRUE(server.flush_stream_session(1, 10));

    const auto &const_server = server;
    EXPECT_EQ(const_server.sessions().size(), 2u);
    EXPECT_EQ(const_server.session_count(), 2u);
}

TEST(QuicEndpointTest, ExistingSessionLookupAndSessionCapRejection) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *first = server.register_stream_session(3, 12);
    ASSERT_NE(first, nullptr);

    EXPECT_EQ(server.register_stream_session(3, 12), first);
    server.set_max_sessions(1);
    EXPECT_EQ(server.register_stream_session(3, 13), nullptr);
    EXPECT_EQ(server.max_sessions(), 1u);
}

TEST(QuicEndpointTest, ConnectionCloseClearsOnlyMatchingServerSessions) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    auto *first = server.register_stream_session(1, 10);
    auto *second = server.register_stream_session(2, 10);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(server.stream_sessions, 2);

    raw_backend->stats.active_connections = 1;
    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::connection_closed, 1, 0, 55,
        "first closed", {}, qb::io::quic::disconnect_reason::application_close});

    server.poll();

    EXPECT_TRUE(server.is_open());
    EXPECT_EQ(server.current_state(), qb::io::async::quic::endpoint::state::connected);
    EXPECT_EQ(server.closed, 1);
    EXPECT_EQ(server.last_close_reason,
              qb::io::quic::disconnect_reason::application_close);
    EXPECT_EQ(server.stream_session(1, 10), nullptr);
    EXPECT_EQ(server.stream_session(2, 10), second);
    EXPECT_EQ(server.session_count(), 1u);
}

TEST(QuicEndpointTest, ConnectionCloseWithoutRemainingServerConnectionsKeepsListenerOpen) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));
    ASSERT_NE(server.register_stream_session(7, 11), nullptr);

    raw_backend->stats.active_connections = 0;
    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::connection_closed, 7, 0, 0,
        "last client closed", {}, qb::io::quic::disconnect_reason::idle_timeout});

    server.poll();

    EXPECT_TRUE(server.is_open());
    EXPECT_EQ(server.current_state(), qb::io::async::quic::endpoint::state::listening);
    EXPECT_EQ(server.closed, 1);
    EXPECT_EQ(server.last_close_reason, qb::io::quic::disconnect_reason::idle_timeout);
    EXPECT_EQ(server.session_count(), 0u);
}

TEST(QuicEndpointTest, RemoteStreamDataFeedsSessionExtendsCreditAndFlushesResponse) {
    auto *raw_backend = new FakeQuicBackend;
    EchoQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    qb::io::quic::backend_event data;
    data.type = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 9;
    data.stream_id = 1;
    data.payload = {std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
    raw_backend->queued_events.push_back(std::move(data));

    server.poll();

    auto *session = server.stream_session(9, 1);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(server.sessions, 1);
    EXPECT_EQ(server.stream_data_events, 1);
    EXPECT_EQ(session->messages, 1);
    EXPECT_EQ(session->received, "ping");
    EXPECT_EQ(session->pendingRead(), 0u);
    EXPECT_EQ(session->pendingWrite(), 0u);

    EXPECT_EQ(raw_backend->extend_stream_credit_calls, 1);
    EXPECT_EQ(raw_backend->extended_stream_id, 1u);
    EXPECT_EQ(raw_backend->extended_bytes, 4u);
    EXPECT_EQ(raw_backend->send_stream_data_calls, 1);
    EXPECT_EQ(raw_backend->sent_connection_id, 9u);
    EXPECT_EQ(raw_backend->sent_stream_id, 1u);
    EXPECT_EQ(raw_backend->sent_stream_bytes, 4u);
}

TEST(QuicEndpointTest, RemoteStreamFinIsDeliveredAndThenSessionIsUnregistered) {
    auto *raw_backend = new FakeQuicBackend;
    EchoQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    qb::io::quic::backend_event data;
    data.type = qb::io::quic::backend_event::kind::stream_data;
    data.connection_id = 9;
    data.stream_id = 1;
    data.payload = {std::byte{'d'}, std::byte{'o'}, std::byte{'n'}, std::byte{'e'}};
    data.error_code = 1;
    raw_backend->queued_events.push_back(std::move(data));

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_closed, 9, 1, 0, {}, {},
        qb::io::quic::disconnect_reason::none,
        qb::io::quic::stream_close_reason::finished});

    server.poll();

    EXPECT_EQ(server.stream_data_events, 1);
    EXPECT_EQ(server.stream_session(9, 1), nullptr);
    EXPECT_EQ(server.session_count(), 0u);
    EXPECT_EQ(raw_backend->extend_stream_credit_calls, 1);
    EXPECT_EQ(raw_backend->send_stream_data_calls, 1);
}

TEST(QuicEndpointTest, CloseConnectionDelegatesWithoutClosingEndpoint) {
    auto *raw_backend = new FakeQuicBackend;
    qb::io::async::quic::endpoint endpoint{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    ASSERT_TRUE(endpoint.connect(qb::io::uri{"quic://127.0.0.1:4433"}));

    endpoint.close_connection(42, 99, "only this connection");

    EXPECT_EQ(raw_backend->close_connection_calls, 1);
    EXPECT_EQ(raw_backend->close_calls, 0);
    EXPECT_EQ(raw_backend->closed_connection_id, 42u);
    EXPECT_EQ(raw_backend->close_code, 99u);
    EXPECT_EQ(raw_backend->close_reason, "only this connection");
    EXPECT_TRUE(endpoint.is_open());
}

TEST(QuicEndpointTest, DispatchesStreamAckEvents) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    raw_backend->queued_events.push_back({
        qb::io::quic::backend_event::kind::stream_data_acked, 7, 11, 42, {}, {}});

    server.poll();

    EXPECT_EQ(server.acked_bytes, 42u);
}

TEST(QuicEndpointTest, StreamSessionCapRejectsExtraRemoteStreamsAndResetsStream) {
    auto *raw_backend = new FakeQuicBackend;
    CallbackQuicServer server{
        std::unique_ptr<qb::io::quic::backend>(raw_backend)};

    server.set_max_sessions(1);
    ASSERT_TRUE(server.listen(qb::io::uri{"quic://127.0.0.1:0"}, "", ""));

    qb::io::quic::backend_event first;
    first.type = qb::io::quic::backend_event::kind::stream_data;
    first.connection_id = 7;
    first.stream_id = 1;
    first.payload.push_back(std::byte{'a'});
    raw_backend->queued_events.push_back(std::move(first));

    qb::io::quic::backend_event second;
    second.type = qb::io::quic::backend_event::kind::stream_data;
    second.connection_id = 7;
    second.stream_id = 5;
    second.payload.push_back(std::byte{'b'});
    raw_backend->queued_events.push_back(std::move(second));

    server.poll();

    EXPECT_EQ(server.session_count(), 1u);
    EXPECT_NE(server.stream_session(7, 1), nullptr);
    EXPECT_EQ(server.stream_session(7, 5), nullptr);
    EXPECT_EQ(raw_backend->reset_stream_calls, 1);
    EXPECT_EQ(raw_backend->reset_stream_id, 5u);
    EXPECT_EQ(raw_backend->reset_stream_code, 1u);
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

TEST(QuicEndpointTest, NativeAlpnMismatchDoesNotEstablishConnection) {
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
                              {"custom-quic"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    qb::io::quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    CallbackQuicClient client;
    const auto uri = std::string{"quic://127.0.0.1:"} +
                     std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(client.connect(qb::io::uri{uri}, client_tls, {"h3"}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (client.current_state() != qb::io::async::quic::endpoint::state::closed &&
           std::chrono::steady_clock::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(client.current_state(), qb::io::async::quic::endpoint::state::closed);
    EXPECT_EQ(server.connected, 0);

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
