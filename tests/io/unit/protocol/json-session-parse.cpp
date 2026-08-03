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
