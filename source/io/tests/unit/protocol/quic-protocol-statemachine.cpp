/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/protocol/quic-protocol-statemachine.cpp
 * @brief In-process QUIC session protocol + io_handler state machine — pure unit, no socket, no loop.
 *
 * These cases exercise the QUIC *stream-session* layer entirely in memory: a `use<>::quic::session`
 * object (carrying a `qb::protocol::text::command` / `binary16` framing protocol, or none) is fed bytes
 * via `append()` and driven with `process()`, and an `async::quic::io_handler` is fed synthetic
 * `event::stream_data` events via `feed_stream_data` / drained via `drain_stream_output`. NO real
 * socket, NO event loop, NO `QB_HAS_QUIC` — they are deterministic and parallel. They were lifted out of
 * the misnamed `system/test-session-text.cpp` (where they masqueraded as `Session, *_OVER_QUIC` network
 * tests); the live loopback QUIC half stays in `system/quic/quic-handshake.cpp`.
 *
 * What is proven (the protocol + buffer + flow-control + session-keying contracts):
 *   - incremental framing: a `binary16` length prefix alone parses to "no message yet", the payload
 *     completes it, and `pendingRead()` empties.
 *   - protocol switch mid-stream: a text "SWITCH\n" flips the session to `binary16`, after which a binary
 *     frame is decoded by the new protocol.
 *   - `close_after_deliver()`: input is flushed, output is *retained*, and the protocol is disabled
 *     (`protocol()->ok() == false`).
 *   - a protocol-less raw session keeps pending input and emits a `pending_read` event with the byte count.
 *   - read/write buffer caps: an oversize `append` / `publish` is rejected and sets
 *     `disconnection_reason() == buffer_overflow`.
 *   - drain accounting: `drain_stream_output` reports the written bytes, resets the output pipe, and bumps
 *     `bytes_written()`; a re-entrant append during drain preserves FIFO chunk ordering.
 *   - io_handler keying: sessions are keyed by (connection_id, stream_id); `clearSessions(conn)` and
 *     `session.dispose()` remove only the matching connection's streams; flow credit is returned only
 *     after the protocol consumes a full frame (partial input grants zero credit).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>
#include <qb/io/protocol/text.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace qb::io;

namespace {

// ---------------------------------------------------------------------------
// Stream-session types under test (one per framing protocol).
// ---------------------------------------------------------------------------

class TextQuicSession : public use<TextQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<TextQuicSession>;

    std::string last_message;

    explicit TextQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        last_message.assign(message.text);
    }
};

class BinaryQuicSession : public use<BinaryQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::binary16<BinaryQuicSession>;

    std::string last_message;

    explicit BinaryQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        last_message.assign(message.data, message.size);
    }
};

class SwitchQuicSession : public use<SwitchQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<SwitchQuicSession>;

    std::size_t text_messages   = 0;
    std::size_t binary_messages = 0;
    std::string last_binary;

    explicit SwitchQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        ++text_messages;
        if (message.text == "SWITCH")
            this->template switch_protocol<qb::protocol::text::binary16<SwitchQuicSession>>(static_cast<SwitchQuicSession &>(*this));
    }

    void
    on(qb::protocol::text::binary16<SwitchQuicSession>::message &&message) {
        ++binary_messages;
        last_binary.assign(message.data, message.size);
    }
};

class CloseAfterDeliverQuicSession : public use<CloseAfterDeliverQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<CloseAfterDeliverQuicSession>;

    explicit CloseAfterDeliverQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        *this << message.text << Protocol::end;
        close_after_deliver();
    }
};

class RawQuicSession : public use<RawQuicSession>::quic::session {
public:
    std::size_t pending_read_events = 0;
    std::size_t last_pending_read   = 0;

    explicit RawQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(async::event::pending_read &&event) {
        ++pending_read_events;
        last_pending_read = event.bytes;
    }
};

// ---------------------------------------------------------------------------
// io_handler probes: a connector-style probe owning TextQuicSession streams and
// a server-owned variant, both exposing feed/drain so a test can drive them
// synchronously with no endpoint behind them.
// ---------------------------------------------------------------------------

class QuicDrainProbe : public async::quic::io_handler<QuicDrainProbe, TextQuicSession> {
public:
    bool
    feed(async::quic::event::stream_data const &event) {
        return feed_stream_data(event);
    }

    bool
    feed_with_credit(async::quic::event::stream_data const &event, std::uint64_t &credited) {
        return feed_stream_data(event, [&credited](std::uint64_t, std::uint64_t, std::uint64_t bytes) { credited += bytes; });
    }

    template <typename Session>
    void
    drain(Session &session, std::size_t &sent) {
        drain_stream_output(session, [&sent](std::uint64_t, std::uint64_t, std::span<const std::byte> data, bool) { sent += data.size(); });
    }

    template <typename Session, typename Send>
    void
    drain_with(Session &session, Send &&send) {
        drain_stream_output(session, std::forward<Send>(send));
    }
};

class ServerOwnedQuicProbe;

class ServerOwnedTextQuicSession : public use<ServerOwnedTextQuicSession>::quic::client<ServerOwnedQuicProbe> {
public:
    using Protocol = qb::protocol::text::command<ServerOwnedTextQuicSession>;

    std::string last_message;

    explicit ServerOwnedTextQuicSession(ServerOwnedQuicProbe &server)
        : client(server) {}

    void
    on(Protocol::message &&message) {
        last_message.assign(message.text);
    }
};

class ServerOwnedQuicProbe : public async::quic::io_handler<ServerOwnedQuicProbe, ServerOwnedTextQuicSession> {
public:
    bool
    feed(async::quic::event::stream_data const &event) {
        return feed_stream_data(event);
    }
};

[[nodiscard]] std::string
binary16_length_prefix(std::size_t length) {
    const std::uint16_t len = htons(static_cast<std::uint16_t>(length));
    return std::string(reinterpret_cast<char const *>(&len), sizeof(len));
}

} // namespace

// =============================================================================
// FRAMING — incremental binary16 + protocol switch
// =============================================================================

/**
 * @test binary16 parses a length prefix then a payload across two appends
 * @brief A lone length prefix yields process()==true but no message (incomplete frame); appending the
 *        payload completes it and drains the read buffer.
 */
TEST(QuicProtocolStateMachine, Binary16ParsesLengthThenPayload) {
    BinaryQuicSession session{0};
    const std::string payload = "binary-over-quic";

    session.append(binary16_length_prefix(payload.size()));
    EXPECT_TRUE(session.process());
    EXPECT_TRUE(session.last_message.empty()) << "an incomplete frame must not deliver a message";

    session.append(payload);
    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.last_message, payload);
    EXPECT_EQ(session.pendingRead(), 0u);
}

/**
 * @test A text "SWITCH" flips the session to the binary16 protocol mid-stream
 * @brief After the switch, a binary frame is decoded by the new protocol; text and binary counters and
 *        the decoded payload are all exact.
 */
TEST(QuicProtocolStateMachine, ProtocolSwitchTextToBinary) {
    SwitchQuicSession session{0};

    session.append("SWITCH\n");
    ASSERT_TRUE(session.process());
    EXPECT_EQ(session.text_messages, 1u);

    const std::string payload = "after-switch";
    session.append(binary16_length_prefix(payload.size()));
    session.append(payload);

    ASSERT_TRUE(session.process());
    EXPECT_EQ(session.binary_messages, 1u);
    EXPECT_EQ(session.last_binary, payload);
}

// =============================================================================
// close_after_deliver + raw (protocol-less) session
// =============================================================================

/**
 * @test close_after_deliver flushes input, keeps output, and disables the protocol
 * @brief After delivering "bye\n" the read buffer is empty, the echoed "bye\n" remains in the output
 *        pipe, and protocol()->ok() is false — the documented "input flushed, output retained, protocol
 *        disabled" invariant.
 */
TEST(QuicProtocolStateMachine, CloseAfterDeliverFlushesInputAndKeepsOutput) {
    CloseAfterDeliverQuicSession session{0};

    session.append("bye\n");

    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 4u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "bye\n");
    EXPECT_FALSE(session.protocol()->ok());
}

/**
 * @test A protocol-less session keeps pending input and emits a pending_read event
 * @brief With no Protocol, raw bytes accumulate in the read buffer and a single pending_read event is
 *        delivered carrying the byte count.
 */
TEST(QuicProtocolStateMachine, RawSessionKeepsPendingInputWithoutProtocol) {
    RawQuicSession session{0};

    session.append("raw-bytes");

    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.pendingRead(), 9u);
    EXPECT_EQ(session.pending_read_events, 1u);
    EXPECT_EQ(session.last_pending_read, 9u);
}

// =============================================================================
// BUFFER CAPS
// =============================================================================

/**
 * @test An oversize append is rejected and fails protocol processing with buffer_overflow
 * @brief A 4-byte read cap rejects a 5-byte append (returns false, no bytes buffered); process() then
 *        fails and the disconnection reason is buffer_overflow.
 */
TEST(QuicProtocolStateMachine, ReadCapOverflowFailsProtocolProcessing) {
    TextQuicSession session{0};
    session.set_max_read_buffer_size(4);

    EXPECT_FALSE(session.append("hello"));
    EXPECT_EQ(session.pendingRead(), 0u);

    EXPECT_FALSE(session.process());
    EXPECT_EQ(session.disconnection_reason(), static_cast<int>(async::event::disconnect_reason::buffer_overflow));
}

/**
 * @test An oversize publish is rejected with buffer_overflow
 * @brief A 4-byte write cap rejects a 5-byte publish (returns nullptr, no bytes queued) and records the
 *        buffer_overflow disconnection reason.
 */
TEST(QuicProtocolStateMachine, WriteCapOverflowRejectsPublish) {
    TextQuicSession session{0};
    session.set_max_write_buffer_size(4);

    EXPECT_EQ(session.publish("hello", std::size_t{5}), nullptr);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.disconnection_reason(), static_cast<int>(async::event::disconnect_reason::buffer_overflow));
}

// =============================================================================
// DRAIN ACCOUNTING
// =============================================================================

/**
 * @test drain_stream_output accounts written bytes and resets the output pipe
 * @brief Before draining, bytes_written()==0; draining "typed\n" reports 6 bytes, empties the pipe,
 *        resets out() to its base, and bumps bytes_written() to 6.
 */
TEST(QuicProtocolStateMachine, DrainAccountsWrittenBytesAfterBackendAcceptsOutput) {
    TextQuicSession session{0};
    QuicDrainProbe  handler;
    std::size_t     sent = 0;

    session << "typed" << TextQuicSession::Protocol::end;
    ASSERT_EQ(session.bytes_written(), 0u);

    handler.drain(session, sent);

    EXPECT_EQ(sent, 6u);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.out().begin(), session.out().data());
    EXPECT_EQ(session.bytes_written(), 6u);
}

/**
 * @test A re-entrant append during drain preserves FIFO chunk ordering
 * @brief Appending "second\n" from inside the drain callback must NOT reorder the output: the two chunks
 *        come out as "first\n" then "second\n" and the pipe ends fully drained.
 */
TEST(QuicProtocolStateMachine, OutputPipeKeepsOrderWhenDrainIsReentered) {
    TextQuicSession          session{0};
    QuicDrainProbe           handler;
    std::vector<std::string> chunks;
    bool                     appended = false;

    session << "first" << TextQuicSession::Protocol::end;

    handler.drain_with(session, [&](std::uint64_t, std::uint64_t, std::span<const std::byte> data, bool) {
        chunks.emplace_back(reinterpret_cast<char const *>(data.data()), data.size());
        if (!appended) {
            appended = true;
            session << "second" << TextQuicSession::Protocol::end;
        }
    });

    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], "first\n");
    EXPECT_EQ(chunks[1], "second\n");
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.out().begin(), session.out().data());
}

// =============================================================================
// io_handler SESSION KEYING + FLOW CREDIT
// =============================================================================

/**
 * @test Sessions are keyed by (connection_id, stream_id)
 * @brief Feeding stream_data for connections 10 and 11 yields two distinct sessions with the right
 *        connection ids and parsed messages; the map holds exactly two.
 */
TEST(QuicProtocolStateMachine, SessionsAreKeyedByConnectionAndStreamId) {
    QuicDrainProbe handler;

    ASSERT_TRUE(handler.feed({10, 0, "first\n", true}));
    ASSERT_TRUE(handler.feed({11, 0, "second\n", true}));

    auto *first  = handler.session(10, 0);
    auto *second = handler.session(11, 0);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(first->connection_id(), 10u);
    EXPECT_EQ(second->connection_id(), 11u);
    EXPECT_EQ(first->last_message, "first");
    EXPECT_EQ(second->last_message, "second");
    EXPECT_EQ(handler.session_count(), 2u);
}

/**
 * @test clearSessions(conn) removes only the matching connection's streams
 * @brief With conn10/stream0, conn10/stream4 and conn11/stream0 registered, clearSessions(10) drops both
 *        conn10 streams and keeps conn11, leaving exactly one session.
 */
TEST(QuicProtocolStateMachine, ConnectionCloseOnlyClearsMatchingConnectionStreams) {
    QuicDrainProbe handler;

    ASSERT_TRUE(handler.feed({10, 0, "first\n", true}));
    ASSERT_TRUE(handler.feed({10, 4, "first-other\n", true}));
    ASSERT_TRUE(handler.feed({11, 0, "second\n", true}));

    handler.clearSessions(10);

    EXPECT_EQ(handler.session(10, 0), nullptr);
    EXPECT_EQ(handler.session(10, 4), nullptr);
    auto *remaining = handler.session(11, 0);
    ASSERT_NE(remaining, nullptr);
    EXPECT_EQ(remaining->last_message, "second");
    EXPECT_EQ(handler.session_count(), 1u);
}

/**
 * @test session.dispose() removes only the disposing session's connection
 * @brief A server-owned session disposing itself unregisters only its (connection,stream) key; the other
 *        connection's session is intact.
 */
TEST(QuicProtocolStateMachine, SessionDisposeRemovesMatchingConnectionOnly) {
    ServerOwnedQuicProbe handler;

    ASSERT_TRUE(handler.feed({10, 0, "first\n", true}));
    ASSERT_TRUE(handler.feed({11, 0, "second\n", true}));

    auto *first = handler.session(10, 0);
    ASSERT_NE(first, nullptr);
    first->dispose();

    EXPECT_EQ(handler.session(10, 0), nullptr);
    auto *remaining = handler.session(11, 0);
    ASSERT_NE(remaining, nullptr);
    EXPECT_EQ(remaining->last_message, "second");
    EXPECT_EQ(handler.session_count(), 1u);
}

/**
 * @test Flow credit is returned only after the protocol consumes a full frame
 * @brief A partial "hello" (fin=false) leaves 5 bytes pending and grants zero credit; the completing
 *        " world\n" (fin=true) drains the buffer, delivers "hello world", and returns credit for all 12
 *        consumed bytes.
 */
TEST(QuicProtocolStateMachine, FlowCreditIsReturnedOnlyAfterProtocolConsumesBytes) {
    QuicDrainProbe handler;
    std::uint64_t  credited = 0;

    ASSERT_TRUE(handler.feed_with_credit({10, 0, "hello", false}, credited));
    auto *session = handler.session(10, 0);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->pendingRead(), 5u);
    EXPECT_EQ(credited, 0u) << "no credit until a full frame is consumed";

    ASSERT_TRUE(handler.feed_with_credit({10, 0, " world\n", true}, credited));
    EXPECT_EQ(session->pendingRead(), 0u);
    EXPECT_EQ(session->last_message, "hello world");
    EXPECT_EQ(credited, 12u);
}
