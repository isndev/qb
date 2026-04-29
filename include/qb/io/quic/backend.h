/**
 * @file qb/io/quic/backend.h
 * @brief Backend contract for QB-native QUIC engines.
 */

#ifndef QB_IO_QUIC_BACKEND_H_
#define QB_IO_QUIC_BACKEND_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "../system/sys__socket.h"
#include "types.h"

namespace qb::io::quic {

struct packet_view {
    qb::io::endpoint remote;
    qb::io::endpoint local;
    std::span<const std::byte> payload;
};

struct packet {
    qb::io::endpoint remote;
    qb::io::endpoint local;
    std::vector<std::byte> payload;
};

struct stream_data {
    std::uint64_t stream_id = 0;
    std::span<const std::byte> payload;
    bool fin = false;
};

struct backend_event {
    enum class kind {
        connected,
        connection_closed,
        stream_started,
        stream_data,
        stream_data_acked,
        stream_closed,
        datagram
    };

    kind type = kind::connected;
    std::uint64_t connection_id = 0;
    std::uint64_t stream_id = 0;
    std::uint64_t error_code = 0;
    std::string text;
    std::vector<std::byte> payload;
    disconnect_reason connection_reason = disconnect_reason::none;
    stream_close_reason stream_reason = stream_close_reason::none;
};

class backend {
public:
    virtual ~backend() = default;

    virtual void configure(settings const& config) = 0;
    virtual void start_server(qb::io::endpoint const& local,
                              std::vector<std::string> const& alpn_protocols,
                              tls_config const& tls) = 0;
    virtual void start_client(qb::io::endpoint const& local,
                              qb::io::endpoint const& remote,
                              std::vector<std::string> const& alpn_protocols,
                              tls_config const& tls) = 0;

    virtual void on_udp_datagram(packet_view datagram) = 0;
    virtual void on_timeout(std::chrono::steady_clock::time_point now) = 0;

    [[nodiscard]] virtual std::chrono::steady_clock::time_point next_timeout() const = 0;
    [[nodiscard]] virtual bool wants_write() const noexcept = 0;

    virtual std::vector<packet> drain_packets() = 0;
    virtual std::vector<backend_event> drain_events() = 0;

    [[nodiscard]] virtual std::uint64_t open_stream(stream_direction direction) = 0;
    [[nodiscard]] virtual std::uint64_t open_stream(std::uint64_t connection_id,
                                                    stream_direction direction) = 0;
    virtual void send_stream_data(std::uint64_t connection_id, std::uint64_t stream_id,
                                  std::span<const std::byte> data, bool fin) = 0;
    virtual void extend_stream_credit(std::uint64_t connection_id,
                                      std::uint64_t stream_id,
                                      std::uint64_t bytes) = 0;
    virtual void reset_stream(std::uint64_t connection_id, std::uint64_t stream_id,
                              std::uint64_t application_error_code) = 0;
    virtual void stop_stream(std::uint64_t connection_id, std::uint64_t stream_id,
                             std::uint64_t application_error_code) = 0;
    virtual void send_datagram(std::uint64_t connection_id,
                               std::span<const std::byte> data) = 0;
    virtual void close_connection(std::uint64_t connection_id,
                                  std::uint64_t application_error_code,
                                  std::string_view reason) = 0;
    virtual void close(std::uint64_t application_error_code, std::string_view reason) = 0;

    [[nodiscard]] virtual stats current_stats() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<backend> make_native_backend();

} // namespace qb::io::quic

#endif // QB_IO_QUIC_BACKEND_H_
