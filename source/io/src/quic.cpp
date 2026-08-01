/**
 * @file qb/io/quic.cpp
 * @brief Native QUIC backend runtime hooks.
 */

#include <qb/io/quic/backend.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <qb/system/container/unordered_map.h>
#include <stdexcept>

#ifdef QB_HAS_QUIC
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h> // X509_CHECK_FLAG_* for client hostname verification
#endif

namespace qb::io::quic {

#ifdef QB_HAS_QUIC
namespace {

#if defined(NGTCP2_CALLBACKS_V3) && NGTCP2_CALLBACKS_VERSION >= NGTCP2_CALLBACKS_V3
#define QB_IO_NGTCP2_HAS_CALLBACKS_V3 1
#else
#define QB_IO_NGTCP2_HAS_CALLBACKS_V3 0
#endif

std::vector<unsigned char>
make_wire_alpn(std::vector<std::string> const &protocols) {
    std::vector<unsigned char> out;
    for (auto const &protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255)
            throw std::invalid_argument("QUIC ALPN entries must be 1..255 bytes long");
        out.push_back(static_cast<unsigned char>(protocol.size()));
        out.insert(out.end(), protocol.begin(), protocol.end());
    }
    if (out.empty())
        throw std::invalid_argument("QUIC requires at least one ALPN protocol");
    return out;
}

// Returns true when `dest` was filled with CSPRNG bytes. On RAND_bytes failure
// the buffer is zeroed (defensive) and false is returned so security-critical
// callers can fail closed instead of proceeding with predictable bytes.
bool
fill_random(uint8_t *dest, size_t len) {
    if (len == 0)
        return true;
    if (RAND_bytes(dest, static_cast<int>(len)) != 1) {
        std::memset(dest, 0, len);
        return false;
    }
    return true;
}

void
rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *) {
    // ngtcp2's rand callback is void-returning (best-effort, non-security-critical
    // randomness such as padding); zeroing on RNG failure is the only option here.
    (void) fill_random(dest, destlen);
}

std::string
cid_key(const uint8_t *data, size_t size) {
    return {reinterpret_cast<const char *>(data), size};
}

std::string
cid_key(ngtcp2_cid const &cid) {
    return cid_key(cid.data, cid.datalen);
}

// Wrap an ngtcp2 C callback so a C++ exception can never unwind through ngtcp2's C frames (UB /
// std::terminate). Our callbacks allocate (`_events.push_back`, `payload.assign`, map inserts) on
// peer-sized data, so bad_alloc under memory pressure is reachable. `guarded<Cb>::call` has the
// SAME signature as Cb (R + A... deduced from the function-pointer non-type parameter), so it drops
// straight into the ngtcp2 callback field; on any throw it returns NGTCP2_ERR_CALLBACK_FAILURE,
// which ngtcp2 turns into a clean connection failure. Mirrors the HTTP/3 guard_callback.
template <auto Cb>
struct guarded;
template <typename R, typename... A, R (*Fn)(A...)>
struct guarded<Fn> {
    static R
    call(A... args) noexcept {
        try {
            return Fn(args...);
        } catch (...) {
            return static_cast<R>(NGTCP2_ERR_CALLBACK_FAILURE);
        }
    }
};

int
fill_new_connection_id(ngtcp2_cid *cid, uint8_t *token, size_t tokenlen, size_t cidlen) {
    uint8_t cidbuf[NGTCP2_MAX_CIDLEN];
    // Fail closed on CSPRNG failure: a predictable connection ID is a
    // linkability problem, and — critically — a predictable stateless reset
    // token lets an off-path attacker forge stateless resets and tear down the
    // connection. Returning NGTCP2_ERR_CALLBACK_FAILURE makes ngtcp2 abort
    // rather than install zeroed security material.
    if (!fill_random(cidbuf, cidlen))
        return NGTCP2_ERR_CALLBACK_FAILURE;
    ngtcp2_cid_init(cid, cidbuf, cidlen);
    if (!fill_random(token, tokenlen))
        return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

#if QB_IO_NGTCP2_HAS_CALLBACKS_V3
int
new_connection_id_cb(ngtcp2_conn *, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token, size_t cidlen, void *) {
    return fill_new_connection_id(cid, token->data, sizeof(token->data), cidlen);
}
#else
int
new_connection_id_cb(ngtcp2_conn *, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *) {
    return fill_new_connection_id(cid, token, NGTCP2_STATELESS_RESET_TOKENLEN, cidlen);
}
#endif

int
alpn_select_cb(SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *arg) {
    auto const          &wire_alpn    = *static_cast<std::vector<unsigned char> const *>(arg);
    const unsigned char *selected     = nullptr;
    unsigned char        selected_len = 0;
    if (SSL_select_next_proto(const_cast<unsigned char **>(&selected), &selected_len, wire_alpn.data(),
                              static_cast<unsigned int>(wire_alpn.size()), in, inlen)
        != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out    = selected;
    *outlen = selected_len;
    return SSL_TLSEXT_ERR_OK;
}

ngtcp2_conn *
get_conn_cb(ngtcp2_crypto_conn_ref *ref) {
    return *static_cast<ngtcp2_conn **>(ref->user_data);
}

// ngtcp2 1.23 deprecated `ngtcp2_conn_get_scid` and `ngtcp2_conn_get_expiry` in favour of the
// const-correct `*2` spellings (identical semantics; only the `conn` parameter gained `const`).
// qb's FindNgtcp2 does not pin a minimum version, so select at COMPILE TIME rather than dropping
// support for pre-1.23 releases — an unconditional switch would simply fail to build there.
// NGTCP2_VERSION_NUM is 0xMMmmpp; 1.23.0 == 0x011700.
#if defined(NGTCP2_VERSION_NUM) && NGTCP2_VERSION_NUM >= 0x011700
inline std::size_t
conn_get_scid(const ngtcp2_conn *conn, ngtcp2_cid *dest) noexcept {
    return ngtcp2_conn_get_scid2(conn, dest);
}
inline ngtcp2_tstamp
conn_get_expiry(const ngtcp2_conn *conn) noexcept {
    return ngtcp2_conn_get_expiry2(conn);
}
#else
inline std::size_t
conn_get_scid(ngtcp2_conn *conn, ngtcp2_cid *dest) noexcept {
    return ngtcp2_conn_get_scid(conn, dest);
}
inline ngtcp2_tstamp
conn_get_expiry(ngtcp2_conn *conn) noexcept {
    return ngtcp2_conn_get_expiry(conn);
}
#endif

std::chrono::steady_clock::time_point
to_time_point(ngtcp2_tstamp ts, std::chrono::steady_clock::time_point base) {
    if (ts == UINT64_MAX)
        return std::chrono::steady_clock::time_point::max();
    return base + std::chrono::nanoseconds(ts);
}

class native_backend final : public backend {
    struct queued_stream {
        std::uint64_t          stream_id = 0;
        std::vector<std::byte> data;
        std::uint64_t          offset      = 0;
        std::size_t            sent_offset = 0;
        bool                   fin         = false;
    };

    struct queued_datagram {
        std::uint64_t          id = 0;
        std::vector<std::byte> data;
    };

    settings                                                          _config;
    stats                                                             _stats;
    std::vector<std::string>                                          _alpn;
    std::vector<unsigned char>                                        _wire_alpn;
    std::deque<queued_stream>                                         _pending_streams;
    std::deque<queued_stream>                                         _inflight_streams;
    std::deque<queued_datagram>                                       _pending_datagrams;
    qb::unordered_map<std::uint64_t, std::uint64_t>                   _inflight_datagrams;
    qb::unordered_map<std::uint64_t, std::uint64_t>                   _next_stream_offsets;
    qb::unordered_map<std::uint64_t, bool>                            _stream_fin_seen;
    qb::unordered_map<std::uint64_t, std::unique_ptr<native_backend>> _server_connections;
    qb::unordered_map<std::string, std::uint64_t>                     _server_cid_index;
    std::vector<ngtcp2_cid>                                           _scid_scratch;    // reused by reconcile_server_cids (no per-packet alloc)
    std::vector<std::string>                                          _cid_key_scratch; // reused by reconcile_server_cids (no per-packet alloc)
    std::vector<std::uint64_t>                                        _server_closed_connections;
    std::uint64_t                                                     _queued_stream_bytes    = 0;
    std::uint64_t                                                     _queued_stream_frames   = 0;
    std::uint64_t                                                     _queued_datagram_bytes  = 0;
    std::uint64_t                                                     _queued_datagram_frames = 0;
    std::uint64_t                                                     _next_datagram_id       = 1;
    std::uint64_t                                                     _connection_id          = 0;
    std::uint64_t                                                     _next_connection_id     = 1;
    std::vector<packet>                                               _packets;
    std::vector<backend_event>                                        _events;
    qb::io::endpoint                                                  _local{"0.0.0.0", 0};
    qb::io::endpoint                                                  _remote;
    tls_config                                                        _server_tls;
    std::string                                                       _local_cid_key;
    std::string                                                       _original_dcid_key;
    /// CID keys this child currently owns in the PARENT's `_server_cid_index`, as installed by
    /// `reconcile_server_cids` from ngtcp2's ACTIVE (non-retired) SCID set. Kept so reconcile can
    /// remove the ones ngtcp2 has since retired — see the note there. Excludes `_local_cid_key` /
    /// `_original_dcid_key`, which the accept path owns and which must outlive CID rotation.
    std::vector<std::string> _indexed_scid_keys;
    // Address-validation Retry (RFC 9000 §8.1). PARENT: a stable secret keys the retry tokens it
    // generates and later verifies. CHILD: when the connection was accepted only after a validated
    // Retry token, carry the recovered original DCID (from the token) + the retry SCID we issued, so
    // accept_server_connection sets original_dcid/retry_scid in the transport params correctly.
    std::array<std::uint8_t, 32>          _retry_secret{};
    bool                                  _retry_secret_ready = false;
    bool                                  _has_retry_context  = false;
    ngtcp2_cid                            _retry_odcid{};
    ngtcp2_cid                            _retry_scid{};
    std::chrono::steady_clock::time_point _base       = std::chrono::steady_clock::now();
    ngtcp2_conn                          *_conn       = nullptr;
    ngtcp2_crypto_ossl_ctx               *_crypto_ctx = nullptr;
    SSL_CTX                              *_ssl_ctx    = nullptr;
    SSL                                  *_ssl        = nullptr;
    ngtcp2_crypto_conn_ref                _conn_ref{};
    bool                                  _server             = false;
    bool                                  _server_parent      = false;
    bool                                  _started            = false;
    bool                                  _closing            = false;
    bool                                  _close_event_queued = false;

    [[nodiscard]] std::string
    negotiated_alpn() const {
        if (!_ssl)
            return {};
        const unsigned char *data = nullptr;
        unsigned int         len  = 0;
        SSL_get0_alpn_selected(_ssl, &data, &len);
        if (!data || len == 0)
            return {};
        return {reinterpret_cast<const char *>(data), len};
    }

public:
    native_backend() {
        if (ngtcp2_crypto_ossl_init() != 0)
            throw std::runtime_error("ngtcp2 OpenSSL crypto initialization failed");
    }

    ~native_backend() override {
        if (_ssl) {
            SSL_set_app_data(_ssl, nullptr);
            SSL_free(_ssl);
        }
        if (_ssl_ctx)
            SSL_CTX_free(_ssl_ctx);
        if (_conn)
            ngtcp2_conn_del(_conn);
        if (_crypto_ctx)
            ngtcp2_crypto_ossl_ctx_del(_crypto_ctx);
    }

    void
    configure(settings const &config) override {
        _config = config;
    }

    void
    start_server(qb::io::endpoint const &local, std::vector<std::string> const &alpn_protocols, tls_config const &tls) override {
        _server        = true;
        _server_parent = true;
        _local         = local;
        _alpn          = alpn_protocols;
        _wire_alpn     = make_wire_alpn(_alpn);
        _server_tls    = tls;
        validate_server_tls_config(tls);
        _closing            = false;
        _close_event_queued = false;
        _started            = true;
    }

    void
    start_client(qb::io::endpoint const &local, qb::io::endpoint const &remote, std::vector<std::string> const &alpn_protocols,
                 tls_config const &tls) override {
        _server    = false;
        _local     = local;
        _remote    = remote;
        _alpn      = alpn_protocols;
        _wire_alpn = make_wire_alpn(_alpn);
        make_ssl_context(tls, false);
        make_client_connection();
        ngtcp2_conn_set_tls_native_handle(_conn, _crypto_ctx);
        _closing                  = false;
        _close_event_queued       = false;
        _started                  = true;
        _stats.active_connections = 1;
        drain_transport();
    }

    void
    on_udp_datagram(packet_view datagram) override {
        if (_server_parent) {
            if (auto *connection = find_or_accept_server_connection(datagram)) {
                connection->on_udp_datagram(datagram);
                // Reading the packet may have made ngtcp2 issue fresh Source Connection IDs (via
                // new_connection_id_cb) that the peer can migrate to. Index the child's CURRENT SCID
                // set so a later packet carrying a rotated DCID still routes to this connection
                // instead of missing the index and being mis-accepted as a brand-new connection.
                reconcile_server_cids(connection);
            }
            return;
        }
        if (!_conn && _server && !accept_server_connection(datagram))
            return;
        if (!_conn)
            return;
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_init(&ps, reinterpret_cast<const ngtcp2_sockaddr *>(&datagram.local.sa_), datagram.local.len(),
                                 reinterpret_cast<const ngtcp2_sockaddr *>(&datagram.remote.sa_), datagram.remote.len(), nullptr);
        ngtcp2_pkt_info pi{};
        const auto      now = timestamp(std::chrono::steady_clock::now());
        auto            rv  = ngtcp2_conn_read_pkt(_conn, &ps.path, &pi, reinterpret_cast<const uint8_t *>(datagram.payload.data()),
                                                   datagram.payload.size(), now);
        if (rv == 0) {
            ++_stats.packets_received;
            _stats.bytes_received += datagram.payload.size();
        } else if (rv == NGTCP2_ERR_CLOSING || rv == NGTCP2_ERR_DRAINING) {
            queue_close_event(disconnect_reason::application_close, 0, "QUIC connection close received");
        } else {
            write_connection_close(rv, "QUIC packet read failed: " + std::to_string(rv));
        }
        update_stats();
        drain_transport();
    }

    void
    on_timeout(std::chrono::steady_clock::time_point now) override {
        if (_server_parent) {
            for (auto &entry : _server_connections)
                entry.second->on_timeout(now);
            return;
        }
        if (!_conn)
            return;
        auto rv = ngtcp2_conn_handle_expiry(_conn, timestamp(now));
        if (rv == NGTCP2_ERR_IDLE_CLOSE) {
            queue_close_event(disconnect_reason::idle_timeout, 0, "QUIC idle timeout");
        } else if (rv < 0) {
            write_connection_close(rv, "QUIC timeout handling failed");
        }
        update_stats();
        drain_transport();
    }

    std::chrono::steady_clock::time_point
    next_timeout() const override {
        if (_server_parent) {
            auto next = std::chrono::steady_clock::time_point::max();
            for (auto const &entry : _server_connections)
                next = std::min(next, entry.second->next_timeout());
            return next;
        }
        if (!_conn)
            return std::chrono::steady_clock::time_point::max();
        return to_time_point(conn_get_expiry(_conn), _base);
    }

    bool
    wants_write() const noexcept override {
        if (_server_parent) {
            if (!_packets.empty())
                return true;
            for (auto const &entry : _server_connections) {
                if (entry.second->wants_write())
                    return true;
            }
            return false;
        }
        return !_packets.empty() || !_pending_streams.empty() || !_pending_datagrams.empty();
    }

    std::vector<packet>
    drain_packets() override {
        if (_server_parent) {
            for (auto &entry : _server_connections) {
                auto packets = entry.second->drain_packets();
                _packets.insert(_packets.end(), std::make_move_iterator(packets.begin()), std::make_move_iterator(packets.end()));
            }
        }
        drain_transport();
        auto out = std::move(_packets);
        _packets.clear();
        return out;
    }

    std::vector<backend_event>
    drain_events() override {
        if (_server_parent) {
            // Retire each closed connection's CID entries through the keys the child already
            // records, not by sweeping the whole index looking for its id. The index holds every
            // live connection's CIDs, so a scan-by-value costs O(active connections) per close —
            // a permanent tax on a server with connection churn, and a multi-second event-loop
            // stall when many connections close at once (graceful shutdown, or a partition
            // expiring every idle timer in the same turn).
            //
            // The child's key set is exactly complete: `_server_cid_index` is written in only
            // three places — `reconcile_server_cids` (which keeps `_indexed_scid_keys` mirroring
            // precisely what it emplaced) and the accept path's `_local_cid_key` /
            // `_original_dcid_key`. The `it->second == id` guard is kept so a CID that happens to
            // collide with another connection's entry is left alone, exactly as before.
            for (auto id : _server_closed_connections) {
                auto conn_it = _server_connections.find(id);
                if (conn_it == _server_connections.end())
                    continue; // already retired by an earlier duplicate id in this same list
                auto      *child = conn_it->second.get();
                const auto drop  = [&](std::string const &key) {
                    if (key.empty())
                        return;
                    if (auto it = _server_cid_index.find(key); it != _server_cid_index.end() && it->second == id)
                        _server_cid_index.erase(it);
                };
                for (auto const &key : child->_indexed_scid_keys)
                    drop(key);
                drop(child->_local_cid_key);
                drop(child->_original_dcid_key);
                _server_connections.erase(conn_it);
            }
            _server_closed_connections.clear();

            std::vector<std::uint64_t> closed_connections;
            for (auto &entry : _server_connections) {
                auto events = entry.second->drain_events();
                for (auto &event : events) {
                    event.connection_id = entry.first;
                    if (event.type == backend_event::kind::connection_closed)
                        closed_connections.push_back(entry.first);
                    _events.push_back(std::move(event));
                }
            }
            _server_closed_connections.insert(_server_closed_connections.end(), closed_connections.begin(), closed_connections.end());
        }
        auto out = std::move(_events);
        _events.clear();
        return out;
    }

    std::uint64_t
    open_stream(stream_direction direction) override {
        return open_stream(0, direction);
    }

    std::uint64_t
    open_stream(std::uint64_t connection_id, stream_direction direction) override {
        if (_server_parent) {
            if (auto *connection = server_connection(connection_id))
                return connection->open_stream(0, direction);
            if (_server_connections.size() == 1)
                return _server_connections.begin()->second->open_stream(0, direction);
            throw std::runtime_error("Cannot open a QUIC stream on an unknown connection");
        }
        if (!_conn)
            throw std::runtime_error("Cannot open a QUIC stream before a connection exists");
        int64_t    stream_id = -1;
        const auto rv        = direction == stream_direction::bidirectional ? ngtcp2_conn_open_bidi_stream(_conn, &stream_id, nullptr)
                                                                            : ngtcp2_conn_open_uni_stream(_conn, &stream_id, nullptr);
        if (rv != 0)
            throw std::runtime_error("ngtcp2 stream open failed with " + std::to_string(rv));
        return static_cast<std::uint64_t>(stream_id);
    }

    void
    send_stream_data(std::uint64_t connection_id, std::uint64_t stream_id, std::span<const std::byte> data, bool fin) override {
        if (_server_parent) {
            if (auto *connection = server_connection(connection_id))
                connection->send_stream_data(0, stream_id, data, fin);
            return;
        }
        if (_closing)
            return;
        if (_config.max_pending_stream_frames > 0 && _queued_stream_frames >= _config.max_pending_stream_frames) {
            write_application_close(static_cast<std::uint64_t>(disconnect_reason::buffer_overflow),
                                    "QUIC stream TX frame queue limit exceeded");
            queue_close_event(disconnect_reason::buffer_overflow, 0, "QUIC stream TX frame queue limit exceeded");
            return;
        }
        if (_config.max_pending_stream_bytes > 0
            && data.size() > _config.max_pending_stream_bytes - std::min(_queued_stream_bytes, _config.max_pending_stream_bytes)) {
            write_application_close(static_cast<std::uint64_t>(disconnect_reason::buffer_overflow), "QUIC stream TX byte queue limit exceeded");
            queue_close_event(disconnect_reason::buffer_overflow, 0, "QUIC stream TX byte queue limit exceeded");
            return;
        }
        auto         &next_offset = _next_stream_offsets[stream_id];
        queued_stream item;
        item.stream_id = stream_id;
        item.data.assign(data.begin(), data.end());
        item.offset = next_offset;
        item.fin    = fin;
        next_offset += item.data.size();
        _queued_stream_bytes += item.data.size();
        ++_queued_stream_frames;
        _pending_streams.push_back(std::move(item));
        drain_transport();
    }

    void
    extend_stream_credit(std::uint64_t connection_id, std::uint64_t stream_id, std::uint64_t bytes) override {
        if (_server_parent) {
            if (auto *connection = server_connection(connection_id))
                connection->extend_stream_credit(0, stream_id, bytes);
            return;
        }
        if (!_conn || bytes == 0)
            return;
        const auto id = static_cast<int64_t>(stream_id);
        const auto rv = ngtcp2_conn_extend_max_stream_offset(_conn, id, bytes);
        if (rv != 0) {
            write_connection_close(rv, "QUIC stream flow-control credit update failed");
            return;
        }
        ngtcp2_conn_extend_max_offset(_conn, bytes);
        drain_transport();
    }

    void
    reset_stream(std::uint64_t connection_id, std::uint64_t stream_id, std::uint64_t application_error_code) override {
        if (_server_parent) {
            if (auto *connection = server_connection(connection_id))
                connection->reset_stream(0, stream_id, application_error_code);
            return;
        }
        if (_conn)
            (void) ngtcp2_conn_shutdown_stream(_conn, 0, static_cast<int64_t>(stream_id), application_error_code);
        erase_queued_stream_data(stream_id);
        queue_stream_close_event(stream_id, stream_close_reason::reset, application_error_code, "stream reset");
    }

    void
    stop_stream(std::uint64_t connection_id, std::uint64_t stream_id, std::uint64_t application_error_code) override {
        if (_server_parent) {
            if (auto *connection = server_connection(connection_id))
                connection->stop_stream(0, stream_id, application_error_code);
            return;
        }
        if (_conn)
            (void) ngtcp2_conn_shutdown_stream_read(_conn, 0, static_cast<int64_t>(stream_id), application_error_code);
        queue_stream_close_event(stream_id, stream_close_reason::stop_sending, application_error_code, "stream stopped");
    }

    void
    send_datagram(std::uint64_t connection_id, std::span<const std::byte> data) override {
        if (_server_parent) {
            if (auto *connection = server_connection(connection_id))
                connection->send_datagram(0, data);
            return;
        }
        if (_closing || data.empty())
            return;
        if (!_config.enable_datagrams) {
            queue_close_event(disconnect_reason::protocol_error, 0, "QUIC DATAGRAM is not enabled");
            return;
        }
        if (_config.max_datagram_frame_size > 0 && data.size() > _config.max_datagram_frame_size) {
            queue_close_event(disconnect_reason::buffer_overflow, 0, "QUIC DATAGRAM payload exceeds configured maximum");
            return;
        }
        if (_config.max_pending_datagram_frames > 0 && _queued_datagram_frames >= _config.max_pending_datagram_frames) {
            queue_close_event(disconnect_reason::buffer_overflow, 0, "QUIC DATAGRAM frame queue limit exceeded");
            return;
        }
        if (_config.max_pending_datagram_bytes > 0
            && data.size() > _config.max_pending_datagram_bytes - std::min(_queued_datagram_bytes, _config.max_pending_datagram_bytes)) {
            queue_close_event(disconnect_reason::buffer_overflow, 0, "QUIC DATAGRAM byte queue limit exceeded");
            return;
        }

        queued_datagram item;
        item.id = _next_datagram_id++;
        item.data.assign(data.begin(), data.end());
        _queued_datagram_bytes += item.data.size();
        ++_queued_datagram_frames;
        _pending_datagrams.push_back(std::move(item));
        drain_transport();
    }

    void
    close(std::uint64_t application_error_code, std::string_view reason) override {
        if (_server_parent) {
            for (auto &entry : _server_connections)
                entry.second->close(application_error_code, reason);
            queue_close_event(disconnect_reason::application_close, application_error_code, reason);
            _stats.active_connections = 0;
            _stats.active_streams     = 0;
            _started                  = false;
            return;
        }
        if (_conn)
            write_application_close(application_error_code, reason);
        queue_close_event(disconnect_reason::application_close, application_error_code, reason);
        _stats.active_connections = 0;
        _stats.active_streams     = 0;
        _started                  = false;
    }

    void
    close_connection(std::uint64_t connection_id, std::uint64_t application_error_code, std::string_view reason) override {
        if (_server_parent) {
            if (auto *connection = server_connection(connection_id))
                connection->close(application_error_code, reason);
            return;
        }
        close(application_error_code, reason);
    }

    stats
    current_stats() const noexcept override {
        if (_server_parent) {
            auto aggregate               = _stats;
            aggregate.active_connections = _server_connections.size() - deferred_closed_connection_count();
            aggregate.active_streams     = 0;
            for (auto const &entry : _server_connections) {
                if (is_deferred_closed_connection(entry.first))
                    continue;
                auto child = entry.second->current_stats();
                aggregate.bytes_sent += child.bytes_sent;
                aggregate.bytes_received += child.bytes_received;
                aggregate.packets_sent += child.packets_sent;
                aggregate.packets_received += child.packets_received;
                aggregate.packets_lost += child.packets_lost;
                aggregate.retransmits += child.retransmits;
                aggregate.datagrams_sent += child.datagrams_sent;
                aggregate.datagrams_received += child.datagrams_received;
                aggregate.datagrams_lost += child.datagrams_lost;
                aggregate.datagrams_acked += child.datagrams_acked;
                aggregate.active_streams += child.active_streams;
                aggregate.bytes_in_flight += child.bytes_in_flight;
                aggregate.congestion_window += child.congestion_window;
                aggregate.smoothed_rtt_us = std::max(aggregate.smoothed_rtt_us, child.smoothed_rtt_us);
            }
            return aggregate;
        }
        return _stats;
    }

private:
    bool
    is_deferred_closed_connection(std::uint64_t connection_id) const noexcept {
        return std::find(_server_closed_connections.begin(), _server_closed_connections.end(), connection_id)
               != _server_closed_connections.end();
    }

    std::size_t
    deferred_closed_connection_count() const noexcept {
        std::size_t count = 0;
        for (std::size_t i = 0; i < _server_closed_connections.size(); ++i) {
            auto const id = _server_closed_connections[i];
            if (_server_connections.find(id) == _server_connections.end())
                continue;
            bool already_counted = false;
            for (std::size_t j = 0; j < i; ++j) {
                if (_server_closed_connections[j] == id) {
                    already_counted = true;
                    break;
                }
            }
            if (!already_counted)
                ++count;
        }
        return count;
    }

    native_backend *
    server_connection(std::uint64_t connection_id) noexcept {
        auto it = _server_connections.find(connection_id);
        return it == _server_connections.end() || is_deferred_closed_connection(connection_id) ? nullptr : it->second.get();
    }

    native_backend const *
    server_connection(std::uint64_t connection_id) const noexcept {
        auto it = _server_connections.find(connection_id);
        return it == _server_connections.end() || is_deferred_closed_connection(connection_id) ? nullptr : it->second.get();
    }

    void
    ensure_retry_secret() {
        if (!_retry_secret_ready) {
            // A failed CSPRNG here only weakens Retry to "tokens we minted this run" (still
            // unforgeable by an off-path attacker who never saw them); it is never a safety issue.
            fill_random(_retry_secret.data(), _retry_secret.size());
            _retry_secret_ready = true;
        }
    }

    // Send a Retry (address-validation challenge) for an untokened Initial: mint a fresh SCID, bind a
    // token to (client address, that SCID, the client's original DCID), and queue the Retry on the
    // listener's own packet vector. NO connection state is created — the client must re-send its
    // Initial carrying the token, proving it can receive at its claimed address, before we spend a
    // full handshake (ngtcp2_conn + SSL) on it. This is the anti-spoofed-Initial-flood defense.
    void
    send_retry(packet_view datagram, ngtcp2_pkt_hd const &hd) {
        ensure_retry_secret();
        std::uint8_t scidbuf[NGTCP2_MIN_INITIAL_DCIDLEN];
        if (!fill_random(scidbuf, sizeof(scidbuf)))
            return;
        ngtcp2_cid retry_scid;
        ngtcp2_cid_init(&retry_scid, scidbuf, sizeof(scidbuf));
        const auto   now = timestamp(std::chrono::steady_clock::now());
        std::uint8_t token[NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN];
        const auto   tokenlen = ngtcp2_crypto_generate_retry_token(token, _retry_secret.data(), _retry_secret.size(), hd.version,
                                                                   reinterpret_cast<const ngtcp2_sockaddr *>(&datagram.remote.sa_),
                                                                   datagram.remote.len(), &retry_scid, &hd.dcid, now);
        if (tokenlen < 0)
            return;
        std::uint8_t buf[NGTCP2_MAX_UDP_PAYLOAD_SIZE];
        const auto   n =
            ngtcp2_crypto_write_retry(buf, sizeof(buf), hd.version, &hd.scid, &retry_scid, &hd.dcid, token, static_cast<std::size_t>(tokenlen));
        if (n < 0)
            return;
        packet pkt;
        pkt.remote = datagram.remote;
        pkt.local  = datagram.local;
        pkt.payload.assign(reinterpret_cast<std::byte *>(buf), reinterpret_cast<std::byte *>(buf + n));
        _packets.push_back(std::move(pkt));
    }

    // Index every Source Connection ID the child currently advertises under its connection id, so
    // packets arriving on a rotated / migrated DCID route to it. ngtcp2 caps the active CID set
    // (active_connection_id_limit), so this is a handful of entries. Add-only: a retired CID left in
    // the index still routes to the owning child (which drops it), and drain_events() erases all of a
    // connection's CID entries when it closes — so no stale mapping outlives the connection.
    void
    reconcile_server_cids(native_backend *child) {
        if (!child || !child->_conn)
            return;
        // `conn_get_scid` (ngtcp2's get_scid2) returns only the SCIDs that are NOT RETIRED.
        // This used to emplace that active set and never remove anything, so every CID the peer
        // caused to be rotated out stayed in `_server_cid_index` for the whole life of the
        // connection — and a peer can drive rotation at will with RETIRE_CONNECTION_ID. The index
        // grew without bound on a long-lived connection (only `drain_events()` swept it, at
        // close), and stale entries kept routing retired-DCID packets into ngtcp2 just to be
        // rejected. It also made the function's name a lie: it added, it did not reconcile.
        //
        // Now it reconciles against the keys this child owns. `n` is bounded by the peer's
        // active_connection_id_limit (a handful), so the set difference below is trivially small —
        // and the common per-datagram case, where nothing rotated, exits after one comparison and
        // performs NO hash lookups at all, which is strictly cheaper than the previous
        // unconditional re-emplace of every active CID on every datagram.
        const std::size_t n = conn_get_scid(child->_conn, nullptr);
        if (n == 0)
            return;
        _scid_scratch.resize(n);
        conn_get_scid(child->_conn, _scid_scratch.data());

        _cid_key_scratch.clear();
        _cid_key_scratch.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            _cid_key_scratch.push_back(cid_key(_scid_scratch[i]));

        // Unchanged (ngtcp2 keeps a stable order): nothing to do.
        if (_cid_key_scratch == child->_indexed_scid_keys)
            return;

        // Retire the keys this child no longer advertises. `_local_cid_key` and
        // `_original_dcid_key` are installed by the accept path and must survive rotation, so they
        // are never removed here even if ngtcp2 has retired them as SCIDs.
        for (auto const &old_key : child->_indexed_scid_keys) {
            if (std::find(_cid_key_scratch.begin(), _cid_key_scratch.end(), old_key) != _cid_key_scratch.end())
                continue;
            if (old_key == child->_local_cid_key || old_key == child->_original_dcid_key)
                continue;
            if (auto it = _server_cid_index.find(old_key); it != _server_cid_index.end() && it->second == child->_connection_id)
                _server_cid_index.erase(it);
        }
        for (auto const &key : _cid_key_scratch)
            _server_cid_index.emplace(key, child->_connection_id);

        child->_indexed_scid_keys = _cid_key_scratch;
    }

    native_backend *
    find_or_accept_server_connection(packet_view datagram) {
        if (!_server_parent || datagram.payload.empty())
            return nullptr;

        ngtcp2_version_cid decoded{};
        const auto         decode_rv = ngtcp2_pkt_decode_version_cid(&decoded, reinterpret_cast<const uint8_t *>(datagram.payload.data()),
                                                                     datagram.payload.size(), NGTCP2_MIN_INITIAL_DCIDLEN);
        // Malformed packet: drop it. The previous `== 0 || dcidlen > 0` guard
        // let undecodable datagrams fall through to a full connection-accept
        // attempt — per-packet allocation cost an off-path attacker controls.
        if (decode_rv != 0 && decode_rv != NGTCP2_ERR_VERSION_NEGOTIATION)
            return nullptr;
        if (decoded.dcidlen > 0) {
            auto it = _server_cid_index.find(cid_key(decoded.dcid, decoded.dcidlen));
            if (it != _server_cid_index.end())
                return server_connection(it->second);
        }
        // Unsupported version with no matching connection: drop rather than
        // attempting to accept (Version Negotiation is not implemented yet).
        if (decode_rv != 0)
            return nullptr;

        if (_config.max_connections > 0 && _server_connections.size() >= _config.max_connections) {
            // At the connection cap: silently drop this new-connection datagram.
            // Do NOT queue_close_event() on the parent listener here — it pushes
            // a connection_closed event carrying the parent's sentinel
            // connection_id 0, which the endpoint interprets as "the listener
            // itself closed" (endpoint.h sets _open=false), and it is sticky
            // (_close_event_queued), so a single over-limit datagram would
            // permanently shut the whole server down — turning a per-client
            // connection-limit rejection into a full-server DoS. Dropping the
            // datagram rejects just this one new connection, keeps the server
            // up, and is repeatable; existing connections (matched by DCID
            // above) are unaffected and the client may retry after a timeout.
            return nullptr;
        }

        // Address validation via Retry (RFC 9000 §8.1): before spending a full handshake (ngtcp2_conn
        // + SSL) on a new Initial, make the client prove it can receive at its claimed source address.
        // An off-path attacker spoofing source addresses can never complete this round-trip, so it
        // cannot make us allocate connection state. Gated on the (previously inert) enable_stateless_retry.
        bool       retry_validated      = false;
        ngtcp2_cid validated_odcid      = {};
        ngtcp2_cid validated_retry_scid = {};
        if (_config.enable_stateless_retry) {
            ngtcp2_pkt_hd hd = {};
            if (ngtcp2_accept(&hd, reinterpret_cast<const uint8_t *>(datagram.payload.data()), datagram.payload.size()) == 0) {
                ensure_retry_secret();
                if (hd.tokenlen == 0) {
                    // First contact: answer with a Retry challenge and create no state.
                    send_retry(datagram, hd);
                    return nullptr;
                }
                // The re-sent Initial carries a token: verify it proves this exact (client address,
                // retry SCID) pair received our Retry. Invalid / expired / replayed -> drop.
                const ngtcp2_duration token_ttl = 10 * NGTCP2_SECONDS;
                const auto            now       = timestamp(std::chrono::steady_clock::now());
                if (ngtcp2_crypto_verify_retry_token(&validated_odcid, hd.token, hd.tokenlen, _retry_secret.data(), _retry_secret.size(),
                                                     hd.version, reinterpret_cast<const ngtcp2_sockaddr *>(&datagram.remote.sa_),
                                                     datagram.remote.len(), &hd.dcid, token_ttl, now)
                    != 0) {
                    return nullptr;
                }
                retry_validated      = true;
                validated_retry_scid = hd.dcid; // the SCID we issued in the Retry, now the client's DCID
            }
        }

        auto child = std::make_unique<native_backend>();
        child->configure(_config);
        child->start_server_child(_next_connection_id, _local, _alpn, _wire_alpn, _server_tls);
        if (retry_validated) {
            child->_has_retry_context = true;
            child->_retry_odcid       = validated_odcid;
            child->_retry_scid        = validated_retry_scid;
        }
        child->on_udp_datagram(datagram);
        if (!child->_conn)
            return nullptr;

        const auto id = _next_connection_id++;
        if (!child->_local_cid_key.empty())
            _server_cid_index.emplace(child->_local_cid_key, id);
        if (!child->_original_dcid_key.empty())
            _server_cid_index.emplace(child->_original_dcid_key, id);
        auto [it, inserted] = _server_connections.emplace(id, std::move(child));
        (void) inserted;
        return it->second.get();
    }

    void
    start_server_child(std::uint64_t connection_id, qb::io::endpoint const &local, std::vector<std::string> const &alpn_protocols,
                       std::vector<unsigned char> const &wire_alpn, tls_config const &tls) {
        _connection_id = connection_id;
        _server        = true;
        _server_parent = false;
        _local         = local;
        _alpn          = alpn_protocols;
        _wire_alpn     = wire_alpn;
        make_ssl_context(tls, true);
        _closing            = false;
        _close_event_queued = false;
        _started            = true;
    }

    static void
    validate_server_tls_config(tls_config const &tls) {
        if (tls.certificate_file.empty() || tls.private_key_file.empty())
            throw std::invalid_argument("QUIC server requires certificate and private key files");
        SSL_CTX *ctx = SSL_CTX_new(TLS_method());
        if (!ctx)
            throw std::runtime_error("SSL_CTX_new failed for QUIC server validation");
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        const auto cert_ok = SSL_CTX_use_certificate_chain_file(ctx, tls.certificate_file.string().c_str()) == 1;
        const auto key_ok  = SSL_CTX_use_PrivateKey_file(ctx, tls.private_key_file.string().c_str(), SSL_FILETYPE_PEM) == 1;
        SSL_CTX_free(ctx);
        if (!cert_ok)
            throw std::runtime_error("Failed to load QUIC server certificate");
        if (!key_ok)
            throw std::runtime_error("Failed to load QUIC server private key");
    }

    void
    make_ssl_context(tls_config const &tls, bool server) {
        _ssl_ctx = SSL_CTX_new(TLS_method());
        if (!_ssl_ctx)
            throw std::runtime_error("SSL_CTX_new failed for QUIC");
        SSL_CTX_set_min_proto_version(_ssl_ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(_ssl_ctx, TLS1_3_VERSION);
        if (server) {
            if (tls.certificate_file.empty() || tls.private_key_file.empty())
                throw std::invalid_argument("QUIC server requires certificate and private key files");
            if (SSL_CTX_use_certificate_chain_file(_ssl_ctx, tls.certificate_file.string().c_str()) != 1)
                throw std::runtime_error("Failed to load QUIC server certificate");
            if (SSL_CTX_use_PrivateKey_file(_ssl_ctx, tls.private_key_file.string().c_str(), SSL_FILETYPE_PEM) != 1)
                throw std::runtime_error("Failed to load QUIC server private key");
            SSL_CTX_set_alpn_select_cb(_ssl_ctx, alpn_select_cb, &_wire_alpn);
        } else if (tls.verify_peer) {
            SSL_CTX_set_verify(_ssl_ctx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(_ssl_ctx);
        } else {
            SSL_CTX_set_verify(_ssl_ctx, SSL_VERIFY_NONE, nullptr);
        }

        _ssl = SSL_new(_ssl_ctx);
        if (!_ssl)
            throw std::runtime_error("SSL_new failed for QUIC");

        if (ngtcp2_crypto_ossl_ctx_new(&_crypto_ctx, nullptr) != 0)
            throw std::runtime_error("ngtcp2 crypto context allocation failed");
        ngtcp2_crypto_ossl_ctx_set_ssl(_crypto_ctx, _ssl);

        if (server) {
            if (ngtcp2_crypto_ossl_configure_server_session(_ssl) != 0)
                throw std::runtime_error("ngtcp2 server TLS session configuration failed");
            SSL_set_accept_state(_ssl);
        } else {
            if (ngtcp2_crypto_ossl_configure_client_session(_ssl) != 0)
                throw std::runtime_error("ngtcp2 client TLS session configuration failed");
            if (!tls.server_name.empty()) {
                SSL_set_tlsext_host_name(_ssl, tls.server_name.c_str());
                // Bind hostname verification: SSL_VERIFY_PEER validates the
                // chain but, without this, accepts any CA-trusted certificate
                // for any host (MITM). Match the cert against the server name.
                if (tls.verify_peer) {
                    SSL_set_hostflags(_ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                    SSL_set1_host(_ssl, tls.server_name.c_str());
                }
            }
            SSL_set_alpn_protos(_ssl, _wire_alpn.data(), static_cast<unsigned int>(_wire_alpn.size()));
            SSL_set_connect_state(_ssl);
        }
        _conn_ref.get_conn  = get_conn_cb;
        _conn_ref.user_data = &_conn;
        SSL_set_app_data(_ssl, &_conn_ref);
    }

    ngtcp2_settings
    make_native_settings() const {
        ngtcp2_settings native_settings;
        ngtcp2_settings_default(&native_settings);
        native_settings.initial_ts              = timestamp(_base);
        native_settings.max_tx_udp_payload_size = NGTCP2_MAX_UDP_PAYLOAD_SIZE;
        native_settings.handshake_timeout =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(_config.handshake_timeout).count())
            * NGTCP2_MILLISECONDS;
        return native_settings;
    }

    ngtcp2_transport_params
    make_transport_params() const {
        ngtcp2_transport_params params;
        ngtcp2_transport_params_default(&params);
        params.initial_max_stream_data_bidi_local  = _config.max_stream_data_bidi_local;
        params.initial_max_stream_data_bidi_remote = _config.max_stream_data_bidi_remote;
        params.initial_max_stream_data_uni         = _config.max_stream_data_uni;
        params.initial_max_data                    = _config.connection_recv_window;
        params.initial_max_streams_bidi            = _config.max_streams_bidi;
        params.initial_max_streams_uni             = _config.max_streams_uni;
        params.max_idle_timeout =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(_config.idle_timeout).count())
            * NGTCP2_MILLISECONDS;
        params.max_udp_payload_size     = NGTCP2_MAX_UDP_PAYLOAD_SIZE;
        params.max_datagram_frame_size  = _config.enable_datagrams ? _config.max_datagram_frame_size : 0;
        params.disable_active_migration = 0;
        return params;
    }

    ngtcp2_callbacks
    make_callbacks(bool server) const {
        ngtcp2_callbacks callbacks{};
        if (server)
            callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
        else {
            callbacks.client_initial      = ngtcp2_crypto_client_initial_cb;
            callbacks.recv_retry          = ngtcp2_crypto_recv_retry_cb;
            callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
        }
        callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
        callbacks.encrypt          = ngtcp2_crypto_encrypt_cb;
        callbacks.decrypt          = ngtcp2_crypto_decrypt_cb;
        callbacks.hp_mask          = ngtcp2_crypto_hp_mask_cb;
        callbacks.rand             = rand_cb;
#if QB_IO_NGTCP2_HAS_CALLBACKS_V3
        callbacks.get_new_connection_id2 = new_connection_id_cb;
#else
        callbacks.get_new_connection_id = new_connection_id_cb;
#endif
        callbacks.update_key               = ngtcp2_crypto_update_key_cb;
        callbacks.delete_crypto_aead_ctx   = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
#if QB_IO_NGTCP2_HAS_CALLBACKS_V3
        callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
#else
        callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
#endif
        // Every own callback below touches heap state (event queue / maps) and so can throw; route
        // each through guarded<>::call so a bad_alloc becomes NGTCP2_ERR_CALLBACK_FAILURE instead of
        // unwinding through ngtcp2's C frames. (The crypto_* callbacks above are ngtcp2's own C code
        // and never throw; rand/new_connection_id fill fixed buffers and already fail closed.)
        callbacks.recv_stream_data         = &guarded<recv_stream_data_cb>::call;
        callbacks.acked_stream_data_offset = &guarded<acked_stream_data_offset_cb>::call;
        callbacks.stream_open              = &guarded<stream_open_cb>::call;
        callbacks.stream_close             = &guarded<stream_close_cb>::call;
        callbacks.stream_reset             = &guarded<stream_reset_cb>::call;
        callbacks.stream_stop_sending      = &guarded<stream_stop_sending_cb>::call;
        callbacks.recv_datagram            = &guarded<recv_datagram_cb>::call;
        callbacks.ack_datagram             = &guarded<ack_datagram_cb>::call;
        callbacks.lost_datagram            = &guarded<lost_datagram_cb>::call;
        callbacks.handshake_completed      = &guarded<handshake_completed_cb>::call;
        return callbacks;
    }

    void
    make_client_connection() {
        auto native_settings = make_native_settings();
        auto params          = make_transport_params();
        auto callbacks       = make_callbacks(false);

        uint8_t dcidbuf[NGTCP2_MIN_INITIAL_DCIDLEN];
        uint8_t scidbuf[NGTCP2_MIN_INITIAL_DCIDLEN];
        // Fail closed on CSPRNG failure: fill_random zeroes the buffer and returns false, and an
        // all-zero initial connection ID is predictable/linkable. Mirrors fill_new_connection_id.
        if (!fill_random(dcidbuf, sizeof(dcidbuf)) || !fill_random(scidbuf, sizeof(scidbuf)))
            throw std::runtime_error("QUIC client: CSPRNG failed to generate initial connection IDs");
        ngtcp2_cid dcid;
        ngtcp2_cid scid;
        ngtcp2_cid_init(&dcid, dcidbuf, sizeof(dcidbuf));
        ngtcp2_cid_init(&scid, scidbuf, sizeof(scidbuf));

        ngtcp2_path_storage ps;
        ngtcp2_path_storage_init(&ps, reinterpret_cast<const ngtcp2_sockaddr *>(&_local.sa_), _local.len(),
                                 reinterpret_cast<const ngtcp2_sockaddr *>(&_remote.sa_), _remote.len(), nullptr);

        const auto rv =
            ngtcp2_conn_client_new(&_conn, &dcid, &scid, &ps.path, NGTCP2_PROTO_VER_V1, &callbacks, &native_settings, &params, nullptr, this);
        if (rv != 0)
            throw std::runtime_error("ngtcp2_conn_client_new failed with " + std::to_string(rv));
    }

    bool
    accept_server_connection(packet_view datagram) {
        if (!_server || !_started)
            return false;

        ngtcp2_pkt_hd hd{};
        const auto    accept_rv = ngtcp2_accept(&hd, reinterpret_cast<const uint8_t *>(datagram.payload.data()), datagram.payload.size());
        if (accept_rv != 0) {
            queue_close_event(disconnect_reason::handshake_failed, static_cast<std::uint64_t>(accept_rv),
                              "QUIC server rejected initial packet");
            return false;
        }
        _original_dcid_key = cid_key(hd.dcid);

        _remote = datagram.remote;
        _local  = datagram.local;

        auto native_settings = make_native_settings();
        auto params          = make_transport_params();
        if (_has_retry_context) {
            // Accepted after a validated Retry: the client's ORIGINAL DCID is the one recovered from
            // the token (hd.dcid is now the retry SCID we issued), and we must echo the retry SCID so
            // the client can confirm the Retry it received was genuinely ours (RFC 9000 §7.3).
            params.original_dcid         = _retry_odcid;
            params.original_dcid_present = 1;
            params.retry_scid            = _retry_scid;
            params.retry_scid_present    = 1;
        } else {
            params.original_dcid         = hd.dcid;
            params.original_dcid_present = 1;
        }

        uint8_t scidbuf[NGTCP2_MIN_INITIAL_DCIDLEN];
        // Fail closed on CSPRNG failure (see make_client_connection): a zeroed server SCID is
        // predictable/linkable. Returning false discards the half-built child (its _conn stays null).
        if (!fill_random(scidbuf, sizeof(scidbuf)))
            return false;
        ngtcp2_cid scid;
        ngtcp2_cid_init(&scid, scidbuf, sizeof(scidbuf));
        _local_cid_key = cid_key(scid);

        ngtcp2_path_storage ps;
        ngtcp2_path_storage_init(&ps, reinterpret_cast<const ngtcp2_sockaddr *>(&_local.sa_), _local.len(),
                                 reinterpret_cast<const ngtcp2_sockaddr *>(&_remote.sa_), _remote.len(), nullptr);

        auto       callbacks = make_callbacks(true);
        const auto rv =
            ngtcp2_conn_server_new(&_conn, &hd.scid, &scid, &ps.path, hd.version, &callbacks, &native_settings, &params, nullptr, this);
        if (rv != 0) {
            queue_close_event(disconnect_reason::handshake_failed, static_cast<std::uint64_t>(rv),
                              "ngtcp2 server connection allocation failed");
            return false;
        }

        ngtcp2_conn_set_tls_native_handle(_conn, _crypto_ctx);
        _stats.active_connections = 1;
        return true;
    }

    // Fill an outgoing packet's addresses from the network path ngtcp2 wrote back into `ps` after a
    // writev — i.e. the peer's CURRENT, path-validated address — rather than the address cached at
    // accept time (`_remote`). This is what lets a peer that migrates or is NAT-rebound keep
    // receiving: ngtcp2 has already validated the new path; sending to the stale `_remote` would
    // black-hole every packet and idle-time-out the connection. Falls back to the cached endpoint
    // only if ngtcp2 left the path empty (should not happen once a packet was produced).
    void
    fill_packet_path(packet &pkt, ngtcp2_path_storage const &ps) const {
        if (ps.path.remote.addrlen > 0)
            pkt.remote.as_is_raw(ps.path.remote.addr, ps.path.remote.addrlen);
        else
            pkt.remote = _remote;
        if (ps.path.local.addrlen > 0)
            pkt.local.as_is_raw(ps.path.local.addr, ps.path.local.addrlen);
        else
            pkt.local = _local;
    }

    void
    drain_transport() {
        if (!_conn || !_started)
            return;
        for (;;) {
            std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> buf{};
            ngtcp2_path_storage                              ps;
            ngtcp2_path_storage_zero(&ps);
            ngtcp2_pkt_info  pi{};
            ngtcp2_ssize     datalen   = -1;
            uint32_t         flags     = 0;
            int64_t          stream_id = -1;
            ngtcp2_vec       vec{};
            queued_datagram *datagram = nullptr;

            if (!_pending_streams.empty()) {
                auto &item = _pending_streams.front();
                stream_id  = static_cast<int64_t>(item.stream_id);
                vec.len    = item.data.size() - item.sent_offset;
                vec.base   = vec.len == 0 ? nullptr : reinterpret_cast<uint8_t *>(item.data.data() + item.sent_offset);
                if (item.fin)
                    flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            } else if (!_pending_datagrams.empty()) {
                datagram = &_pending_datagrams.front();
                vec.base = reinterpret_cast<uint8_t *>(datagram->data.data());
                vec.len  = datagram->data.size();
            }

            int        datagram_accepted = 0;
            const auto now               = timestamp(std::chrono::steady_clock::now());
            const auto written = datagram ? ngtcp2_conn_writev_datagram(_conn, &ps.path, &pi, buf.data(), buf.size(), &datagram_accepted,
                                                                        NGTCP2_WRITE_DATAGRAM_FLAG_NONE, datagram->id, &vec, 1, now)
                                          : ngtcp2_conn_writev_stream(_conn, &ps.path, &pi, buf.data(), buf.size(), &datalen, flags, stream_id,
                                                                      vec.len > 0 ? &vec : nullptr, vec.len > 0 ? 1 : 0, now);

            if (written == 0)
                break;
            if (written < 0) {
                if (datagram) {
                    if (written == NGTCP2_ERR_INVALID_STATE || written == NGTCP2_ERR_INVALID_ARGUMENT) {
                        drop_front_datagram(false);
                        continue;
                    }
                    queue_close_event(disconnect_reason::transport_error, static_cast<std::uint64_t>(-written),
                                      "QUIC DATAGRAM packet write failed");
                    break;
                }
                if (written == NGTCP2_ERR_STREAM_DATA_BLOCKED)
                    break;
                if (written == NGTCP2_ERR_CLOSING || written == NGTCP2_ERR_DRAINING)
                    break;
                if (written == NGTCP2_ERR_STREAM_NOT_FOUND || written == NGTCP2_ERR_STREAM_SHUT_WR) {
                    if (!_pending_streams.empty()) {
                        _queued_stream_bytes -= std::min<std::uint64_t>(_queued_stream_bytes, _pending_streams.front().data.size());
                        if (_queued_stream_frames > 0)
                            --_queued_stream_frames;
                        _pending_streams.pop_front();
                    }
                    continue;
                }
                queue_close_event(disconnect_reason::transport_error, static_cast<std::uint64_t>(-written),
                                  "QUIC packet write failed: " + std::to_string(written));
                break;
            }

            packet pkt;
            fill_packet_path(pkt, ps);
            pkt.payload.assign(reinterpret_cast<std::byte *>(buf.data()), reinterpret_cast<std::byte *>(buf.data() + written));
            _packets.push_back(std::move(pkt));
            ++_stats.packets_sent;
            _stats.bytes_sent += static_cast<std::uint64_t>(written);

            if (datagram && datagram_accepted) {
                _inflight_datagrams.emplace(datagram->id, datagram->data.size());
                ++_stats.datagrams_sent;
                _pending_datagrams.pop_front();
            } else if (datalen >= 0 && !_pending_streams.empty()) {
                auto      &item      = _pending_streams.front();
                const auto remaining = item.data.size() - item.sent_offset;
                const auto consumed  = std::min<std::size_t>(remaining, static_cast<std::size_t>(datalen));
                item.sent_offset += consumed;
                if (item.sent_offset == item.data.size()) {
                    _inflight_streams.push_back(std::move(item));
                    _pending_streams.pop_front();
                }
            }
            ngtcp2_conn_update_pkt_tx_time(_conn, now);
        }
        update_stats();
    }

    void
    write_application_close(std::uint64_t application_error_code, std::string_view reason) {
        if (!_conn || _closing)
            return;
        // A QUIC application error code is encoded as a varint and MUST be < 2^62,
        // otherwise ngtcp2 aborts in ngtcp2_put_uvarintlen() while encoding the
        // CONNECTION_CLOSE frame. Some internal call sites pass a negative
        // disconnect_reason cast to uint64_t (e.g. transport_error == -1 ->
        // 0xffff...), which is out of range; clamp those to 0 (no specific error).
        constexpr std::uint64_t kMaxQuicVarint = (static_cast<std::uint64_t>(1) << 62) - 1;
        if (application_error_code > kMaxQuicVarint)
            application_error_code = 0;
        ngtcp2_ccerr ccerr;
        ngtcp2_ccerr_default(&ccerr);
        ngtcp2_ccerr_set_application_error(&ccerr, application_error_code, reinterpret_cast<const uint8_t *>(reason.data()), reason.size());
        write_connection_close_packet(ccerr);
    }

    void
    write_connection_close(int liberr, std::string_view reason) {
        if (!_conn || _closing)
            return;
        ngtcp2_ccerr ccerr;
        ngtcp2_ccerr_default(&ccerr);
        ngtcp2_ccerr_set_liberr(&ccerr, liberr, reinterpret_cast<const uint8_t *>(reason.data()), reason.size());
        write_connection_close_packet(ccerr);
        queue_close_event(disconnect_reason::transport_error, static_cast<std::uint64_t>(-liberr), reason);
    }

    void
    write_connection_close_packet(ngtcp2_ccerr const &ccerr) {
        std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> buf{};
        ngtcp2_path_storage                              ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};
        const auto      written = ngtcp2_conn_write_connection_close(_conn, &ps.path, &pi, buf.data(), buf.size(), &ccerr,
                                                                     timestamp(std::chrono::steady_clock::now()));
        if (written > 0) {
            packet pkt;
            fill_packet_path(pkt, ps);
            pkt.payload.assign(reinterpret_cast<std::byte *>(buf.data()), reinterpret_cast<std::byte *>(buf.data() + written));
            _packets.push_back(std::move(pkt));
            ++_stats.packets_sent;
            _stats.bytes_sent += static_cast<std::uint64_t>(written);
        }
        _closing = true;
    }

    ngtcp2_tstamp
    timestamp(std::chrono::steady_clock::time_point now) const {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - _base).count());
    }

    void
    update_stats() noexcept {
        if (!_conn)
            return;
        ngtcp2_conn_info info{};
        ngtcp2_conn_get_conn_info(_conn, &info);
        _stats.smoothed_rtt_us   = info.smoothed_rtt / NGTCP2_MICROSECONDS;
        _stats.congestion_window = info.cwnd;
        _stats.bytes_in_flight   = info.bytes_in_flight;
        _stats.packets_lost      = info.pkt_lost;
    }

    void
    queue_close_event(disconnect_reason why, std::uint64_t code, std::string_view reason) {
        if (_close_event_queued)
            return;
        _close_event_queued       = true;
        _stats.active_connections = 0;
        _stats.active_streams     = 0;
        _started                  = false;
        backend_event event;
        event.type              = backend_event::kind::connection_closed;
        event.connection_id     = _connection_id;
        event.error_code        = code;
        event.text              = std::string(reason);
        event.connection_reason = why;
        _events.push_back(std::move(event));
    }

    void
    queue_stream_close_event(std::uint64_t stream_id, stream_close_reason why, std::uint64_t code, std::string_view reason) {
        backend_event event;
        event.type          = backend_event::kind::stream_closed;
        event.connection_id = _connection_id;
        event.stream_id     = stream_id;
        event.error_code    = code;
        event.text          = std::string(reason);
        event.stream_reason = why;
        _events.push_back(std::move(event));
    }

    void
    erase_queued_stream_data(std::uint64_t id) {
        for (auto it = _pending_streams.begin(); it != _pending_streams.end();) {
            if (it->stream_id == id) {
                _queued_stream_bytes -= std::min<std::uint64_t>(_queued_stream_bytes, it->data.size());
                if (_queued_stream_frames > 0)
                    --_queued_stream_frames;
                it = _pending_streams.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = _inflight_streams.begin(); it != _inflight_streams.end();) {
            if (it->stream_id == id) {
                _queued_stream_bytes -= std::min<std::uint64_t>(_queued_stream_bytes, it->data.size());
                if (_queued_stream_frames > 0)
                    --_queued_stream_frames;
                it = _inflight_streams.erase(it);
            } else {
                ++it;
            }
        }
    }

    void
    drop_front_datagram(bool count_lost) {
        if (_pending_datagrams.empty())
            return;
        auto const &item = _pending_datagrams.front();
        _queued_datagram_bytes -= std::min<std::uint64_t>(_queued_datagram_bytes, item.data.size());
        if (_queued_datagram_frames > 0)
            --_queued_datagram_frames;
        if (count_lost)
            ++_stats.datagrams_lost;
        _pending_datagrams.pop_front();
    }

    void
    release_inflight_datagram(std::uint64_t id, bool acked) {
        auto it = _inflight_datagrams.find(id);
        if (it == _inflight_datagrams.end())
            return;
        _queued_datagram_bytes -= std::min<std::uint64_t>(_queued_datagram_bytes, it->second);
        if (_queued_datagram_frames > 0)
            --_queued_datagram_frames;
        if (acked)
            ++_stats.datagrams_acked;
        else
            ++_stats.datagrams_lost;
        _inflight_datagrams.erase(it);
    }

    static int
    recv_stream_data_cb(ngtcp2_conn *, uint32_t flags, int64_t stream_id, uint64_t, const uint8_t *data, size_t datalen, void *user_data,
                        void *) {
        auto         *self = static_cast<native_backend *>(user_data);
        backend_event event;
        event.type          = backend_event::kind::stream_data;
        event.connection_id = self->_connection_id;
        event.stream_id     = static_cast<std::uint64_t>(stream_id);
        event.payload.assign(reinterpret_cast<const std::byte *>(data), reinterpret_cast<const std::byte *>(data + datalen));
        event.error_code = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0 ? 1 : 0;
        if (event.error_code != 0)
            self->_stream_fin_seen[static_cast<std::uint64_t>(stream_id)] = true;
        self->_events.push_back(std::move(event));
        return 0;
    }

    static int
    acked_stream_data_offset_cb(ngtcp2_conn *, int64_t stream_id, uint64_t offset, uint64_t datalen, void *user_data, void *) {
        auto      *self    = static_cast<native_backend *>(user_data);
        const auto id      = static_cast<std::uint64_t>(stream_id);
        const auto ack_end = offset + datalen;
        if (datalen > 0) {
            backend_event event;
            event.type          = backend_event::kind::stream_data_acked;
            event.connection_id = self->_connection_id;
            event.stream_id     = id;
            event.error_code    = datalen;
            self->_events.push_back(std::move(event));
        }
        for (auto it = self->_inflight_streams.begin(); it != self->_inflight_streams.end();) {
            const auto item_end     = it->offset + it->data.size();
            const bool zero_len_fin = it->data.empty() && it->fin && offset == it->offset;
            if (it->stream_id == id && (ack_end >= item_end || zero_len_fin)) {
                self->_queued_stream_bytes -= std::min<std::uint64_t>(self->_queued_stream_bytes, it->data.size());
                if (self->_queued_stream_frames > 0)
                    --self->_queued_stream_frames;
                it = self->_inflight_streams.erase(it);
            } else {
                ++it;
            }
        }
        return 0;
    }

    static int
    stream_open_cb(ngtcp2_conn *, int64_t stream_id, void *user_data) {
        auto *self = static_cast<native_backend *>(user_data);
        ++self->_stats.active_streams;
        self->_events.push_back({backend_event::kind::stream_started, self->_connection_id, static_cast<std::uint64_t>(stream_id), 0, {}, {}});
        return 0;
    }

    static int
    stream_close_cb(ngtcp2_conn *, uint32_t, int64_t stream_id, uint64_t app_error_code, void *user_data, void *) {
        auto *self = static_cast<native_backend *>(user_data);
        if (self->_stats.active_streams > 0)
            --self->_stats.active_streams;
        const auto id = static_cast<std::uint64_t>(stream_id);
        // Non-inserting lookup: operator[] would default-insert a `false` entry
        // for `id` only to erase it again on the line below. find() avoids the
        // transient insert while preserving the "absent == not-yet-seen-FIN"
        // semantics that drives the synthetic FIN event.
        const auto fin_it           = self->_stream_fin_seen.find(id);
        const bool fin_already_seen = fin_it != self->_stream_fin_seen.end() && fin_it->second;
        if (app_error_code == 0 && !fin_already_seen) {
            backend_event fin_event;
            fin_event.type          = backend_event::kind::stream_data;
            fin_event.connection_id = self->_connection_id;
            fin_event.stream_id     = id;
            fin_event.error_code    = 1;
            self->_events.push_back(std::move(fin_event));
        }
        self->_stream_fin_seen.erase(id);
        self->erase_queued_stream_data(id);
        self->_next_stream_offsets.erase(id);
        self->queue_stream_close_event(static_cast<std::uint64_t>(stream_id),
                                       app_error_code == 0 ? stream_close_reason::finished : stream_close_reason::reset, app_error_code, {});
        return 0;
    }

    static int
    stream_reset_cb(ngtcp2_conn *, int64_t stream_id, uint64_t, uint64_t app_error_code, void *user_data, void *) {
        auto      *self = static_cast<native_backend *>(user_data);
        const auto id   = static_cast<std::uint64_t>(stream_id);
        self->erase_queued_stream_data(id);
        self->_next_stream_offsets.erase(id);
        self->queue_stream_close_event(id, stream_close_reason::reset, app_error_code, "stream reset by peer");
        return 0;
    }

    static int
    stream_stop_sending_cb(ngtcp2_conn *, int64_t stream_id, uint64_t app_error_code, void *user_data, void *) {
        auto      *self = static_cast<native_backend *>(user_data);
        const auto id   = static_cast<std::uint64_t>(stream_id);
        self->erase_queued_stream_data(id);
        self->queue_stream_close_event(id, stream_close_reason::stop_sending, app_error_code, "stream stop sending");
        return 0;
    }

    static int
    recv_datagram_cb(ngtcp2_conn *, uint32_t, const uint8_t *data, size_t datalen, void *user_data) {
        auto         *self = static_cast<native_backend *>(user_data);
        backend_event event;
        event.type          = backend_event::kind::datagram;
        event.connection_id = self->_connection_id;
        event.payload.assign(reinterpret_cast<const std::byte *>(data), reinterpret_cast<const std::byte *>(data + datalen));
        self->_events.push_back(std::move(event));
        ++self->_stats.datagrams_received;
        return 0;
    }

    static int
    ack_datagram_cb(ngtcp2_conn *, uint64_t datagram_id, void *user_data) {
        auto *self = static_cast<native_backend *>(user_data);
        self->release_inflight_datagram(datagram_id, true);
        return 0;
    }

    static int
    lost_datagram_cb(ngtcp2_conn *, uint64_t datagram_id, void *user_data) {
        auto *self = static_cast<native_backend *>(user_data);
        self->release_inflight_datagram(datagram_id, false);
        return 0;
    }

    static int
    handshake_completed_cb(ngtcp2_conn *, void *user_data) {
        auto *self = static_cast<native_backend *>(user_data);
        if (!self->_alpn.empty() && self->negotiated_alpn().empty()) {
            self->write_application_close(static_cast<std::uint64_t>(disconnect_reason::protocol_error), "QUIC ALPN negotiation failed");
            self->queue_close_event(disconnect_reason::handshake_failed, 0, "QUIC ALPN negotiation failed");
            return 0;
        }
        self->_events.push_back({backend_event::kind::connected, self->_connection_id, 0, 0, self->negotiated_alpn(), {}});
        return 0;
    }
};

} // namespace
#undef QB_IO_NGTCP2_HAS_CALLBACKS_V3
#endif

backend_info
native_backend_info() noexcept {
#ifdef QB_HAS_QUIC
    auto const *info = ngtcp2_version(NGTCP2_VERSION_AGE);
    return {"libngtcp2", info && info->version_str ? std::string_view{info->version_str} : std::string_view{}, ngtcp2_crypto_ossl_init() == 0};
#else
    return {"none", {}, false};
#endif
}

bool
native_backend_ready() noexcept {
    return available() && native_backend_info().crypto_initialized;
}

std::unique_ptr<backend>
make_native_backend() {
#ifdef QB_HAS_QUIC
    return std::make_unique<native_backend>();
#else
    throw std::runtime_error(unavailable_reason());
#endif
}

} // namespace qb::io::quic
