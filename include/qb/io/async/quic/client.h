/**
 * @file qb/io/async/quic/client.h
 * @brief Async QUIC client facade.
 */

#ifndef QB_IO_ASYNC_QUIC_CLIENT_H_
#define QB_IO_ASYNC_QUIC_CLIENT_H_

#include "endpoint.h"
#include "io_handler.h"

namespace qb::io::async::quic {

template <typename Derived, typename StreamSession = void>
class connector;

template <typename Derived>
class connector<Derived, void> : public endpoint {
public:
    using endpoint::endpoint;

    bool connect(qb::io::uri const& remote_uri,
                 std::vector<std::string> const& alpn_protocols = {std::string(qb::io::quic::alpn::h3)}) {
        return endpoint::connect(remote_uri, alpn_protocols);
    }

    bool connect(qb::io::uri const& remote_uri,
                 qb::io::quic::tls_config tls,
                 std::vector<std::string> const& alpn_protocols = {std::string(qb::io::quic::alpn::h3)}) {
        return endpoint::connect(remote_uri, std::move(tls), alpn_protocols);
    }

protected:
    void dispatch(event::connected const& ev) override {
        if constexpr (requires(Derived& derived, event::connected const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::connection_closed const& ev) override {
        if constexpr (requires(Derived& derived, event::connection_closed const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::stream_started const& ev) override {
        if constexpr (requires(Derived& derived, event::stream_started const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::stream_data const& ev) override {
        if constexpr (requires(Derived& derived, event::stream_data const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::stream_closed const& ev) override {
        if constexpr (requires(Derived& derived, event::stream_closed const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::datagram const& ev) override {
        if constexpr (requires(Derived& derived, event::datagram const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }
};

template <typename Derived, typename StreamSession>
class connector : public endpoint,
                  public io_handler<Derived, StreamSession> {
    using handler_type = io_handler<Derived, StreamSession>;

public:
    using stream_session_type = StreamSession;
    using endpoint::endpoint;

    bool connect(qb::io::uri const& remote_uri,
                 std::vector<std::string> const& alpn_protocols = {std::string(qb::io::quic::alpn::h3)}) {
        return endpoint::connect(remote_uri, alpn_protocols);
    }

    bool connect(qb::io::uri const& remote_uri,
                 qb::io::quic::tls_config tls,
                 std::vector<std::string> const& alpn_protocols = {std::string(qb::io::quic::alpn::h3)}) {
        return endpoint::connect(remote_uri, std::move(tls), alpn_protocols);
    }

    [[nodiscard]] StreamSession *stream_session(std::uint64_t stream_id) noexcept {
        return handler_type::session(stream_id);
    }

    [[nodiscard]] StreamSession *stream_session(std::uint64_t connection_id,
                                                std::uint64_t stream_id) noexcept {
        return handler_type::session(connection_id, stream_id);
    }

protected:
    void dispatch(event::connected const& ev) override {
        if constexpr (requires(Derived& derived, event::connected const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::connection_closed const& ev) override {
        handler_type::clearSessions(ev.connection_id);
        if constexpr (requires(Derived& derived, event::connection_closed const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::stream_started const& ev) override {
        if (ev.origin == qb::io::quic::stream_origin::remote)
            (void)handler_type::ensure_stream_session(ev.connection_id, ev.id);
        if constexpr (requires(Derived& derived, event::stream_started const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::stream_data const& ev) override {
        if (!handler_type::feed_stream_data(
                ev, [this](std::uint64_t connection_id, std::uint64_t stream_id,
                           std::uint64_t bytes) {
                    this->extend_stream_credit(connection_id, stream_id, bytes);
                })) {
            this->backend()->reset_stream(ev.connection_id, ev.id, 1);
            return;
        }
        if (auto session = handler_type::session_handle(ev.connection_id, ev.id)) {
            handler_type::drain_stream_output(
                *session,
                [this](std::uint64_t connection_id, std::uint64_t stream_id,
                       std::span<const std::byte> data, bool fin) {
                    this->send_stream_data(connection_id, stream_id, data, fin);
                });
        }
        if constexpr (requires(Derived& derived, event::stream_data const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::stream_closed const& ev) override {
        handler_type::unregisterSession(ev.connection_id, ev.id);
        if constexpr (requires(Derived& derived, event::stream_closed const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }

    void dispatch(event::datagram const& ev) override {
        if constexpr (requires(Derived& derived, event::datagram const& event) { derived.on(event); })
            static_cast<Derived&>(*this).on(ev);
    }
};

} // namespace qb::io::async::quic

#endif // QB_IO_ASYNC_QUIC_CLIENT_H_
