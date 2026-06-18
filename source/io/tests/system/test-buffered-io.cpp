/**
 * @file qb/source/io/tests/system/test-buffered-io.cpp
 * @brief Unit tests for logical buffered async I/O sessions.
 *
 * These tests exercise the protocol/buffer state machine used by logical
 * transports such as QUIC streams without requiring a native QUIC backend.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @ingroup Tests
 */

#include <gtest/gtest.h>

#include <qb/io/async/buffered_io.h>
#include <qb/io/protocol/base.h>

#include <arpa/inet.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

class FixedFrameProtocol : public qb::io::async::AProtocol<BufferedProbe> {
    std::size_t _frame_size;
    bool        _close_after_message;

public:
    FixedFrameProtocol(BufferedProbe &io, std::size_t frame_size, bool close_after_message = false) noexcept;

    std::size_t getMessageSize() noexcept final;
    void        onMessage(std::size_t size) noexcept final;
    void
    reset() noexcept final {}
};

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
        ADD_FAILURE();
    }
    void
    reset() noexcept final {}
};

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

FixedFrameProtocol::FixedFrameProtocol(BufferedProbe &io, std::size_t frame_size, bool close_after_message) noexcept
    : AProtocol(io)
    , _frame_size(frame_size)
    , _close_after_message(close_after_message) {}

std::size_t
FixedFrameProtocol::getMessageSize() noexcept {
    return _io.pendingRead() >= _frame_size ? _frame_size : 0u;
}

void
FixedFrameProtocol::onMessage(std::size_t size) noexcept {
    _io.messages.emplace_back(_io.in().begin(), size);
    if (_close_after_message) {
        _io.publish(std::string_view{"reply"});
        _io.close_after_deliver();
    }
}

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

    using base_io_t = qb::io::async::buffered_io<ServerOwnedBufferedProbe>;

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

class ProtocolProbe {
    qb::allocator::pipe<char> _in;

public:
    using base_io_t = qb::io::async::buffered_io<ProtocolProbe>;

    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }

    void
    append(std::string_view data) {
        _in << data;
    }
};

struct DoubleCrLf {
    static constexpr char _EndBytes[] = "\r\n\r\n";
};

constexpr char DoubleCrLf::_EndBytes[];

class LineTerminatedProtocol : public qb::protocol::base::byte_terminated<ProtocolProbe, '\n'> {
public:
    using byte_terminated::byte_terminated;

    void
    onMessage(std::size_t) noexcept final {}
};

class HeaderTerminatedProtocol : public qb::protocol::base::bytes_terminated<ProtocolProbe, DoubleCrLf> {
public:
    using bytes_terminated::bytes_terminated;

    void
    onMessage(std::size_t) noexcept final {}
};

template <typename Size>
class SizeHeaderProtocol : public qb::protocol::base::size_as_header<ProtocolProbe, Size> {
public:
    using qb::protocol::base::size_as_header<ProtocolProbe, Size>::size_as_header;

    void
    onMessage(std::size_t) noexcept final {}
};

} // namespace

TEST(BufferedIO, RejectsInvalidProtocolAndCanClearStoredProtocols) {
    BufferedProbe session;
    const auto   &const_session = session;

    EXPECT_EQ(session.switch_protocol<RejectingProtocol>(session), nullptr);
    EXPECT_EQ(session.protocol(), nullptr);
    EXPECT_EQ(const_session.protocol(), nullptr);

    auto *protocol = session.switch_protocol<FixedFrameProtocol>(session, 4u);
    ASSERT_NE(protocol, nullptr);
    EXPECT_EQ(session.protocol(), protocol);

    session.clear_protocols();
    EXPECT_EQ(session.protocol(), nullptr);
    EXPECT_FALSE(session.has_pending_read());
}

TEST(BufferedIO, AccessorsCountersAndTypedDisconnectTrackLifecycleState) {
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

TEST(BufferedIO, ProcessInputStopsWhenProtocolWasClosedBeforeParsing) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<FixedFrameProtocol>(session, 4u), nullptr);
    session.close_after_deliver();

    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.pendingRead(), 4u);
    EXPECT_TRUE(session.messages.empty());
}

TEST(BufferedIO, NoProtocolReportsPendingReadAndEofEvents) {
    BufferedProbe session;

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.pending_read_events, 0u);
    EXPECT_EQ(session.eof_events, 0u);

    session.append("raw");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.pending_read_events, 1u);
    EXPECT_EQ(session.last_pending_read, 3u);

    session.flush(session.pendingRead());
    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.eof_events, 0u);
}

TEST(BufferedIO, InvalidProtocolStopsBeforeParsingWithoutFlushingInput) {
    BufferedProbe session;
    auto         *protocol = session.switch_protocol<FixedFrameProtocol>(session, 4u);
    ASSERT_NE(protocol, nullptr);
    protocol->not_ok();

    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.pendingRead(), 4u);
    EXPECT_TRUE(session.messages.empty());
}

TEST(BufferedIO, ProcessesFramesFlushesInputAndReportsPendingReadThenEof) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<FixedFrameProtocol>(session, 3u), nullptr);

    session.append("abcdefghiZ");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"abc", "def", "ghi"}));
    EXPECT_EQ(session.pendingRead(), 1u);
    EXPECT_EQ(session.pending_read_events, 1u);
    EXPECT_EQ(session.last_pending_read, 1u);
    EXPECT_EQ(session.bytes_read(), 10u);
    EXPECT_EQ(session.messages_processed(), 3u);

    session.flush(session.pendingRead());
    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.eof_events, 1u);
}

TEST(BufferedIO, CloseAfterDeliverKeepsOutputAvailableForDrain) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<FixedFrameProtocol>(session, 4u, true), nullptr);

    session.append("data");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 5u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "reply");
    EXPECT_FALSE(session.protocol()->ok());
    EXPECT_EQ(session.disconnection_reason(), 0);
}

TEST(BufferedIO, DisconnectDuringMessageFlushesInputAndKeepsPendingOutput) {
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

TEST(BufferedIO, ReentrantProcessInputReturnsImmediatelyWhileMessageIsActive) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<ReentrantProtocol>(session), nullptr);

    session.append("xy");

    EXPECT_TRUE(session.process_input());
    EXPECT_TRUE(session.reentrant_process_result);
    EXPECT_EQ(session.messages, (std::vector<std::string>{"xy"}));
    EXPECT_EQ(session.pendingRead(), 0u);
}

TEST(BufferedIO, ClearingProtocolDuringMessageDisconnectsAfterFlushingInput) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<ClearProtocolOnMessage>(session), nullptr);

    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.disconnection_reason(), -1);
}

TEST(BufferedIO, InvalidProtocolAfterMessageKeepsPendingOutputForDrain) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<InvalidatingProtocol>(session, true), nullptr);

    session.append("data");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 5u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "reply");
    EXPECT_EQ(session.disconnection_reason(), 0);
}

TEST(BufferedIO, InvalidProtocolAfterMessageWithoutOutputDisconnects) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<InvalidatingProtocol>(session, false), nullptr);

    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.disconnection_reason(), -1);
}

TEST(BufferedIO, NonFlushingProtocolPreservesInputUntilCallerFlushes) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<NonFlushingProtocol>(session), nullptr);

    session.append("data");

    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 4u);

    session.flush(session.pendingRead());
    EXPECT_TRUE(session.process_input());
    EXPECT_EQ(session.eof_events, 1u);
}

TEST(BufferedIO, NonFlushingDisconnectLeavesInputForManualDrain) {
    BufferedProbe session;
    ASSERT_NE(session.switch_protocol<NonFlushingProtocol>(session, true), nullptr);

    session.append("data");

    EXPECT_FALSE(session.process_input());
    EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
    EXPECT_EQ(session.pendingRead(), 4u);
    EXPECT_EQ(session.disconnection_reason(), 91);
}

TEST(BufferedIO, OversizedOrIncoherentFramesDisconnectProcessing) {
    {
        BufferedProbe session;
        ASSERT_NE(session.switch_protocol<OversizedProtocol>(session, 8u), nullptr);
        session.set_max_message_size(4u);
        session.append("12345678");

        EXPECT_FALSE(session.process_input());
        EXPECT_EQ(session.disconnection_reason(), -2);
        EXPECT_FALSE(session.protocol()->ok());
    }

    {
        BufferedProbe session;
        ASSERT_NE(session.switch_protocol<OversizedProtocol>(session, 8u), nullptr);
        session.append("1");

        EXPECT_FALSE(session.process_input());
        EXPECT_EQ(session.disconnection_reason(), -2);
        EXPECT_FALSE(session.protocol()->ok());
    }
}

TEST(BufferedIO, PublishOverflowRollsBackPartialAppendAndDisconnects) {
    BufferedProbe session;
    session.set_max_write_buffer_size(4u);

    session.publish(std::string_view{"ab"});
    EXPECT_EQ(session.pendingWrite(), 2u);

    session.publish(std::string_view{"cdef"});
    EXPECT_EQ(session.pendingWrite(), 4u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd");
    EXPECT_EQ(session.disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));

    session.publish(std::string_view{"ignored"});
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd");
}

TEST(BufferedIO, PublishAtExistingWriteCapDisconnectsWithoutAppending) {
    BufferedProbe session;
    session.set_max_write_buffer_size(2u);

    session.publish(std::string_view{"ab"});
    ASSERT_EQ(session.pendingWrite(), 2u);

    session.publish(std::string_view{"c"});
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "ab");
    EXPECT_EQ(session.disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));
}

TEST(BufferedIO, DisposeIsIdempotentAndResetRestoresConnectionState) {
    BufferedProbe session;
    session.disconnect(0);

    EXPECT_FALSE(session.is_connected());
    session.dispose();
    session.dispose();

    EXPECT_EQ(session.disconnected_events, 1u);
    EXPECT_EQ(session.dispose_events, 1u);
    EXPECT_EQ(session.last_disconnect_reason, static_cast<int>(qb::io::async::event::disconnect_reason::user_initiated));
    EXPECT_FALSE(session.process_input());

    session.reset_state();
    EXPECT_TRUE(session.is_connected());
}

TEST(BufferedIO, ServerOwnedDisposeReportsConnectionAndStreamIdentity) {
    ServerDisposeRecorder    recorder;
    ServerOwnedBufferedProbe session{recorder};

    session.dispose();
    session.dispose();

    EXPECT_EQ(recorder.disconnected_calls, 1u);
    EXPECT_EQ(recorder.last_connection, 7u);
    EXPECT_EQ(recorder.last_stream, 42u);
}

TEST(ProtocolBase, ByteAndSequenceTerminatorsTrackOffsetsAndReset) {
    ProtocolProbe          probe;
    LineTerminatedProtocol line_protocol{probe};

    probe.append("partial");
    EXPECT_EQ(line_protocol.getMessageSize(), 0u);
    probe.append("-line\nnext");
    EXPECT_EQ(line_protocol.getMessageSize(), 13u);
    EXPECT_EQ(line_protocol.shiftSize(13u), 12u);
    EXPECT_EQ(line_protocol.shiftSize(0u), 0u);

    line_protocol.reset();
    probe.in().reset();
    probe.append("abc\r\n\r\ntrailer");

    HeaderTerminatedProtocol header_protocol{probe};
    EXPECT_EQ(header_protocol.getMessageSize(), 7u);
    EXPECT_EQ(header_protocol.shiftSize(7u), 3u);
    EXPECT_EQ(header_protocol.shiftSize(2u), 0u);

    header_protocol.reset();
    probe.in().reset();
    probe.append("abc");
    EXPECT_EQ(header_protocol.getMessageSize(), 0u);
    probe.append("defgh");
    EXPECT_EQ(header_protocol.getMessageSize(), 0u);
}

TEST(ProtocolBase, SizeHeaderRejectsZeroAndChecksHeaderCapacity) {
    ProtocolProbe                     probe;
    SizeHeaderProtocol<std::uint16_t> protocol{probe};

    const std::uint16_t zero = 0;
    probe.append(std::string_view{reinterpret_cast<char const *>(&zero), sizeof(zero)});

    EXPECT_EQ(protocol.getMessageSize(), 0u);
    EXPECT_FALSE(protocol.ok());

    auto make_too_large_header = [] {
        return SizeHeaderProtocol<std::uint8_t>::Header(256u);
    };
    EXPECT_THROW((void) make_too_large_header(), std::runtime_error);

    EXPECT_EQ(SizeHeaderProtocol<std::uint8_t>::Header(7u), 7u);

    const auto header = SizeHeaderProtocol<std::uint32_t>::Header(7u);
    EXPECT_EQ(ntohl(header), 7u);

    protocol.reset();
}

TEST(ProtocolBase, SizeHeaderHandlesOneAndFourByteHeadersAcrossPartialFrames) {
    {
        ProtocolProbe                    probe;
        SizeHeaderProtocol<std::uint8_t> protocol{probe};
        const auto                       header = SizeHeaderProtocol<std::uint8_t>::Header(3u);

        probe.append(std::string_view{reinterpret_cast<char const *>(&header), sizeof(header)});
        EXPECT_EQ(protocol.shiftSize(), sizeof(header));
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_EQ(probe.in().size(), 0u);

        probe.append("ab");
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        probe.append("c");
        EXPECT_EQ(protocol.getMessageSize(), 3u);

        protocol.reset();
    }

    {
        ProtocolProbe                     probe;
        SizeHeaderProtocol<std::uint32_t> protocol{probe};
        const auto                        header = SizeHeaderProtocol<std::uint32_t>::Header(4u);
        const std::string_view header_view{reinterpret_cast<char const *>(&header), sizeof(header)};

        probe.append(header_view.substr(0u, 2u));
        EXPECT_EQ(protocol.getMessageSize(), 0u);

        probe.append(header_view.substr(2u));
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_EQ(probe.in().size(), 0u);

        probe.append("data");
        EXPECT_EQ(protocol.getMessageSize(), 4u);
        EXPECT_TRUE(protocol.ok());
    }
}
