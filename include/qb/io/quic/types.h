/**
 * @file qb/io/quic/types.h
 * @brief Public QUIC type definitions for qb-io.
 */

#ifndef QB_IO_QUIC_TYPES_H_
#define QB_IO_QUIC_TYPES_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <qb/system/timestamp.h>
#include <string>
#include <string_view>

namespace qb::io::quic {

enum class disconnect_reason : std::int32_t {
    none = 0,
    application_close = 1,
    transport_error = -1,
    handshake_failed = -2,
    idle_timeout = -3,
    stateless_retry_failed = -4,
    path_validation_failed = -5,
    backend_unavailable = -6,
    buffer_overflow = -7,
    protocol_error = -8
};

enum class stream_close_reason : std::int32_t {
    none = 0,
    finished = 1,
    reset = -1,
    stop_sending = -2,
    flow_control_error = -3,
    connection_closed = -4,
    backend_unavailable = -5
};

enum class stream_direction : std::uint8_t {
    bidirectional,
    unidirectional
};

enum class stream_origin : std::uint8_t {
    local,
    remote
};

struct settings {
    qb::duration  handshake_timeout = std::chrono::seconds(10);
    qb::duration  idle_timeout = std::chrono::seconds(30);
    std::uint64_t stream_recv_window = 1024 * 1024;
    std::uint64_t connection_recv_window = 16 * 1024 * 1024;
    std::uint64_t max_stream_data_bidi_local = 1024 * 1024;
    std::uint64_t max_stream_data_bidi_remote = 1024 * 1024;
    std::uint64_t max_stream_data_uni = 1024 * 1024;
    std::uint64_t max_streams_bidi = 100;
    std::uint64_t max_streams_uni = 100;
    std::uint64_t max_datagram_frame_size = 0;
    std::uint64_t max_connections = 4096;
    std::uint64_t max_pending_stream_bytes = 16 * 1024 * 1024;
    std::uint64_t max_pending_stream_frames = 4096;
    std::uint64_t max_pending_datagram_bytes = 4 * 1024 * 1024;
    std::uint64_t max_pending_datagram_frames = 1024;
    std::uint64_t udp_rx_batch_size = 256;
    std::uint64_t udp_tx_batch_size = 256;
    bool enable_stateless_retry = true;
    bool enable_datagrams = false;
    bool enable_keylog = false;
};

struct tls_config {
    std::filesystem::path certificate_file;
    std::filesystem::path private_key_file;
    std::string server_name;
    bool verify_peer = true;
};

struct stats {
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t packets_lost = 0;
    std::uint64_t retransmits = 0;
    std::uint64_t datagrams_sent = 0;
    std::uint64_t datagrams_received = 0;
    std::uint64_t datagrams_lost = 0;
    std::uint64_t datagrams_acked = 0;
    std::uint64_t active_connections = 0;
    std::uint64_t active_streams = 0;
    std::uint64_t smoothed_rtt_us = 0;
    std::uint64_t congestion_window = 0;
    std::uint64_t bytes_in_flight = 0;
};

struct alpn {
    static constexpr std::string_view h3 = "h3";
};

struct backend_info {
    std::string_view name;
    std::string_view version;
    bool crypto_initialized = false;
};

[[nodiscard]] inline constexpr bool
available() noexcept {
#ifdef QB_HAS_QUIC
    return true;
#else
    return false;
#endif
}

[[nodiscard]] inline std::string
unavailable_reason() {
#ifdef QB_HAS_QUIC
    return {};
#else
    return "QUIC support is not enabled. Configure with QB_WITH_QUIC=ON and install libngtcp2.";
#endif
}

[[nodiscard]] backend_info native_backend_info() noexcept;
[[nodiscard]] bool native_backend_ready() noexcept;

} // namespace qb::io::quic

#endif // QB_IO_QUIC_TYPES_H_
