/**
 * @file qb/io/async/quic/events.h
 * @brief Event payloads delivered by the qb QUIC async layer.
 */

#ifndef QB_IO_ASYNC_QUIC_EVENTS_H_
#define QB_IO_ASYNC_QUIC_EVENTS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include "../../quic/types.h"

namespace qb::io::async::quic::event {

struct connected {
    std::uint64_t connection_id = 0;
    std::string   negotiated_alpn;
};

struct connection_closed {
    std::uint64_t                   connection_id = 0;
    qb::io::quic::disconnect_reason reason        = qb::io::quic::disconnect_reason::none;
    std::uint64_t                   error_code    = 0;
    std::string                     reason_phrase;
};

struct stream_started {
    std::uint64_t                  connection_id = 0;
    std::uint64_t                  id            = 0;
    qb::io::quic::stream_direction direction     = qb::io::quic::stream_direction::bidirectional;
    qb::io::quic::stream_origin    origin        = qb::io::quic::stream_origin::remote;
};

struct stream_data {
    std::uint64_t    connection_id = 0;
    std::uint64_t    id            = 0;
    std::string_view payload;
    bool             fin = false;
};

struct stream_data_acked {
    std::uint64_t connection_id = 0;
    std::uint64_t id            = 0;
    std::uint64_t bytes         = 0;
};

struct stream_closed {
    std::uint64_t                     connection_id = 0;
    std::uint64_t                     id            = 0;
    qb::io::quic::stream_close_reason reason        = qb::io::quic::stream_close_reason::none;
    std::uint64_t                     error_code    = 0;
};

struct datagram {
    std::uint64_t    connection_id = 0;
    std::string_view payload;
};

} // namespace qb::io::async::quic::event

#endif // QB_IO_ASYNC_QUIC_EVENTS_H_
