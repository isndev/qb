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
#include <functional>
#include <memory>
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
    EXPECT_EQ(session.protocol(), qb::io::async::no_protocol()) << "no protocol set ⇒ the NoProtocol sentinel (never null)";
    EXPECT_EQ(const_session.protocol(), qb::io::async::no_protocol());

    auto *protocol = session.switch_protocol<FixedFrameProtocol>(session, 4u);
    ASSERT_NE(protocol, nullptr);
    EXPECT_EQ(session.protocol(), protocol);

    session.clear_protocols();
    EXPECT_EQ(session.protocol(), qb::io::async::no_protocol()) << "clear_protocols() restores the sentinel";
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
    EXPECT_EQ(const_session.protocol(), qb::io::async::no_protocol()); // default session ⇒ sentinel

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
    EXPECT_EQ(session.disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::user_initiated));

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
    EXPECT_EQ(session.disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));

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
    EXPECT_EQ(session.disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));
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
    EXPECT_EQ(session.last_disconnect_reason, static_cast<int>(qb::io::async::event::disconnect_reason::user_initiated));
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

// =============================================================================
// CLEAR FROM INSIDE A DISPATCH (Huly QB-1278)
// =============================================================================
// A handler that called clear_protocols() ended the protocol whose onMessage() was still running; a protocol
// that writes its own state after its handler -- as the WebSocket decoder does -- then wrote freed memory.

namespace {

struct BufferedProtocolLifetime {
    bool        in_dispatch           = false;
    std::size_t destroyed             = 0u;
    std::size_t destroyed_in_dispatch = 0u;
    std::size_t frames_after_handler  = 0u;
};

class DispatchingBufferedProtocol : public qb::io::async::AProtocol<BufferedProbe> {
    BufferedProtocolLifetime &_life;
    void (*_handler)(BufferedProbe &);
    std::size_t _frames = 0u;

public:
    DispatchingBufferedProtocol(BufferedProbe &io, BufferedProtocolLifetime &life, void (*handler)(BufferedProbe &)) noexcept
        : AProtocol(io)
        , _life(life)
        , _handler(handler) {}

    ~DispatchingBufferedProtocol() override {
        ++_life.destroyed;
        if (_life.in_dispatch)
            ++_life.destroyed_in_dispatch;
    }

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        auto &life       = _life;
        life.in_dispatch = true;
        _io.messages.emplace_back(_io.in().begin(), size);
        _handler(_io);
        ++_frames; // the protocol's own state, after its handler: freed memory if the handler ended it
        life.frames_after_handler = _frames;
        life.in_dispatch          = false;
    }

    void
    reset() noexcept final {
        _frames = 0u;
    }
};

void
buffered_clear_in_handler(BufferedProbe &io) {
    io.clear_protocols();
}

void
buffered_clear_then_switch_in_handler(BufferedProbe &io) {
    io.clear_protocols();
    (void) io.switch_protocol<FixedFrameProtocol>(io, 4u);
}

void
buffered_clear_then_switch_wide_in_handler(BufferedProbe &io) {
    io.clear_protocols();
    (void) io.switch_protocol<FixedFrameProtocol>(io, 8u);
}

void
buffered_disconnect_then_clear_in_handler(BufferedProbe &io) {
    io.disconnect(5);
    io.clear_protocols();
}

void
buffered_publish_then_clear_in_handler(BufferedProbe &io) {
    io.publish(std::string_view{"bye"});
    io.clear_protocols();
}

} // namespace

TEST(BufferedIoSession, ClearProtocolsFromAHandlerEndsTheProtocolOnceItsDispatchReturned) {
    BufferedProtocolLifetime life;
    {
        BufferedProbe session;
        ASSERT_NE(session.switch_protocol<DispatchingBufferedProtocol>(session, life, &buffered_clear_in_handler), nullptr);
        session.append("abcd");

        EXPECT_FALSE(session.process_input());
        EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd"}));
        EXPECT_EQ(life.frames_after_handler, 1u) << "the protocol finished its onMessage() after the handler cleared it";
        EXPECT_EQ(life.destroyed, 1u) << "the cleared protocol is destroyed once the dispatch has returned";
        EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
        EXPECT_EQ(session.disconnection_reason(), -1);
    }
    EXPECT_EQ(life.destroyed, 1u) << "destroyed once";
}

TEST(BufferedIoSession, ClearThenSwitchFromAHandlerKeepsTheNewProtocolAndEndsTheOld) {
    BufferedProtocolLifetime life;
    BufferedProbe            session;
    ASSERT_NE(session.switch_protocol<DispatchingBufferedProtocol>(session, life, &buffered_clear_then_switch_in_handler), nullptr);
    session.append("abcdefgh");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd", "efgh"}));
    EXPECT_EQ(life.destroyed, 1u) << "the cleared protocol is destroyed once the dispatch has returned";
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_NE(session.protocol(), qb::io::async::no_protocol()) << "the protocol switched in after the clear is kept";
}

// The other exits of process_input() end what the dispatch retired too: a disconnect() from the handler, a clear
// with output still to deliver, and a frame of the switched-in protocol over the size limit.
TEST(BufferedIoSession, DisconnectThenClearFromAHandlerEndsTheProtocolOnceItsDispatchReturned) {
    BufferedProtocolLifetime life;
    BufferedProbe            session;
    ASSERT_NE(session.switch_protocol<DispatchingBufferedProtocol>(session, life, &buffered_disconnect_then_clear_in_handler), nullptr);
    session.append("abcd");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(life.frames_after_handler, 1u);
    EXPECT_EQ(life.destroyed, 1u) << "the disconnect exit ends what the dispatch retired";
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_EQ(session.disconnection_reason(), 5);
}

TEST(BufferedIoSession, ClearWithOutputPendingFromAHandlerEndsTheProtocolOnceItsDispatchReturned) {
    BufferedProtocolLifetime life;
    BufferedProbe            session;
    ASSERT_NE(session.switch_protocol<DispatchingBufferedProtocol>(session, life, &buffered_publish_then_clear_in_handler), nullptr);
    session.append("abcd");

    EXPECT_TRUE(session.process_input()) << "output still to deliver: the session drains it first";
    EXPECT_EQ(life.destroyed, 1u) << "the drain exit ends what the dispatch retired";
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "bye");
}

TEST(BufferedIoSession, AnOversizedFrameAfterAClearThenSwitchEndsTheOldProtocol) {
    BufferedProtocolLifetime life;
    BufferedProbe            session;
    ASSERT_NE(session.switch_protocol<DispatchingBufferedProtocol>(session, life, &buffered_clear_then_switch_wide_in_handler), nullptr);
    session.set_max_message_size(4u);
    session.append("abcdefghijkl"); // a 4-byte frame for the old protocol, an 8-byte one for the new: over the limit

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd"}));
    EXPECT_EQ(life.destroyed, 1u) << "the size-guard exit ends what the dispatch retired";
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_EQ(session.disconnection_reason(), -2);
}

TEST(BufferedIoSession, ClearProtocolsOutsideADispatchEndsTheProtocolsAtOnce) {
    BufferedProtocolLifetime life;
    BufferedProbe            session;
    ASSERT_NE(session.switch_protocol<DispatchingBufferedProtocol>(session, life, &buffered_clear_in_handler), nullptr);
    session.clear_protocols();
    EXPECT_EQ(life.destroyed, 1u) << "outside a dispatch, clear_protocols() destroys what it ends before it returns";
}

// =============================================================================
// THE PROTOCOL LIST (Huly QB-1278)
// =============================================================================
// What a dispatch retired ends when it returns, or with the list; a retired protocol's destructor may call back
// into its component -- switch_protocol(), clear_protocols() -- while the list ends it.

namespace {

struct EndingRecord {
    std::size_t destroyed           = 0u;
    bool        flag_when_destroyed = false; ///< the component's dispatch flag, read by the destructor
};

/// Whether an EndingProtocol's hook may run: a test disarms them before its list -- what they call into -- dies.
bool g_ending_hooks_armed = true;

struct ArmEndingHooks {
    ArmEndingHooks() noexcept {
        g_ending_hooks_armed = true;
    }
    ~ArmEndingHooks() {
        g_ending_hooks_armed = false;
    }
};

/// Frames nothing; its destructor counts itself, reads the dispatch flag it ends under, then runs a hook.
class EndingProtocol final : public qb::io::async::IProtocol {
    EndingRecord         &_record;
    bool const           *_dispatch_flag;
    std::function<void()> _on_destroy;

public:
    explicit EndingProtocol(EndingRecord &record, bool const *dispatch_flag = nullptr, std::function<void()> on_destroy = {})
        : _record(record)
        , _dispatch_flag(dispatch_flag)
        , _on_destroy(std::move(on_destroy)) {}

    ~EndingProtocol() override {
        ++_record.destroyed;
        if (_dispatch_flag)
            _record.flag_when_destroyed = *_dispatch_flag;
        if (_on_destroy && g_ending_hooks_armed)
            _on_destroy(); // never into a list being destroyed: a failed case must fail, not crash
    }

    std::size_t
    getMessageSize() noexcept override {
        return 0u;
    }

    void
    onMessage(std::size_t) noexcept override {}

    void
    reset() noexcept override {}
};

using protocol_list = qb::io::async::detail::protocol_list;

} // namespace

TEST(ProtocolList, ARetiredProtocolEndsWithTheListWhenNoDispatchEndsIt) {
    EndingRecord record;
    {
        protocol_list list;
        list.push_back(std::make_unique<EndingProtocol>(record));
        list.clear(true);
        EXPECT_EQ(record.destroyed, 0u) << "retired, not destroyed, while the dispatch runs";
    }
    EXPECT_EQ(record.destroyed, 1u) << "the component's destructor ends what a dispatch retired";
}

TEST(ProtocolList, RetiredProtocolsEndWithTheDispatchFlagSetThenTheFlagIsRestored) {
    EndingRecord  record;
    bool          in_dispatch = false; // the read loop has returned: its flag is clear
    protocol_list list;
    list.push_back(std::make_unique<EndingProtocol>(record, &in_dispatch));
    list.clear(true);

    EXPECT_FALSE(list.end_dispatch(false, in_dispatch)) << "the dispatch's verdict passes through";
    EXPECT_EQ(record.destroyed, 1u);
    EXPECT_TRUE(record.flag_when_destroyed) << "the destructor ends under the dispatch flag, as when the clear destroyed at once";
    EXPECT_FALSE(in_dispatch) << "the flag is the read loop's again";
    EXPECT_TRUE(list.end_dispatch(true, in_dispatch));
    EXPECT_EQ(record.destroyed, 1u) << "nothing retired, nothing ends";
}

TEST(ProtocolList, ADestructorRunByAClearOutsideADispatchMaySwitchOrClear) {
    // A clear outside a dispatch ends its protocols one at a time too: a destructor's push_back is kept, a
    // destructor's clear joins the clear under way (it used to run inside vector::clear()).
    EndingRecord   switching, next, clearing, live;
    bool           in_dispatch = false;
    protocol_list  list;
    ArmEndingHooks armed; // disarms the hooks before `list` dies
    list.push_back(std::make_unique<EndingProtocol>(switching, &in_dispatch, [&] { list.push_back(std::make_unique<EndingProtocol>(next)); }));
    list.clear(false);
    EXPECT_EQ(switching.destroyed, 1u);
    EXPECT_FALSE(switching.flag_when_destroyed) << "outside a dispatch: no dispatch flag";
    EXPECT_EQ(next.destroyed, 0u) << "switched in by the destructor: kept";

    list.push_back(std::make_unique<EndingProtocol>(clearing, nullptr, [&] { list.clear(in_dispatch); }));
    list.push_back(std::make_unique<EndingProtocol>(live));
    list.clear(false);
    EXPECT_EQ(next.destroyed, 1u);
    EXPECT_EQ(clearing.destroyed, 1u);
    EXPECT_EQ(live.destroyed, 1u) << "the destructor's clear joined the one under way";
    EXPECT_TRUE(list.end_dispatch(true, in_dispatch));
}

TEST(ProtocolList, AClearOutsideADispatchEndsEveryProtocolAtOnce) {
    EndingRecord  first, second;
    protocol_list list;
    list.push_back(std::make_unique<EndingProtocol>(first));
    list.push_back(std::make_unique<EndingProtocol>(second));
    list.clear(false);
    EXPECT_EQ(first.destroyed, 1u);
    EXPECT_EQ(second.destroyed, 1u);
}

TEST(ProtocolList, AClearThatFindsRetiredProtocolsRetiresToo) {
    // Retired entries mean a dispatch is on the stack, even when the dispatch flag was lost under it (a
    // reset_io_state() from a handler): the running protocol must not be destroyed by a second clear.
    EndingRecord  running, switched_in;
    bool          in_dispatch = false;
    protocol_list list;
    list.push_back(std::make_unique<EndingProtocol>(running));
    list.clear(true);
    list.push_back(std::make_unique<EndingProtocol>(switched_in));
    list.clear(false);
    EXPECT_EQ(running.destroyed, 0u);
    EXPECT_EQ(switched_in.destroyed, 0u);
    (void) list.end_dispatch(true, in_dispatch);
    EXPECT_EQ(running.destroyed, 1u);
    EXPECT_EQ(switched_in.destroyed, 1u);
}

TEST(ProtocolList, ARetiredProtocolsDestructorMaySwitchTheNextOneIn) {
    EndingRecord   retired, live, next;
    bool           in_dispatch = false;
    protocol_list  list;
    ArmEndingHooks armed; // disarms the hooks before `list` dies
    list.push_back(std::make_unique<EndingProtocol>(retired, &in_dispatch, [&] { list.push_back(std::make_unique<EndingProtocol>(next)); }));
    list.clear(true);
    list.push_back(std::make_unique<EndingProtocol>(live)); // switched in after the clear, by the handler
    (void) list.end_dispatch(true, in_dispatch);

    EXPECT_EQ(retired.destroyed, 1u);
    EXPECT_EQ(live.destroyed, 0u) << "switched in after the clear: kept";
    EXPECT_EQ(next.destroyed, 0u) << "switched in by the destructor: kept";
    list.clear(false);
    EXPECT_EQ(live.destroyed, 1u);
    EXPECT_EQ(next.destroyed, 1u);
}

TEST(ProtocolList, ARetiredProtocolsDestructorThatClearsEndsEveryProtocolInTheSameCall) {
    EndingRecord   retired, live;
    bool           in_dispatch = false;
    protocol_list  list;
    ArmEndingHooks armed; // disarms the hooks before `list` dies
    list.push_back(std::make_unique<EndingProtocol>(retired, &in_dispatch, [&] { list.clear(in_dispatch); }));
    list.clear(true);
    list.push_back(std::make_unique<EndingProtocol>(live, &in_dispatch));
    (void) list.end_dispatch(true, in_dispatch);

    EXPECT_EQ(retired.destroyed, 1u);
    EXPECT_EQ(live.destroyed, 1u) << "the destructor's clear retired it, and the same call ended it";
    EXPECT_TRUE(live.flag_when_destroyed);
    EXPECT_FALSE(in_dispatch);
}
