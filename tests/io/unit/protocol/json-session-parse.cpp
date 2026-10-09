/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/protocol/json-session-parse.cpp
 * @brief `qb::protocol::json` over a QUIC stream session — pure in-process parser, no socket, no loop.
 *
 * This is the unit outlier extracted from `system/test-session-json.cpp` (where it lived as the
 * misnamed `Session, JSON_MALFORMED_OVER_QUIC`): it constructs a `use<>::quic::session` carrying the
 * `\0`-delimited `qb::protocol::json` framing protocol, feeds raw bytes with `append()`, and drives the
 * parser with `process()` — NO event loop, NO real socket, NO `QB_HAS_QUIC`. It exercises exactly the
 * `json::onMessage` contract (`json.h`):
 *   - a well-formed `{...}\0` frame parses, delivers an `on(message)` with the right values, and drains
 *     the read buffer;
 *   - a malformed `{...}\0` frame is discarded by the parser, which calls `not_ok()` so `process()`
 *     returns false and NO message is delivered (the framework never hands a discarded body to `on`);
 *   - a frame split across two `append()` calls is parsed incrementally — the first half yields "no
 *     message yet" (process()==true, nothing delivered), the completing half delivers it.
 *
 * And the same for `qb::protocol::json_packed`, whose frame is one MessagePack value then a NUL: the value is framed
 * by its own lengths, so the zero bytes MessagePack carries inside a value (the integer 0, a length, a float, a string
 * or binary byte) are data, never the terminator (Huly QB-305).
 *
 * The system-tier round-trip (JSON over real TCP/TLS/QUIC loopback) lives in
 * `system/session/session-json.cpp`.
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

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>
#include <qb/json.h>

using namespace qb::io;

namespace {

class JsonQuicSession : public use<JsonQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::json<JsonQuicSession>;

    int      messages = 0;
    qb::json last_json;

    explicit JsonQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        ++messages;
        last_json = std::move(message.json);
    }
};

[[nodiscard]] std::string
nul_terminated(std::string body) {
    body.push_back('\0');
    return body;
}

class JsonPackedQuicSession : public use<JsonPackedQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::json_packed<JsonPackedQuicSession>;

    int         messages  = 0;
    std::size_t last_size = 0;
    qb::json    last_json;

    explicit JsonPackedQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        ++messages;
        last_size = message.size;
        last_json = std::move(message.json);
    }
};

// One json_packed frame, as a sender writes it: the MessagePack encoding of the value, then the NUL.
[[nodiscard]] std::string
packed_frame(const qb::json &value) {
    const std::vector<std::uint8_t> bytes = qb::json::to_msgpack(value);
    return nul_terminated(std::string(bytes.begin(), bytes.end()));
}

} // namespace

// =============================================================================
// MALFORMED INPUT
// =============================================================================

/**
 * @test A malformed JSON frame fails parser processing and delivers no message
 * @brief The discarded payload makes json::onMessage call not_ok(), so process() returns false and the
 *        session's on(message) is never invoked. This is the headline resilience contract: malformed
 *        input is rejected, not crashed-on, and not silently accepted.
 */
TEST(JsonSessionParse, MalformedFrameFailsProcessingAndDeliversNothing) {
    JsonQuicSession session{0};

    session.append(nul_terminated("{not-json}"));

    EXPECT_FALSE(session.process()) << "a discarded JSON body must fail process()";
    EXPECT_EQ(session.messages, 0) << "a malformed frame must NOT be delivered to on(message)";
}

// =============================================================================
// WELL-FORMED INPUT
// =============================================================================

/**
 * @test A well-formed JSON frame parses, delivers, and drains the read buffer
 * @brief A complete `{"message":...,"n":42}\0` frame is parsed into the exact object, delivered once via
 *        on(message), and leaves no pending input.
 */
TEST(JsonSessionParse, WellFormedFrameParsesAndDelivers) {
    JsonQuicSession session{0};

    session.append(nul_terminated(qb::json{{"message", "hello-json"}, {"n", 42}}.dump()));

    EXPECT_TRUE(session.process());
    ASSERT_EQ(session.messages, 1);
    ASSERT_TRUE(session.last_json.is_object());
    EXPECT_EQ(session.last_json["message"].get<std::string>(), "hello-json");
    EXPECT_EQ(session.last_json["n"].get<int>(), 42);
    EXPECT_EQ(session.pendingRead(), 0u);
}

// =============================================================================
// INCREMENTAL PARSE (frame split across two appends)
// =============================================================================

/**
 * @test A frame split across two appends is parsed incrementally
 * @brief The first half (no `\0` terminator yet) yields process()==true but no delivery; the completing
 *        half (including the terminator) delivers the reconstructed object exactly once.
 */
TEST(JsonSessionParse, SplitFrameIsParsedIncrementally) {
    JsonQuicSession   session{0};
    const std::string body = qb::json{{"message", "split"}}.dump();
    ASSERT_GT(body.size(), 4u);

    const std::string head = body.substr(0, 4);
    const std::string tail = body.substr(4);

    session.append(head);
    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.messages, 0) << "an unterminated partial frame must not deliver yet";

    session.append(nul_terminated(tail));
    EXPECT_TRUE(session.process());
    ASSERT_EQ(session.messages, 1);
    EXPECT_EQ(session.last_json["message"].get<std::string>(), "split");
    EXPECT_EQ(session.pendingRead(), 0u);
}

// =============================================================================
// json_packed: ONE MESSAGEPACK VALUE, THEN THE NUL (Huly QB-305)
// =============================================================================

/**
 * @test A zero byte inside a MessagePack value is data, not the terminator
 * @brief `json_packed` cut a frame at the first zero byte, and MessagePack writes zero bytes inside values: the
 *        integer 0 is the byte 0x00, a length or a 16-bit integer carries one, a float or a binary its own. Each of
 *        these frames is delivered whole, its value intact, and the read buffer drained.
 */
TEST(JsonPackedSessionParse, AZeroByteInsideTheValueIsDataNotTheTerminator) {
    const qb::json values[] = {
        qb::json{{"qty", 0}},                    // fixint 0
        qb::json(0),                             // the whole value is the byte 0x00
        qb::json{{"s", std::string("a\0b", 3)}}, // a NUL inside a string
        qb::json::binary({0x00, 0x00, 0x01}),    // bin 8: zero bytes of data
        qb::json(256),                           // uint 16: 0xcd 0x01 0x00
        qb::json(-256),                          // int 16: 0xd1 0xff 0x00
        qb::json(0.0),                           // a float of zero bytes
        qb::json{{"list", qb::json::array({0, 0, 0})}, {"n", nullptr}}
    };
    for (const qb::json &value : values) {
        JsonPackedQuicSession session{0};
        const std::string     frame = packed_frame(value);

        session.append(frame);

        EXPECT_TRUE(session.process()) << value.dump();
        ASSERT_EQ(session.messages, 1) << value.dump() << " was cut at a zero byte of its own";
        EXPECT_EQ(session.last_json, value);
        EXPECT_EQ(session.last_size, frame.size() - 1) << "the message is the value, without its NUL";
        EXPECT_EQ(session.pendingRead(), 0u);
    }
}

/**
 * @test A frame split anywhere is framed incrementally
 * @brief The framing resumes where the previous read stopped: a frame cut at any byte -- inside a header, a length,
 *        a string, a nested container, or right before its NUL -- delivers nothing until its last byte has come, then
 *        delivers exactly the value. Fed one byte at a time, the same.
 */
TEST(JsonPackedSessionParse, AFrameSplitAnywhereIsFramedIncrementally) {
    qb::json value          = {{"a", qb::json::array({0, 1, qb::json{{"b", 0}}})}, {"s", std::string(300, 'x')}, {"n", -1}};
    value["bin"]            = qb::json::binary({0x00, 0x7f, 0x00});
    const std::string frame = packed_frame(value);

    for (std::size_t cut = 0; cut <= frame.size(); ++cut) {
        JsonPackedQuicSession session{0};
        session.append(frame.substr(0, cut));
        EXPECT_TRUE(session.process()) << "cut at " << cut;
        EXPECT_EQ(session.messages, cut == frame.size() ? 1 : 0) << "cut at " << cut;
        session.append(frame.substr(cut));
        EXPECT_TRUE(session.process()) << "cut at " << cut;
        ASSERT_EQ(session.messages, 1) << "cut at " << cut;
        EXPECT_EQ(session.last_json, value) << "cut at " << cut;
        EXPECT_EQ(session.pendingRead(), 0u) << "cut at " << cut;
    }

    JsonPackedQuicSession session{0};
    for (std::size_t at = 0; at < frame.size(); ++at) {
        EXPECT_EQ(session.messages, 0) << "delivered before byte " << at;
        session.append(frame.substr(at, 1));
        EXPECT_TRUE(session.process()) << "at byte " << at;
    }
    ASSERT_EQ(session.messages, 1);
    EXPECT_EQ(session.last_json, value);
}

/**
 * @test Frames read together are delivered one by one
 * @brief Two frames in one read, each carrying zero bytes of its own, are two messages, in order.
 */
TEST(JsonPackedSessionParse, FramesReadTogetherAreDeliveredOneByOne) {
    JsonPackedQuicSession session{0};

    session.append(packed_frame(qb::json{{"n", 0}}) + packed_frame(qb::json{{"n", 256}}) + packed_frame(qb::json{{"n", 2}}));

    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.messages, 3);
    EXPECT_EQ(session.last_json, (qb::json{{"n", 2}}));
    EXPECT_EQ(session.pendingRead(), 0u);
}

/**
 * @test The byte after the value must be the NUL
 * @brief Once one whole value has arrived, the next byte is its terminator or the stream is not json_packed: any
 *        other byte fails process() at once and delivers nothing -- it is not waited past for a NUL further on.
 */
TEST(JsonPackedSessionParse, AnyByteButTheNulAfterTheValueIsRefused) {
    const std::vector<std::uint8_t> bytes = qb::json::to_msgpack(qb::json{{"a", 1}});
    JsonPackedQuicSession           session{0};

    session.append(std::string(bytes.begin(), bytes.end()) + "X");

    EXPECT_FALSE(session.process()) << "a value followed by 'X' is not a json_packed frame";
    EXPECT_EQ(session.messages, 0);
}

/**
 * @test Nesting past the bound and the byte MessagePack never uses are refused
 * @brief The framing bounds the nesting the recursive decode will walk (kJsonMaxNestingDepth, 512, an empty
 *        container counted): 512 nested arrays are a message, 513 are refused before anything is decoded, 600
 *        too. 0xc1 is never MessagePack.
 */
TEST(JsonPackedSessionParse, NestingPastTheBoundAndTheReservedByteAreRefused) {
    const auto nested = [](std::size_t depth) {
        std::string frame(depth, static_cast<char>(0x91)); // fixarray of one element, `depth` times
        frame.push_back(static_cast<char>(0xc0));          // nil
        return nul_terminated(frame);
    };

    {
        JsonPackedQuicSession session{0};
        session.append(nested(qb::protocol::detail::kJsonMaxNestingDepth));
        EXPECT_TRUE(session.process()) << "nesting AT the bound is accepted";
        EXPECT_EQ(session.messages, 1);
    }
    for (const std::size_t depth : {qb::protocol::detail::kJsonMaxNestingDepth + 1, std::size_t{600}}) {
        JsonPackedQuicSession session{0};
        session.append(nested(depth));
        EXPECT_FALSE(session.process()) << depth << " nested arrays were not refused";
        EXPECT_EQ(session.messages, 0);
    }
    {
        JsonPackedQuicSession session{0};
        session.append(nul_terminated(std::string(1, static_cast<char>(0xc1))));
        EXPECT_FALSE(session.process()) << "0xc1 is never MessagePack";
        EXPECT_EQ(session.messages, 0);
    }
}
