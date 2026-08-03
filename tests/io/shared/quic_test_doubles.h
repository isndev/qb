/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/quic_test_doubles.h
 * @brief In-memory QUIC backend mock + endpoint/session harnesses for the qb-io QUIC suite.
 *
 * The qb async QUIC stack is layered: `qb::io::async::quic::endpoint` (and the `connector` /
 * `server` facades built on it) own the I/O lifecycle and event dispatch, while a swappable
 * `qb::io::quic::backend` (production: libngtcp2) does the wire work. Every behaviour of the
 * facade layer — settings plumbing, stream/datagram delegation, event fan-out, session
 * registration/teardown — can therefore be exercised *without a real QUIC stack* by injecting a
 * mock backend. `FakeQuicBackend` is that mock: it implements the full `backend` vtable, records
 * the arguments of each call, counts invocations per method, and lets a test hand-feed
 * `backend_event`s / `packet`s back up into the facade via `queued_events` / `queued_packets`.
 * Because it is a pure double (no native types), it compiles and runs whether or not
 * `QB_HAS_QUIC` is defined — this is what makes the `unit/quic` tier hermetic.
 *
 * The harness classes are the minimal `Derived` types the CRTP facades expect:
 *   - `CallbackQuicClient`  — a `connector<…, void>` that tallies every event callback and
 *                             records the last close/stream metadata (used as the universal
 *                             event-dispatch probe).
 *   - `CallbackQuicServer`  — a `server<…, DummyQuicStreamSession>` counterpart that additionally
 *                             counts spawned stream sessions and accumulates acked bytes.
 *   - `SessionQuicClient`   — a `connector<…, DummyQuicStreamSession>` that inherits the facade
 *                             verbatim, used to exercise the local-stream-session helpers.
 *   - `DummyQuicStreamSession` — a do-nothing `use<>::quic::session` (a StreamSession placeholder).
 *   - `EchoQuicStreamSession` / `EchoQuicProtocol` / `EchoQuicServer` — a 4-byte framed echo:
 *     the protocol slices fixed 4-byte messages off the inbound pipe, records them, and publishes
 *     "ack!" back, so a test can prove the full remote-stream-data → session → response → flush
 *     round-trip through the facade.
 *
 * Spec §7.4 fix (carried here as the single source of truth): the original `CallbackQuicClient`
 * multiplexed two unrelated quantities through one `last_error_code` member — the acked byte
 * count from `stream_data_acked` *and* the close error code from `connection_closed`. That made
 * an ack silently clobber a previously-recorded close code (and vice-versa), so an assertion on
 * one was only valid until the other event arrived. They are split into two dedicated fields:
 * `last_acked_bytes` (written only by `stream_data_acked`) and `last_close_error_code` (written
 * only by `connection_closed`). Consumers must assert against the field that matches the event
 * they are probing.
 *
 * The native-only helpers (`quic_payload`, `deliver_quic_packets`) drive two real backends
 * against each other for the `system/quic/quic-handshake` live tests and are compiled only under
 * `QB_HAS_QUIC`; the mock doubles above never touch them.
 *
 * Hoisted verbatim (modulo the §7.4 split) from the former `test-quic.cpp` anonymous namespace so
 * the `unit/quic` and `system/quic` split targets share a single source of truth.
 */

#ifndef QB_IO_TESTS_SHARED_QUIC_TEST_DOUBLES_H
#define QB_IO_TESTS_SHARED_QUIC_TEST_DOUBLES_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>

namespace qb::io::test {

// ---------------------------------------------------------------------------
// Mock `qb::io::quic::backend`: full vtable, per-method call counters + last-arg
// capture, plus `queued_events` / `queued_packets` so a test can feed the facade.
// Pure double — no native QUIC types, compiles with or without QB_HAS_QUIC.
// ---------------------------------------------------------------------------
class FakeQuicBackend final : public qb::io::quic::backend {
public:
    qb::io::quic::settings                   last_settings{};
    qb::io::quic::stats                      stats{};
    std::vector<std::string>                 last_alpn;
    qb::io::endpoint                         last_local;
    qb::io::endpoint                         last_remote;
    qb::io::quic::tls_config                 last_tls;
    int                                      configure_calls             = 0;
    int                                      start_server_calls          = 0;
    int                                      start_client_calls          = 0;
    int                                      open_bidi_calls             = 0;
    int                                      open_uni_calls              = 0;
    int                                      close_calls                 = 0;
    int                                      close_connection_calls      = 0;
    std::uint64_t                            closed_connection_id        = 0;
    std::uint64_t                            last_open_connection_id     = 0;
    int                                      send_stream_data_calls      = 0;
    std::uint64_t                            sent_connection_id          = 0;
    std::uint64_t                            sent_stream_id              = 0;
    std::size_t                              sent_stream_bytes           = 0;
    bool                                     sent_stream_fin             = false;
    int                                      extend_stream_credit_calls  = 0;
    std::uint64_t                            extended_stream_id          = 0;
    std::uint64_t                            extended_bytes              = 0;
    int                                      send_datagram_calls         = 0;
    std::uint64_t                            sent_datagram_connection_id = 0;
    std::size_t                              sent_datagram_bytes         = 0;
    int                                      reset_stream_calls          = 0;
    int                                      stop_stream_calls           = 0;
    std::uint64_t                            reset_stream_id             = 0;
    std::uint64_t                            reset_stream_code           = 0;
    std::uint64_t                            stop_stream_id              = 0;
    std::uint64_t                            stop_stream_code            = 0;
    std::uint64_t                            close_code                  = 0;
    std::string                              close_reason;
    bool                                     close_on_send_stream_data = false;
    int                                      timeout_calls             = 0;
    std::vector<qb::io::quic::backend_event> queued_events;
    std::vector<qb::io::quic::packet>        queued_packets;

    void
    configure(qb::io::quic::settings const &config) override {
        ++configure_calls;
        last_settings = config;
    }

    void
    start_server(qb::io::endpoint const &local, std::vector<std::string> const &alpn_protocols, qb::io::quic::tls_config const &tls) override {
        ++start_server_calls;
        last_local               = local;
        last_alpn                = alpn_protocols;
        last_tls                 = tls;
        stats.active_connections = 0;
    }

    void
    start_client(qb::io::endpoint const &local, qb::io::endpoint const &remote, std::vector<std::string> const &alpn_protocols,
                 qb::io::quic::tls_config const &tls) override {
        ++start_client_calls;
        last_local               = local;
        last_alpn                = alpn_protocols;
        last_tls                 = tls;
        last_remote              = remote;
        stats.active_connections = 1;
    }

    void
    on_udp_datagram(qb::io::quic::packet_view) override {}
    void
    on_timeout(std::chrono::steady_clock::time_point) override {
        ++timeout_calls;
    }

    std::chrono::steady_clock::time_point
    next_timeout() const override {
        return std::chrono::steady_clock::time_point::max();
    }

    bool
    wants_write() const noexcept override {
        return !queued_packets.empty();
    }

    std::vector<qb::io::quic::packet>
    drain_packets() override {
        auto out = std::move(queued_packets);
        queued_packets.clear();
        return out;
    }

    std::vector<qb::io::quic::backend_event>
    drain_events() override {
        auto out = std::move(queued_events);
        queued_events.clear();
        return out;
    }

    std::uint64_t
    open_stream(qb::io::quic::stream_direction direction) override {
        return open_stream(0, direction);
    }

    std::uint64_t
    open_stream(std::uint64_t connection_id, qb::io::quic::stream_direction direction) override {
        last_open_connection_id = connection_id;
        if (direction == qb::io::quic::stream_direction::bidirectional)
            ++open_bidi_calls;
        else
            ++open_uni_calls;
        ++stats.active_streams;
        return direction == qb::io::quic::stream_direction::bidirectional ? 0 : 2;
    }

    void
    send_stream_data(std::uint64_t connection_id, std::uint64_t stream_id, std::span<const std::byte> data, bool fin) override {
        ++send_stream_data_calls;
        sent_connection_id = connection_id;
        sent_stream_id     = stream_id;
        sent_stream_bytes += data.size();
        sent_stream_fin = fin;
        if (close_on_send_stream_data) {
            queued_events.push_back({qb::io::quic::backend_event::kind::connection_closed, 0, 0, 77, "closed while sending stream data", {}});
        }
    }

    void
    extend_stream_credit(std::uint64_t, std::uint64_t stream_id, std::uint64_t bytes) override {
        ++extend_stream_credit_calls;
        extended_stream_id = stream_id;
        extended_bytes += bytes;
    }

    void
    send_datagram(std::uint64_t connection_id, std::span<const std::byte> data) override {
        ++send_datagram_calls;
        sent_datagram_connection_id = connection_id;
        sent_datagram_bytes += data.size();
    }

    void
    reset_stream(std::uint64_t, std::uint64_t stream_id, std::uint64_t application_error_code) override {
        ++reset_stream_calls;
        reset_stream_id   = stream_id;
        reset_stream_code = application_error_code;
        queued_events.push_back(
            {qb::io::quic::backend_event::kind::stream_closed,
             0,
             stream_id,
             application_error_code,
             "stream reset",
             {},
             qb::io::quic::disconnect_reason::none,
             qb::io::quic::stream_close_reason::reset});
    }

    void
    stop_stream(std::uint64_t, std::uint64_t stream_id, std::uint64_t application_error_code) override {
        ++stop_stream_calls;
        stop_stream_id   = stream_id;
        stop_stream_code = application_error_code;
        queued_events.push_back(
            {qb::io::quic::backend_event::kind::stream_closed,
             0,
             stream_id,
             application_error_code,
             "stream stopped",
             {},
             qb::io::quic::disconnect_reason::none,
             qb::io::quic::stream_close_reason::stop_sending});
    }

    void
    close(std::uint64_t application_error_code, std::string_view reason) override {
        ++close_calls;
        close_code = application_error_code;
        close_reason.assign(reason);
        stats.active_connections = 0;
        stats.active_streams     = 0;
    }

    void
    close_connection(std::uint64_t connection_id, std::uint64_t application_error_code, std::string_view reason) override {
        ++close_connection_calls;
        closed_connection_id = connection_id;
        close_code           = application_error_code;
        close_reason.assign(reason);
        stats.active_connections = 0;
        stats.active_streams     = 0;
    }

    qb::io::quic::stats
    current_stats() const noexcept override {
        return stats;
    }
};

// ---------------------------------------------------------------------------
// `connector<…, void>` probe: tallies every event callback and records the last
// close / stream metadata. The universal event-dispatch double.
//
// §7.4: ack-byte count and close error code are SEPARATE members
// (`last_acked_bytes` vs `last_close_error_code`) — never one multiplexed field.
// ---------------------------------------------------------------------------
class CallbackQuicClient : public qb::io::async::quic::connector<CallbackQuicClient> {
public:
    int                               connected      = 0;
    int                               closed         = 0;
    int                               stream_started = 0;
    int                               stream_data    = 0;
    int                               stream_acked   = 0;
    int                               stream_closed  = 0;
    int                               datagrams      = 0;
    std::string                       received;
    std::string                       datagram_received;
    qb::io::quic::stream_close_reason last_stream_close_reason = qb::io::quic::stream_close_reason::none;
    qb::io::quic::disconnect_reason   last_close_reason        = qb::io::quic::disconnect_reason::none;
    std::uint64_t                     last_acked_bytes         = 0; // written only by stream_data_acked
    std::uint64_t                     last_close_error_code    = 0; // written only by connection_closed
    std::string                       last_reason_phrase;
    qb::io::quic::stream_direction    last_stream_direction = qb::io::quic::stream_direction::bidirectional;
    qb::io::quic::stream_origin       last_stream_origin    = qb::io::quic::stream_origin::local;

    CallbackQuicClient() = default;

    explicit CallbackQuicClient(std::unique_ptr<qb::io::quic::backend> backend)
        : connector(std::move(backend)) {}

    void
    on(qb::io::async::quic::event::connected const &) {
        ++connected;
    }
    void
    on(qb::io::async::quic::event::connection_closed const &ev) {
        ++closed;
        last_close_reason     = ev.reason;
        last_close_error_code = ev.error_code;
        last_reason_phrase    = ev.reason_phrase;
    }
    void
    on(qb::io::async::quic::event::stream_started const &ev) {
        ++stream_started;
        last_stream_direction = ev.direction;
        last_stream_origin    = ev.origin;
    }
    void
    on(qb::io::async::quic::event::stream_data const &ev) {
        ++stream_data;
        received.append(ev.payload.data(), ev.payload.size());
    }
    void
    on(qb::io::async::quic::event::stream_data_acked const &ev) {
        ++stream_acked;
        last_acked_bytes = ev.bytes;
    }
    void
    on(qb::io::async::quic::event::stream_closed const &ev) {
        ++stream_closed;
        last_stream_close_reason = ev.reason;
    }
    void
    on(qb::io::async::quic::event::datagram const &ev) {
        ++datagrams;
        datagram_received.append(ev.payload.data(), ev.payload.size());
    }
};

// ---------------------------------------------------------------------------
// A do-nothing `use<>::quic::session` — the StreamSession placeholder for the
// connector/server harnesses that need a session type but no behaviour.
// ---------------------------------------------------------------------------
class DummyQuicStreamSession : public qb::io::use<DummyQuicStreamSession>::quic::session {
public:
    using Base = qb::io::use<DummyQuicStreamSession>::quic::session;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// `server<…, DummyQuicStreamSession>` probe: like CallbackQuicClient but for the
// server role — also counts spawned stream sessions and accumulates acked bytes.
// ---------------------------------------------------------------------------
class CallbackQuicServer : public qb::io::async::quic::server<CallbackQuicServer, DummyQuicStreamSession> {
public:
    int                             connected       = 0;
    int                             closed          = 0;
    int                             stream_started  = 0;
    int                             stream_sessions = 0;
    std::uint64_t                   acked_bytes     = 0;
    std::string                     received;
    std::string                     datagram_received;
    qb::io::quic::disconnect_reason last_close_reason     = qb::io::quic::disconnect_reason::none;
    qb::io::quic::stream_direction  last_stream_direction = qb::io::quic::stream_direction::bidirectional;
    qb::io::quic::stream_origin     last_stream_origin    = qb::io::quic::stream_origin::local;

    CallbackQuicServer() = default;

    explicit CallbackQuicServer(std::unique_ptr<qb::io::quic::backend> backend)
        : server(std::move(backend)) {}

    void
    on(qb::io::async::quic::event::connected const &) {
        ++connected;
    }
    void
    on(qb::io::async::quic::event::connection_closed const &ev) {
        ++closed;
        last_close_reason = ev.reason;
    }
    void
    on(qb::io::async::quic::event::stream_started const &ev) {
        ++stream_started;
        last_stream_direction = ev.direction;
        last_stream_origin    = ev.origin;
    }
    void
    on(qb::io::async::quic::event::stream_data const &ev) {
        received.append(ev.payload.data(), ev.payload.size());
    }
    void
    on(qb::io::async::quic::event::stream_data_acked const &ev) {
        acked_bytes += ev.bytes;
    }
    void
    on(qb::io::async::quic::event::datagram const &ev) {
        datagram_received.append(ev.payload.data(), ev.payload.size());
    }
    void
    on(DummyQuicStreamSession &) {
        ++stream_sessions;
    }
};

// ---------------------------------------------------------------------------
// `connector<…, DummyQuicStreamSession>` that inherits the facade verbatim —
// exercises the local-stream-session helpers (open/flush/finish) on the client.
// ---------------------------------------------------------------------------
class SessionQuicClient : public qb::io::async::quic::connector<SessionQuicClient, DummyQuicStreamSession> {
public:
    using connector::connector;
};

// ---------------------------------------------------------------------------
// 4-byte framed echo session + protocol + server. The protocol slices fixed
// 4-byte messages off the inbound pipe, records each, and publishes "ack!" back.
// Together they prove the remote-data → session → response → flush round-trip.
// ---------------------------------------------------------------------------
class EchoQuicStreamSession : public qb::io::use<EchoQuicStreamSession>::quic::session {
public:
    using Base = qb::io::use<EchoQuicStreamSession>::quic::session;
    using Base::Base;

    int         messages = 0;
    std::string received;
};

class EchoQuicProtocol : public qb::io::async::AProtocol<EchoQuicStreamSession> {
public:
    explicit EchoQuicProtocol(EchoQuicStreamSession &session) noexcept
        : AProtocol(session) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4 ? 4 : 0;
    }

    void
    onMessage(std::size_t size) noexcept final {
        _io.received.append(_io.in().begin(), size);
        ++_io.messages;
        _io.publish("ack!", std::size_t{4});
    }

    void
    reset() noexcept final {}
};

class EchoQuicServer : public qb::io::async::quic::server<EchoQuicServer, EchoQuicStreamSession> {
public:
    using server::server;

    int sessions           = 0;
    int stream_data_events = 0;

    void
    on(EchoQuicStreamSession &session) {
        ++sessions;
        session.switch_protocol<EchoQuicProtocol>(session);
    }
    void
    on(qb::io::async::quic::event::stream_data const &) {
        ++stream_data_events;
    }
};

#ifdef QB_HAS_QUIC
// ---------------------------------------------------------------------------
// Native-only helpers for the live `system/quic/quic-handshake` tests: a small
// fixed payload and a pump that delivers one backend's queued packets to another
// (used to drive two real libngtcp2 backends against each other in-process).
// ---------------------------------------------------------------------------
inline std::array<std::byte, 4>
quic_payload() {
    return {std::byte{'q'}, std::byte{'u'}, std::byte{'i'}, std::byte{'c'}};
}

inline void
deliver_quic_packets(qb::io::quic::backend &from, qb::io::quic::backend &to) {
    for (auto &packet : from.drain_packets()) {
        qb::io::quic::packet_view view{packet.remote, packet.local, std::span<const std::byte>{packet.payload.data(), packet.payload.size()}};
        to.on_udp_datagram(view);
    }
}
#endif // QB_HAS_QUIC

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_QUIC_TEST_DOUBLES_H
