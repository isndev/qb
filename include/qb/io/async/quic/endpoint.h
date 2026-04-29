/**
 * @file qb/io/async/quic/endpoint.h
 * @brief QB-native QUIC endpoint contract.
 */

#ifndef QB_IO_ASYNC_QUIC_ENDPOINT_H_
#define QB_IO_ASYNC_QUIC_ENDPOINT_H_

#include <array>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../../uri.h"
#include "../../udp/socket.h"
#include "../../quic.h"
#include "../event/io.h"
#include "../event/timer.h"
#include "../listener.h"
#include "events.h"
#include "stream.h"

namespace qb::io::async::quic {

class endpoint {
public:
    enum class state {
        idle,
        listening,
        connecting,
        connected,
        closing,
        closed
    };

private:
    qb::io::quic::settings _settings;
    qb::io::quic::stats _stats;
    std::unique_ptr<qb::io::quic::backend> _backend;
    std::deque<qb::io::quic::packet> _pending_udp_packets;
    qb::io::udp::socket _socket;
    qb::io::endpoint _local_endpoint;
    qb::io::async::event::io *_io_event = nullptr;
    qb::io::async::event::timer *_timer_event = nullptr;
    state _state = state::idle;
    bool _server_role = false;
    bool _open = false;

protected:
    virtual void dispatch(event::connected const&) {}
    virtual void dispatch(event::connection_closed const&) {}
    virtual void dispatch(event::stream_started const&) {}
    virtual void dispatch(event::stream_data const&) {}
    virtual void dispatch(event::stream_data_acked const&) {}
    virtual void dispatch(event::stream_closed const&) {}
    virtual void dispatch(event::datagram const&) {}
    virtual void after_dispatch_events() {}

    [[nodiscard]] qb::io::quic::stream_direction stream_direction(std::uint64_t id) const noexcept {
        return (id & 0x2u) == 0
            ? qb::io::quic::stream_direction::bidirectional
            : qb::io::quic::stream_direction::unidirectional;
    }

    [[nodiscard]] qb::io::quic::stream_origin stream_origin(std::uint64_t id) const noexcept {
        const bool server_initiated = (id & 0x1u) != 0;
        return server_initiated == _server_role
            ? qb::io::quic::stream_origin::local
            : qb::io::quic::stream_origin::remote;
    }

    void ensure_backend() {
        if (_backend)
            return;
        if (!qb::io::quic::available())
            throw std::runtime_error(qb::io::quic::unavailable_reason());
        _backend = qb::io::quic::make_native_backend();
        if (!_backend)
            throw std::runtime_error("No QUIC backend is attached to this endpoint.");
    }

    void register_io_watcher() {
        if (_io_event)
            return;
        auto& ev = listener::current.registerEvent<qb::io::async::event::io>(
            *this, _socket.native_handle(), EV_READ);
        _io_event = &ev;
        _io_event->start();
    }

    void register_timer_watcher() {
        if (_timer_event)
            return;
        auto& ev = listener::current.registerEvent<qb::io::async::event::timer>(*this);
        _timer_event = &ev;
    }

    void unregister_watchers() noexcept {
        if (_io_event) {
            auto *iface = _io_event->_interface;
            _io_event = nullptr;
            listener::current.unregisterEvent(iface);
        }
        if (_timer_event) {
            auto *iface = _timer_event->_interface;
            _timer_event = nullptr;
            listener::current.unregisterEvent(iface);
        }
    }

    void arm_timer() {
        if (!_backend || !_timer_event)
            return;
        const auto deadline = _backend->next_timeout();
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            _timer_event->stop();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto delay = deadline <= now
            ? std::chrono::duration<double>{0.}
            : std::chrono::duration<double>(deadline - now);
        _timer_event->start(delay.count());
    }

    void fail_transport(std::uint64_t error_code, std::string_view reason) {
        _pending_udp_packets.clear();
        if (_io_event)
            _io_event->stop();
        if (_timer_event)
            _timer_event->stop();
        if (_socket.is_open())
            _socket.close();
        _state = state::closed;
        _open = false;
        dispatch(qb::io::async::quic::event::connection_closed{
            0,
            qb::io::quic::disconnect_reason::transport_error,
            error_code,
            std::string(reason)
        });
    }

    void flush_udp_packets() {
        std::uint64_t budget = _settings.udp_tx_batch_size ? _settings.udp_tx_batch_size
                                                           : std::numeric_limits<std::uint64_t>::max();
        while (!_pending_udp_packets.empty() && budget-- > 0) {
            auto const& pkt = _pending_udp_packets.front();
            if (!pkt.remote) {
                _pending_udp_packets.pop_front();
                continue;
            }
            const auto ret = _socket.write(pkt.payload.data(), pkt.payload.size(), pkt.remote);
            if (ret < 0) {
                if (qb::io::socket::not_send_error(qb::io::socket::get_last_errno()))
                    break;
                fail_transport(static_cast<std::uint64_t>(qb::io::socket::get_last_errno()),
                               "QUIC UDP write failed");
                return;
            }
            _pending_udp_packets.pop_front();
        }
    }

    void drain_backend_packets() {
        if (!_backend)
            return;
        auto packets = _backend->drain_packets();
        for (auto& pkt : packets)
            _pending_udp_packets.push_back(std::move(pkt));
        flush_udp_packets();
        if (!_backend)
            return;
        _stats = _backend->current_stats();
        if ((!_pending_udp_packets.empty() || _backend->wants_write()) && _io_event)
            _io_event->set(_io_event->events | EV_WRITE);
        else if (_io_event)
            _io_event->set(EV_READ);
        arm_timer();
    }

    void drain_backend_events() {
        if (!_backend)
            return;
        for (auto const& event : _backend->drain_events()) {
            if (event.type == qb::io::quic::backend_event::kind::connected) {
                _state = state::connected;
                dispatch(qb::io::async::quic::event::connected{
                    event.connection_id,
                    event.text
                });
            } else if (event.type == qb::io::quic::backend_event::kind::connection_closed) {
                if (!_server_role || event.connection_id == 0) {
                    _state = state::closed;
                    _open = false;
                } else {
                    _state = _backend->current_stats().active_connections > 0
                        ? state::connected
                        : state::listening;
                    _open = true;
                }
                dispatch(qb::io::async::quic::event::connection_closed{
                    event.connection_id,
                    event.connection_reason == qb::io::quic::disconnect_reason::none
                        ? qb::io::quic::disconnect_reason::transport_error
                        : event.connection_reason,
                    event.error_code,
                    event.text
                });
            } else if (event.type == qb::io::quic::backend_event::kind::stream_started) {
                dispatch(qb::io::async::quic::event::stream_started{
                    event.connection_id,
                    event.stream_id,
                    stream_direction(event.stream_id),
                    stream_origin(event.stream_id)
                });
            } else if (event.type == qb::io::quic::backend_event::kind::stream_data) {
                dispatch(qb::io::async::quic::event::stream_data{
                    event.connection_id,
                    event.stream_id,
                    std::string_view{
                        reinterpret_cast<const char *>(event.payload.data()),
                        event.payload.size()
                    },
                    event.error_code != 0
                });
            } else if (event.type == qb::io::quic::backend_event::kind::stream_data_acked) {
                dispatch(qb::io::async::quic::event::stream_data_acked{
                    event.connection_id,
                    event.stream_id,
                    event.error_code
                });
            } else if (event.type == qb::io::quic::backend_event::kind::stream_closed) {
                dispatch(qb::io::async::quic::event::stream_closed{
                    event.connection_id,
                    event.stream_id,
                    event.stream_reason == qb::io::quic::stream_close_reason::none
                        ? qb::io::quic::stream_close_reason::reset
                        : event.stream_reason,
                    event.error_code
                });
            } else if (event.type == qb::io::quic::backend_event::kind::datagram) {
                dispatch(qb::io::async::quic::event::datagram{
                    event.connection_id,
                    std::string_view{
                        reinterpret_cast<const char *>(event.payload.data()),
                        event.payload.size()
                    }
                });
            }
        }
        _stats = _backend->current_stats();
        after_dispatch_events();
    }

public:
    endpoint() = default;
    explicit endpoint(qb::io::quic::settings settings)
        : _settings(settings) {}
    explicit endpoint(std::unique_ptr<qb::io::quic::backend> backend,
                      qb::io::quic::settings settings = {})
        : _settings(settings)
        , _backend(std::move(backend)) {}

    endpoint(endpoint const&) = delete;
    endpoint& operator=(endpoint const&) = delete;
    endpoint(endpoint&&) = delete;
    endpoint& operator=(endpoint&&) = delete;

    ~endpoint() {
        close();
        unregister_watchers();
    }

    [[nodiscard]] bool is_open() const noexcept { return _open; }
    [[nodiscard]] state current_state() const noexcept { return _state; }
    [[nodiscard]] qb::io::quic::settings const& settings() const noexcept { return _settings; }
    [[nodiscard]] qb::io::quic::stats const& stats() const noexcept { return _stats; }
    [[nodiscard]] qb::io::endpoint const& local_endpoint() const noexcept { return _local_endpoint; }
    [[nodiscard]] qb::io::quic::backend *backend() noexcept { return _backend.get(); }
    [[nodiscard]] qb::io::quic::backend const *backend() const noexcept { return _backend.get(); }

    void set_backend(std::unique_ptr<qb::io::quic::backend> backend) noexcept {
        _backend = std::move(backend);
    }

    void set_settings(qb::io::quic::settings settings) noexcept {
        _settings = settings;
    }

    void poll() {
        drain_backend_packets();
        drain_backend_events();
    }

    bool listen(qb::io::uri const& bind_uri,
                std::filesystem::path const& cert_file,
                std::filesystem::path const& key_file,
                std::vector<std::string> const& alpn_protocols = {std::string(qb::io::quic::alpn::h3)}) {
        ensure_backend();
        if (_socket.bind(bind_uri) != 0)
            return false;
        _socket.set_nonblocking(true);
        _local_endpoint = _socket.local_endpoint();
        qb::io::quic::tls_config tls;
        tls.certificate_file = cert_file;
        tls.private_key_file = key_file;
        _backend->configure(_settings);
        _backend->start_server(_local_endpoint, alpn_protocols, tls);
        _state = state::listening;
        _server_role = true;
        _open = true;
        register_io_watcher();
        register_timer_watcher();
        _stats = _backend->current_stats();
        drain_backend_packets();
        return true;
    }

    bool connect(qb::io::uri const& remote_uri,
                 std::vector<std::string> const& alpn_protocols = {std::string(qb::io::quic::alpn::h3)}) {
        qb::io::quic::tls_config tls;
        tls.server_name.assign(remote_uri.host());
        return connect(remote_uri, tls, alpn_protocols);
    }

    bool connect(qb::io::uri const& remote_uri,
                 qb::io::quic::tls_config tls,
                 std::vector<std::string> const& alpn_protocols = {std::string(qb::io::quic::alpn::h3)}) {
        ensure_backend();
        qb::io::endpoint remote;
        if (!remote_uri.host().empty())
            remote = qb::io::endpoint(std::string(remote_uri.host()).c_str(), remote_uri.u_port());
        if (!remote)
            return false;
        if (!_socket.is_open() && !_socket.init(remote.af()))
            return false;
        qb::io::endpoint bind_any;
        bind_any.as_in(remote.af() == AF_INET ? "0.0.0.0" : "::", 0);
        if (_socket.bind(bind_any) != 0)
            return false;
        _socket.set_nonblocking(true);
        _local_endpoint = _socket.local_endpoint();
        if (tls.server_name.empty())
            tls.server_name.assign(remote_uri.host());
        _backend->configure(_settings);
        _backend->start_client(_local_endpoint, remote, alpn_protocols, tls);
        _state = state::connecting;
        _server_role = false;
        _open = true;
        register_io_watcher();
        register_timer_watcher();
        _stats = _backend->current_stats();
        drain_backend_packets();
        return true;
    }

    stream open_bidirectional_stream() {
        return open_bidirectional_stream(0);
    }

    stream open_bidirectional_stream(std::uint64_t connection_id) {
        ensure_backend();
        const auto id = _backend->open_stream(connection_id,
                                              qb::io::quic::stream_direction::bidirectional);
        _stats = _backend->current_stats();
        return {connection_id, id, qb::io::quic::stream_direction::bidirectional,
                qb::io::quic::stream_origin::local};
    }

    stream open_unidirectional_stream() {
        return open_unidirectional_stream(0);
    }

    stream open_unidirectional_stream(std::uint64_t connection_id) {
        ensure_backend();
        const auto id = _backend->open_stream(connection_id,
                                              qb::io::quic::stream_direction::unidirectional);
        _stats = _backend->current_stats();
        return {connection_id, id, qb::io::quic::stream_direction::unidirectional,
                qb::io::quic::stream_origin::local};
    }

    void send_stream_data(std::uint64_t stream_id, std::span<const std::byte> data,
                          bool fin = false) {
        send_stream_data(0, stream_id, data, fin);
    }

    void send_stream_data(std::uint64_t connection_id, std::uint64_t stream_id,
                          std::span<const std::byte> data, bool fin = false) {
        ensure_backend();
        _backend->send_stream_data(connection_id, stream_id, data, fin);
        drain_backend_packets();
        drain_backend_events();
    }

    void send_stream_data(std::uint64_t stream_id, std::string_view data,
                          bool fin = false) {
        send_stream_data(0, stream_id, data, fin);
    }

    void send_stream_data(std::uint64_t connection_id, std::uint64_t stream_id,
                          std::string_view data, bool fin = false) {
        send_stream_data(connection_id, stream_id,
                         std::span<const std::byte>(
                             reinterpret_cast<const std::byte *>(data.data()),
                             data.size()),
                         fin);
    }

    void extend_stream_credit(std::uint64_t connection_id, std::uint64_t stream_id,
                              std::uint64_t bytes) {
        if (!_backend || bytes == 0)
            return;
        _backend->extend_stream_credit(connection_id, stream_id, bytes);
    }

    void reset_stream(std::uint64_t stream_id,
                      std::uint64_t application_error_code = 0) {
        reset_stream(0, stream_id, application_error_code);
    }

    void reset_stream(std::uint64_t connection_id, std::uint64_t stream_id,
                      std::uint64_t application_error_code) {
        ensure_backend();
        _backend->reset_stream(connection_id, stream_id, application_error_code);
        drain_backend_packets();
        drain_backend_events();
    }

    void stop_stream(std::uint64_t stream_id,
                     std::uint64_t application_error_code = 0) {
        stop_stream(0, stream_id, application_error_code);
    }

    void stop_stream(std::uint64_t connection_id, std::uint64_t stream_id,
                     std::uint64_t application_error_code) {
        ensure_backend();
        _backend->stop_stream(connection_id, stream_id, application_error_code);
        drain_backend_packets();
        drain_backend_events();
    }

    void send_datagram(std::span<const std::byte> data) {
        send_datagram(0, data);
    }

    void send_datagram(std::uint64_t connection_id, std::span<const std::byte> data) {
        ensure_backend();
        _backend->send_datagram(connection_id, data);
        drain_backend_packets();
        drain_backend_events();
    }

    void send_datagram(std::string_view data) {
        send_datagram(0, data);
    }

    void send_datagram(std::uint64_t connection_id, std::string_view data) {
        send_datagram(connection_id,
                      std::span<const std::byte>(
                          reinterpret_cast<const std::byte *>(data.data()),
                          data.size()));
    }

    void close_connection(std::uint64_t connection_id,
                          std::uint64_t application_error_code = 0,
                          std::string_view reason = {}) {
        if (!_backend)
            return;
        _backend->close_connection(connection_id, application_error_code, reason);
        drain_backend_packets();
        drain_backend_events();
    }

    void close(std::uint64_t application_error_code = 0,
               std::string_view reason = {}) {
        if (_backend) {
            _backend->close(application_error_code, reason);
            drain_backend_packets();
            drain_backend_events();
        }
        if (_io_event)
            _io_event->stop();
        if (_timer_event)
            _timer_event->stop();
        if (_socket.is_open())
            _socket.close();
        _state = state::closed;
        _open = false;
    }

    void on(qb::io::async::event::io const& event) {
        if (!_backend || !_socket.is_open())
            return;
        if (event._revents & EV_READ) {
            std::uint64_t budget = _settings.udp_rx_batch_size ? _settings.udp_rx_batch_size
                                                               : std::numeric_limits<std::uint64_t>::max();
            while (budget-- > 0) {
                std::array<std::byte, qb::io::udp::socket::MaxDatagramSize> buffer{};
                qb::io::endpoint remote;
                const auto ret = _socket.read(buffer.data(), buffer.size(), remote);
                if (ret < 0) {
                    if (qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
                        break;
                    close(static_cast<std::uint64_t>(qb::io::quic::disconnect_reason::transport_error),
                          "QUIC UDP read failed");
                    return;
                }
                if (ret == 0)
                    break;
                _backend->on_udp_datagram({
                    remote,
                    _local_endpoint,
                    std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(ret))
                });
            }
        }
        if (event._revents & EV_WRITE)
            flush_udp_packets();
        drain_backend_packets();
        drain_backend_events();
    }

    void on(qb::io::async::event::timer&) {
        if (!_backend)
            return;
        _backend->on_timeout(std::chrono::steady_clock::now());
        drain_backend_packets();
        drain_backend_events();
    }
};

} // namespace qb::io::async::quic

#endif // QB_IO_ASYNC_QUIC_ENDPOINT_H_
