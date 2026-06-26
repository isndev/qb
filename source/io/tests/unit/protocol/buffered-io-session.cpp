/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/protocol/buffered-io-session.cpp
 * @brief `qb::io::async::buffered_io<>` — the protocol/buffer state machine that frames every session.
 *
 * `buffered_io<_Derived>` (qb/io/async/buffered_io.h) is the CRTP base every qb-io session inherits:
 * it owns the input/output `pipe`s, holds the active `AProtocol`, and runs `process_input()` — the
 * loop that asks the protocol for the next frame, delivers it via `onMessage`, flushes consumed
 * input, and dispatches the lifecycle events (`pending_read`, `eof`/`input_drained`, `disconnected`,
 * `dispose`). This test drives that machine directly with an in-memory probe session — NO socket, NO
 * event loop, NO TLS, NO QUIC backend — so it is a strict `unit` test of the framing contract.
 *
 * (The original system/test-buffered-io.cpp header framed this as a "QUIC stream" stand-in. That is
 * misleading: nothing here is QUIC-specific — `buffered_io` is the framing base for *every* logical
 * transport. The probe is just a session whose `in()`/`out()` are plain pipes; the spec calls for
 * dropping that false connotation, which this file does.)
 *
 * Split from system/test-buffered-io.cpp (spec §2): the session lifecycle/dispatch/overflow cases
 * live here; the framing-primitive cases (byte/bytes-terminated, size_as_header) move to
 * protocol-base-framing.cpp. Strengthened: the byte counters (`bytes_read`/`bytes_written`/
 * `messages_processed`) are asserted explicitly after a frame drain, not just at zero.
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async/buffered_io.h>
#include <qb/io/protocol/base.h>

namespace {

// ---------------------------------------------------------------------------
// In-memory probe session: in()/out() are plain pipes; lifecycle callbacks are
// counted so process_input()'s dispatch can be observed deterministically.
// ---------------------------------------------------------------------------
class BufferedProbe : public qb::io::async::buffered_io<BufferedProbe> {
    qb::allocator::pipe<char> _in;
    qb::allocator::pipe<char> _out;
    std::size_t               _max_write_buffer_size = QB_MAX_WRITE_BUFFER_SIZE;

public:
    std::vector<std::string> messages;
    std::size_t              pending_read_events      = 0u;
    std::size_t              eof_events               = 0u;
    std::size_t              disconnected_events      = 0u;
    std::size_t              dispose_events           = 0u;
    std::size_t              last_pending_read        = 0u;
    int                      last_disconnect_reason   = 0;
    bool                     reentrant_process_result = false;

    using base_io_t = qb::io::async::buffered_io<BufferedProbe>;

    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }
    qb::allocator::pipe<char> const &
    in() const noexcept {
        return _in;
    }
    qb::allocator::pipe<char> &
    out() noexcept {
        return _out;
    }
    qb::allocator::pipe<char> const &
    out() const noexcept {
        return _out;
    }

    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _in.size();
    }
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out.size();
    }
    [[nodiscard]] std::size_t
    max_write_buffer_size() const noexcept {
        return _max_write_buffer_size;
    }

    void
    set_max_write_buffer_size(std::size_t size) noexcept {
        _max_write_buffer_size = size;
    }

    void
    append(std::string_view data) {
        _in << data;
        account_read(data.size());
    }

    void
    flush(std::size_t size) noexcept {
        _in.free_front(size);
    }

    void
    reset_state() noexcept {
        reset_buffered_io_state();
    }

    void
    on(qb::io::async::event::pending_read &&event) noexcept {
        ++pending_read_events;
        last_pending_read = event.bytes;
    }
    void
    on(qb::io::async::event::eof &&) noexcept {
        ++eof_events;
    }
    void
    on(qb::io::async::event::disconnected &&event) noexcept {
        ++disconnected_events;
        last_disconnect_reason = event.reason;
    }
    void
    on(qb::io::async::event::dispose &&) noexcept {
        ++dispose_events;
    }
};

// Fixed-width framing: every `frame_size` bytes is one message; optionally replies + closes.
class FixedFrameProtocol : public qb::io::async::AProtocol<BufferedProbe> {
    std::size_t _frame_size;
    bool        _close_after_message;

public:
    FixedFrameProtocol(BufferedProbe &io, std::size_t frame_size, bool close_after_message = false) noexcept
        : AProtocol(io)
        , _frame_size(frame_size)
        , _close_after_message(close_after_message) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= _frame_size ? _frame_size : 0u;
    }
    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        if (_close_after_message) {
            _io.publish(std::string_view{"reply"});
            _io.close_after_deliver();
        }
    }
    void
    reset() noexcept final {}
};

// Always reports an oversized frame — used to force the message-too-large path.
class OversizedProtocol : public qb::io::async::AProtocol<BufferedProbe> {
    std::size_t _reported_size;

public:
    OversizedProtocol(BufferedProbe &io, std::size_t reported_size) noexcept
        : AProtocol(io)
        , _reported_size(reported_size) {}

    std::size_t
    getMessageSize() noexcept final {
        return _reported_size;
    }
    void
    onMessage(std::size_t) noexcept final {
        ADD_FAILURE() << "an oversized frame must never be delivered";
    }
    void
    reset() noexcept final {}
};

// Marks itself not-ok at construction — the session must reject it on switch_protocol.
class RejectingProtocol : public qb::io::async::AProtocol<BufferedProbe> {
public:
    explicit RejectingProtocol(BufferedProbe &io) noexcept
        : AProtocol(io) {
        not_ok();
    }
    std::size_t
    getMessageSize() noexcept final {
        return 0u;
    }
    void
    onMessage(std::size_t) noexcept final {}
    void
    reset() noexcept final {}
};

// Disconnects (with a reply queued) from inside onMessage.
class DisconnectingProtocol : public qb::io::async::AProtocol<BufferedProbe> {
public:
    explicit DisconnectingProtocol(BufferedProbe &io) noexcept
        : AProtocol(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }
    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        _io.publish(std::string_view{"pending"});
        _io.disconnect(77);
    }
    void
    reset() noexcept final {}
};

// Re-enters process_input() from inside onMessage to prove the reentrancy guard.
class ReentrantProtocol : public qb::io::async::AProtocol<BufferedProbe> {
public:
    explicit ReentrantProtocol(BufferedProbe &io) noexcept
        : AProtocol(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 2u ? 2u : 0u;
    }
    void
    onMessage(std::size_t size) noexcept final {
        _io.reentrant_process_result = _io.process_input();
        _io.messages.emplace_back(_io.in().begin(), size);
    }
    void
    reset() noexcept final {}
};

// Clears the protocol from inside onMessage.
class ClearProtocolOnMessage : public qb::io::async::AProtocol<BufferedProbe> {
public:
    explicit ClearProtocolOnMessage(BufferedProbe &io) noexcept
        : AProtocol(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }
    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        _io.clear_protocols();
    }
    void
    reset() noexcept final {}
};

// Invalidates itself (not_ok) after delivering, optionally with a queued reply.
class InvalidatingProtocol : public qb::io::async::AProtocol<BufferedProbe> {
    bool _publish_reply;

public:
    explicit InvalidatingProtocol(BufferedProbe &io, bool publish_reply = false) noexcept
        : AProtocol(io)
        , _publish_reply(publish_reply) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }
    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        if (_publish_reply)
            _io.publish(std::string_view{"reply"});
        not_ok();
    }
    void
    reset() noexcept final {}
};

// Opts out of auto-flush so the caller must drain input manually.
class NonFlushingProtocol : public qb::io::async::AProtocol<BufferedProbe> {
    bool _disconnect_after_message;
    bool _message_seen = false;

public:
    explicit NonFlushingProtocol(BufferedProbe &io, bool disconnect_after_message = false) noexcept
        : AProtocol(io)
        , _disconnect_after_message(disconnect_after_message) {
        set_should_flush(false);
    }

    std::size_t
    getMessageSize() noexcept final {
        if (_message_seen)
            return 0u;
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }
    void
    onMessage(std::size_t size) noexcept final {
        _message_seen = true;
        _io.messages.emplace_back(_io.in().begin(), size);
        if (_disconnect_after_message)
            _io.disconnect(91);
    }
    void
    reset() noexcept final {}
};

// ---------------------------------------------------------------------------
// Server-owned probe: routes dispose() through a server's disconnected() hook
// with connection/stream identity, as a pooled session would.
// ---------------------------------------------------------------------------
struct ServerDisposeRecorder {
    std::size_t   disconnected_calls = 0u;
    std::uint64_t last_connection    = 0u;
    std::uint64_t last_stream        = 0u;

    void
    disconnected(std::uint64_t connection_id, std::uint64_t stream_id) noexcept {
        ++disconnected_calls;
        last_connection = connection_id;
        last_stream     = stream_id;
    }
};

class ServerOwnedBufferedProbe : public qb::io::async::buffered_io<ServerOwnedBufferedProbe> {
    qb::allocator::pipe<char> _in;
    qb::allocator::pipe<char> _out;
    ServerDisposeRecorder    &_server;

public:
    static constexpr bool has_server = true;
    using base_io_t                  = qb::io::async::buffered_io<ServerOwnedBufferedProbe>;

    explicit ServerOwnedBufferedProbe(ServerDisposeRecorder &server) noexcept
        : _server(server) {}

    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }
    qb::allocator::pipe<char> &
    out() noexcept {
        return _out;
    }
    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _in.size();
    }
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out.size();
    }
    [[nodiscard]] std::size_t
    max_write_buffer_size() const noexcept {
        return QB_MAX_WRITE_BUFFER_SIZE;
    }
    void
    flush(std::size_t size) noexcept {
        _in.free_front(size);
    }
    [[nodiscard]] std::uint64_t
    id() const noexcept {
        return 42u;
    }
    [[nodiscard]] std::uint64_t
    connection_id() const noexcept {
        return 7u;
    }
    ServerDisposeRecorder &
    server() noexcept {
        return _server;
    }
};

} // namespace

// =============================================================================
// PROTOCOL SWITCH / CLEAR
// =============================================================================

TEST(BufferedIoSession, RejectsInvalidProtocolAndClearsStoredProtocol) {
    BufferedProbe session;
    const auto   &const_session = session;

    EXPECT_EQ(session.switch_protocol<RejectingProtocol>(session), nullptr) << "a not_ok protocol is rejected";
    EXPECT_EQ(session.protocol(), nullptr);
    EXPECT_EQ(const_session.protocol(), nullptr);

    auto *protocol = session.switch_protocol<FixedFrameProtocol>(session, 4u);
    ASSERT_NE(protocol, nullptr);
    EXPECT_EQ(session.protocol(), protocol);

    session.clear_protocols();
    EXPECT_EQ(session.protocol(), nullptr);
    EXPECT_FALSE(session.has_pending_read());
}

// =============================================================================
// ACCESSORS / COUNTERS / TYPED DISCONNECT
// =============================================================================

/**
 * @test The session's accessors and lifecycle counters track state across a typed disconnect.
 * @brief Folded from BufferedIO.AccessorsCountersAndTypedDisconnectTrackLifecycleState. Asserts the
 *        initial-zero counters, the `account_written` path, close-after-deliver + publish flush, the
 *        output-queue accessor, the typed `disconnect_reason`, and the dispose dispatch counts.
 */
TEST(BufferedIoSession, AccessorsCountersAndTypedDisconnectTrackLifecycle) {
    BufferedProbe session;
    const auto   &const_session = session;

    EXPECT_TRUE(session.is_connected());
    EXPECT_FALSE(session.has_pending_read());
    EXPECT_FALSE(session.has_pending_write());
    EXPECT_EQ(session.max_message_size(), QB_MAX_MESSAGE_SIZE);
    EXPECT_EQ(session.disconnection_reason(), 0);
    EXPECT_EQ(session.system_error(), 0);
    EXPECT_EQ(session.bytes_read(), 0u);
    EXPECT_EQ(session.bytes_written(), 0u);
    EXPECT_EQ(session.messages_processed(), 0u);
    EXPECT_EQ(const_session.protocol(), nullptr);

    session.set_max_message_size(16u);
    EXPECT_EQ(session.max_message_size(), 16u);

    session.account_written(7u);
    EXPECT_EQ(session.bytes_written(), 7u);

    session.close_after_deliver();
    session.publish();
    EXPECT_EQ(session.pendingWrite(), 0u);

    session << std::string_view{"queued"};
    EXPECT_TRUE(session.has_pending_write());
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "queued");

    session.disconnect(qb::io::async::event::disconnect_reason::user_initiated);
    EXPECT_FALSE(session.is_connected());
    EXPECT_EQ(session.disconnection_reason(),
              static_cast<int>(qb::io::async::event::disconnect_reason::user_initiated));

    session.dispose();
    EXPECT_EQ(session.disconnected_events, 1u);
    EXPECT_EQ(session.dispose_events, 1u);
}

// =============================================================================
// process_input — guard conditions before parsing
// =============================================================================

TEST(BufferedIoSession, ProcessInputStopsWhenProtocolWasClosedBeforeParsing) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<FixedFrameProtocol>(session, 4u), nullptr);
    session.close_after_deliver();
    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.pendingRead(), 4u) << "a closed protocol must not consume input";
    EXPECT_TRUE(session.messages.empty());
}

TEST(BufferedIoSession, NoProtocolReportsPendingReadThenStaysQuietWhenEmpty) {
    BufferedProbe session;

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.pending_read_events, 0u) << "no input ⇒ no pending_read event";

    session.append("raw");
    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.pending_read_events, 1u);
    EXPECT_EQ(session.last_pending_read, 3u);

    session.flush(session.pendingRead());
    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.eof_events, 0u) << "the no-protocol path never fires eof";
}

TEST(BufferedIoSession, InvalidProtocolStopsBeforeParsingWithoutFlushingInput) {
    BufferedProbe session;
    auto         *protocol = session.switch_protocol<FixedFrameProtocol>(session, 4u);
    ASSERT_NE(protocol, nullptr);
    protocol->not_ok();
    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.pendingRead(), 4u);
    EXPECT_TRUE(session.messages.empty());
}

// =============================================================================
// process_input — frame delivery, flush, counters, eof
// =============================================================================

/**
 * @test A multi-frame buffer is parsed frame-by-frame, input is flushed, and the byte/message
 *       counters reflect the drain; a follow-up empty parse fires `eof`.
 * @brief Folded from BufferedIO.ProcessesFramesFlushesInputAndReportsPendingReadThenEof. The
 *        counter assertions (`bytes_read`/`messages_processed`) are the spec's "written-byte
 *        counters after drain" strengthening: they pin that accounting is driven by the drain, not
 *        left at zero.
 */
TEST(BufferedIoSession, ProcessesFramesFlushesInputAndCountsThenEof) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<FixedFrameProtocol>(session, 3u), nullptr);

    session.append("abcdefghiZ"); // 3 full 3-byte frames + 1 leftover byte

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"abc", "def", "ghi"}));
    EXPECT_EQ(session.pendingRead(), 1u) << "the trailing partial byte remains buffered";
    EXPECT_EQ(session.pending_read_events, 1u);
    EXPECT_EQ(session.last_pending_read, 1u);
    EXPECT_EQ(session.bytes_read(), 10u) << "all appended bytes are accounted as read";
    EXPECT_EQ(session.messages_processed(), 3u) << "exactly three frames were delivered";

    session.flush(session.pendingRead());
    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.eof_events, 1u) << "an empty buffer after a prior read fires eof once";
}

/**
 * @test close_after_deliver keeps the queued reply available for the drain.
 */
TEST(BufferedIoSession, CloseAfterDeliverKeepsOutputForDrain) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<FixedFrameProtocol>(session, 4u, true), nullptr);
    session.append("data");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 5u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "reply");
    EXPECT_FALSE(session.protocol()->ok());
    EXPECT_EQ(session.disconnection_reason(), 0) << "close_after_deliver does not set a disconnect reason";
}

// =============================================================================
// process_input — disconnect / clear / invalidate inside onMessage
// =============================================================================

TEST(BufferedIoSession, DisconnectDuringMessageFlushesInputAndKeepsPendingOutput) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<DisconnectingProtocol>(session), nullptr);
    session.append("data");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 7u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "pending");
    EXPECT_EQ(session.disconnection_reason(), 77);
}

TEST(BufferedIoSession, ReentrantProcessInputReturnsImmediatelyWhileMessageActive) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<ReentrantProtocol>(session), nullptr);
    session.append("xy");

    EXPECT_TRUE(session.process_input());
    EXPECT_TRUE(session.reentrant_process_result) << "the nested process_input() returns true (re-entry guarded)";
    EXPECT_EQ(session.messages, (std::vector<std::string>{"xy"}));
    EXPECT_EQ(session.pendingRead(), 0u);
}

TEST(BufferedIoSession, ClearingProtocolDuringMessageDisconnectsAfterFlush) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<ClearProtocolOnMessage>(session), nullptr);
    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.disconnection_reason(), -1) << "clearing the protocol mid-message disconnects (protocol_error)";
}

TEST(BufferedIoSession, InvalidProtocolAfterMessageKeepsPendingOutputForDrain) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<InvalidatingProtocol>(session, true), nullptr);
    session.append("data");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 5u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "reply");
    EXPECT_EQ(session.disconnection_reason(), 0) << "a queued reply defers the disconnect until drain";
}

TEST(BufferedIoSession, InvalidProtocolAfterMessageWithoutOutputDisconnects) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<InvalidatingProtocol>(session, false), nullptr);
    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.disconnection_reason(), -1);
}

// =============================================================================
// process_input — non-flushing protocol leaves input for manual drain
// =============================================================================

TEST(BufferedIoSession, NonFlushingProtocolPreservesInputUntilCallerFlushes) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<NonFlushingProtocol>(session), nullptr);
    session.append("data");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 4u) << "set_should_flush(false) keeps the consumed bytes";

    session.flush(session.pendingRead());
    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.eof_events, 1u);
}

TEST(BufferedIoSession, NonFlushingDisconnectLeavesInputForManualDrain) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<NonFlushingProtocol>(session, true), nullptr);
    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 4u);
    EXPECT_EQ(session.disconnection_reason(), 91);
}

// =============================================================================
// process_input — oversized / incoherent frames
// =============================================================================

TEST(BufferedIoSession, OversizedOrIncoherentFramesDisconnectProcessing) {
    {
        BufferedProbe session;
        ASSERT_NE(session.switch_protocol<OversizedProtocol>(session, 8u), nullptr);
        session.set_max_message_size(4u);
        session.append("12345678");

        EXPECT_FALSE(session.process_input());
        EXPECT_EQ(session.disconnection_reason(), -2) << "message-too-large disconnect";
        EXPECT_FALSE(session.protocol()->ok());
    }
    {
        BufferedProbe session;
        ASSERT_NE(session.switch_protocol<OversizedProtocol>(session, 8u), nullptr);
        session.append("1"); // claims size 8 but only 1 byte is present

        EXPECT_FALSE(session.process_input());
        EXPECT_EQ(session.disconnection_reason(), -2);
        EXPECT_FALSE(session.protocol()->ok());
    }
}

// =============================================================================
// publish — write-buffer overflow rollback
// =============================================================================

TEST(BufferedIoSession, PublishOverflowRollsBackPartialAppendAndDisconnects) {
    BufferedProbe session;
    session.set_max_write_buffer_size(4u);

    session.publish(std::string_view{"ab"});
    EXPECT_EQ(session.pendingWrite(), 2u);

    session.publish(std::string_view{"cdef"}); // would exceed the 4-byte cap
    EXPECT_EQ(session.pendingWrite(), 4u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd");
    EXPECT_EQ(session.disconnection_reason(),
              static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));

    session.publish(std::string_view{"ignored"}); // post-overflow publishes are dropped
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd");
}

TEST(BufferedIoSession, PublishAtExistingWriteCapDisconnectsWithoutAppending) {
    BufferedProbe session;
    session.set_max_write_buffer_size(2u);

    session.publish(std::string_view{"ab"});
    ASSERT_EQ(session.pendingWrite(), 2u);

    session.publish(std::string_view{"c"}); // already at cap
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "ab");
    EXPECT_EQ(session.disconnection_reason(),
              static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));
}

// =============================================================================
// dispose / reset lifecycle
// =============================================================================

TEST(BufferedIoSession, DisposeIsIdempotentAndResetRestoresConnectionState) {
    BufferedProbe session;
    session.disconnect(0);

    EXPECT_FALSE(session.is_connected());
    session.dispose();
    session.dispose(); // idempotent

    EXPECT_EQ(session.disconnected_events, 1u);
    EXPECT_EQ(session.dispose_events, 1u);
    EXPECT_EQ(session.last_disconnect_reason,
              static_cast<int>(qb::io::async::event::disconnect_reason::user_initiated));
    EXPECT_FALSE(session.process_input());

    session.reset_state();
    EXPECT_TRUE(session.is_connected()) << "reset_buffered_io_state() re-arms the session";
}

TEST(BufferedIoSession, ServerOwnedDisposeReportsConnectionAndStreamIdentity) {
    ServerDisposeRecorder    recorder;
    ServerOwnedBufferedProbe session{recorder};

    session.dispose();
    session.dispose(); // idempotent

    EXPECT_EQ(recorder.disconnected_calls, 1u);
    EXPECT_EQ(recorder.last_connection, 7u);
    EXPECT_EQ(recorder.last_stream, 42u);
}
