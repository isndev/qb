/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/stream/stream-templates.cpp
 * @brief `qb::io::{istream,ostream,stream}<_IO_>` buffering templates over a scripted transport.
 *
 * The stream templates (qb/io/stream.h) own the read/write buffering, the buffer-limit DoS guards
 * (`ErrBufferLimitExceeded` / publish→nullptr), partial-write reorder, and the close→disconnect+close
 * teardown — all of which sit ABOVE the transport. Driving them against a real socket/file is
 * inherently flaky (partial writes and short reads depend on kernel timing). Instead every case here
 * drives a `qb::io::test::ScriptedStreamTransport` (shared/scripted_stream_transport.h): a fully
 * in-memory `_IO_` whose read chunks, per-call write limits and failure injection are all scripted,
 * so each branch is deterministic. No fd, no socket, no event loop — pure `unit`.
 *
 * Contracts proven:
 *   - istream: read-buffer-limit → `ErrBufferLimitExceeded` (0 pending); injected read failure → -1;
 *     a scripted payload reads to the exact byte count; `eof()`/`flush()` buffer management.
 *   - ostream: partial writes drain the buffer in scripted steps and preserve the exact wire image;
 *     `LimitedScriptedOStream` proves publish() rejects (nullptr) once the cap would be crossed;
 *     an injected write failure keeps the pending bytes intact.
 *   - stream (bidirectional): partial writes + over-limit publish rejection + close()→disconnect+close.
 *   - NullDevice: the degenerate /dev/null stream (write claims success, read is EOF).
 *   - (revived) large chunked transfer through the scripted transport with no byte loss.
 *   - (revived) line-by-line `flush()` buffer-management semantics.
 *
 * Restructured from the dissolved system/test-stream-operations.cpp: the 4 `StreamTemplates.*` cases
 * plus `NullStream` are folded in verbatim-in-spirit (the local helper clones now come from the shared
 * header); `DISABLED_LargeDataTransfer` and `DISABLED_StreamBufferManagement` are REVIVED as real
 * deterministic unit tests against the scripted transport; the 3 dead `DISABLED_` dups
 * (TCPStream / UDPStream / MemoryBufferStream — duplicates of the socket suite / subsumed by the
 * scripted transport) are DELETED. Per-file main() dropped for the shared gtest main.
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
 * @ingroup Tests
 */

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/stream.h>

#include "../../shared/scripted_stream_transport.h"

using qb::io::test::LimitedScriptedOStream;
using qb::io::test::NullDevice;
using qb::io::test::ScriptedStreamTransport;

// =============================================================================
// istream — read-limit, injected failure, payload read, buffer management.
// =============================================================================

/**
 * @test istream read-limit, read-failure, payload, and const transport access.
 * @brief Salvaged verbatim-in-spirit from StreamTemplates.InputReadLimitsFailuresAndConstAccess.
 */
TEST(StreamTemplates, InputReadLimitsFailuresAndConstAccess) {
    // A read-buffer cap below one bucket makes the first read() trip the DoS guard.
    qb::io::istream<ScriptedStreamTransport> limited;
    limited.set_max_read_buffer_size(4u);
    EXPECT_EQ(limited.read(), qb::io::ErrBufferLimitExceeded);
    EXPECT_EQ(limited.pendingRead(), 0u);

    // Injected read failure surfaces as -1 with nothing buffered.
    qb::io::istream<ScriptedStreamTransport> failing;
    failing.transport() = ScriptedStreamTransport{"", {}, true};
    EXPECT_EQ(failing.read(), -1);
    EXPECT_EQ(failing.pendingRead(), 0u);

    // A scripted payload reads to its exact length; eof() is a no-op until flush() drains it.
    qb::io::istream<ScriptedStreamTransport> input;
    input.transport() = ScriptedStreamTransport{"payload"};
    ASSERT_EQ(input.read(), 7);
    EXPECT_EQ(input.pendingRead(), 7u);
    input.eof();
    EXPECT_EQ(input.pendingRead(), 7u);
    input.flush(input.pendingRead());
    input.eof();
    EXPECT_EQ(input.pendingRead(), 0u);

    const auto &const_input = input;
    EXPECT_FALSE(const_input.transport().closed);
}

// =============================================================================
// ostream — partial / full / rejected publishes.
// =============================================================================

/**
 * @test ostream partial-write drain, exact wire image, and publish cap rejection.
 * @brief Salvaged verbatim-in-spirit from StreamTemplates.OutputPartialFullAndRejectedPublishes.
 */
TEST(StreamTemplates, OutputPartialFullAndRejectedPublishes) {
    qb::io::ostream<ScriptedStreamTransport> output;
    output.transport() = ScriptedStreamTransport{"", {3u}}; // first write() caps at 3 bytes

    ASSERT_NE(output.publish("abcdef", 6u), nullptr);
    EXPECT_EQ(output.pendingWrite(), 6u);
    EXPECT_EQ(output.write(), 3);
    EXPECT_EQ(output.pendingWrite(), 3u);
    EXPECT_EQ(output.write(), 3); // limit vector exhausted → full chunk
    EXPECT_EQ(output.pendingWrite(), 0u);
    EXPECT_EQ(output.transport().written, "abcdef");

    const auto &const_output = output;
    EXPECT_EQ(const_output.transport().written, "abcdef");

    // A publish that would cross the cap is rejected (nullptr), leaving pending untouched.
    LimitedScriptedOStream limited;
    limited.set_max_write_buffer_size(3u);
    EXPECT_EQ(limited.publish("abcd", 4u), nullptr);
    EXPECT_EQ(limited.pendingWrite(), 0u);

    ASSERT_NE(limited.publish("ab", 2u), nullptr);
    limited.set_max_write_buffer_size(1u);
    EXPECT_EQ(limited.publish("c", 1u), nullptr); // 2 + 1 > cap of 1
    EXPECT_EQ(limited.pendingWrite(), 2u);
}

/**
 * @test An injected write failure keeps the pending data buffered.
 * @brief Salvaged verbatim-in-spirit from StreamTemplates.OutputWriteFailureKeepsPendingData.
 */
TEST(StreamTemplates, OutputWriteFailureKeepsPendingData) {
    qb::io::ostream<ScriptedStreamTransport> output;
    output.transport().fail_writes();

    ASSERT_NE(output.publish("data", 4u), nullptr);
    EXPECT_EQ(output.write(), -1);
    EXPECT_EQ(output.pendingWrite(), 4u);
    EXPECT_TRUE(output.transport().written.empty());
}

// =============================================================================
// stream — bidirectional partial writes, write-limit, teardown.
// =============================================================================

/**
 * @test Bidirectional partial writes, over-limit publish rejection, and close()→disconnect+close.
 * @brief Salvaged verbatim-in-spirit from StreamTemplates.BidirectionalStreamPartialWritesAndWriteLimit.
 */
TEST(StreamTemplates, BidirectionalStreamPartialWritesAndWriteLimit) {
    qb::io::stream<ScriptedStreamTransport> stream;
    stream.transport() = ScriptedStreamTransport{"", {2u}};

    ASSERT_NE(stream.publish("wxyz", 4u), nullptr);
    EXPECT_EQ(stream.write(), 2);
    EXPECT_EQ(stream.pendingWrite(), 2u);
    EXPECT_EQ(stream.write(), 2);
    EXPECT_EQ(stream.pendingWrite(), 0u);
    EXPECT_EQ(stream.transport().written, "wxyz");

    stream.set_max_write_buffer_size(3u);
    EXPECT_EQ(stream.max_write_buffer_size(), 3u);
    EXPECT_EQ(stream.publish("abcd", 4u), nullptr); // 4 > cap of 3
    EXPECT_EQ(stream.pendingWrite(), 0u);

    stream.close();
    EXPECT_TRUE(stream.transport().disconnected);
    EXPECT_TRUE(stream.transport().closed);
}

// =============================================================================
// NullDevice — the degenerate stream.
// =============================================================================

/**
 * @test A /dev/null stream reports full writes and EOF reads, buffering nothing.
 * @brief Salvaged from StreamTest.NullStream; NullDevice now lives in the shared header.
 */
TEST(StreamTemplates, NullDeviceWritesSucceedAndReadsAreEof) {
    qb::io::stream<NullDevice> null_stream;
    null_stream.transport() = NullDevice{};

    const std::string payload = "This data should be discarded";
    ASSERT_NE(null_stream.publish(payload.c_str(), payload.size()), nullptr);
    EXPECT_EQ(null_stream.write(), static_cast<int>(payload.size()));

    EXPECT_EQ(null_stream.read(), 0); // EOF
    EXPECT_EQ(null_stream.in().size(), 0u);
}

// =============================================================================
// REVIVED — large chunked transfer (was DISABLED_LargeDataTransfer).
// =============================================================================

/**
 * @test A 1 MiB read script is drained chunk-by-chunk with no byte loss.
 * @brief Revives DISABLED_LargeDataTransfer deterministically. The scripted transport hands the
 *        istream the payload in read()-sized chunks (the stream's own bucket size), and we
 *        accumulate via flush() until the whole payload is consumed — proving the read/flush loop
 *        handles a multi-read transfer without dropping or duplicating bytes. No file, no socket.
 */
TEST(StreamTemplates, LargeChunkedReadTransfersAllBytes) {
    constexpr std::size_t kSize = 1024u * 1024u;
    std::string           payload(kSize, '\0');
    for (std::size_t i = 0; i < kSize; ++i)
        payload[i] = static_cast<char>('A' + (i % 26));

    qb::io::istream<ScriptedStreamTransport> input;
    input.transport() = ScriptedStreamTransport{payload};

    std::string assembled;
    assembled.reserve(kSize);

    int read_ops = 0;
    for (;;) {
        const int n = input.read();
        ASSERT_GE(n, 0) << "scripted read must never fail";
        if (n == 0)
            break; // EOF
        ++read_ops;
        // in().begin() is the start of the *valid* (unflushed) region; in().data()
        // is the raw buffer base and would re-read already-flushed bytes.
        assembled.append(input.in().begin(), input.in().size());
        input.flush(input.in().size());
    }

    EXPECT_GT(read_ops, 1) << "a 1 MiB payload must take more than one bucket read";
    ASSERT_EQ(assembled.size(), payload.size());
    EXPECT_EQ(assembled, payload);
}

/**
 * @test A scripted payload is written out through repeated partial writes with the exact wire image.
 * @brief Output companion to the large-read revival: a per-call write-limit script forces the
 *        ostream to drain a large buffer over many write() calls, and the transport's accumulated
 *        `written` must equal the published bytes exactly.
 */
TEST(StreamTemplates, LargeChunkedWriteEmitsExactWireImage) {
    constexpr std::size_t kSize = 64u * 1024u;
    std::string           payload(kSize, '\0');
    for (std::size_t i = 0; i < kSize; ++i)
        payload[i] = static_cast<char>('a' + (i % 26));

    // Cap every write() at 4 KiB so the drain genuinely spans many calls.
    std::vector<std::size_t> limits(64, 4096u);
    qb::io::ostream<ScriptedStreamTransport> output;
    output.transport() = ScriptedStreamTransport{"", std::move(limits)};

    ASSERT_NE(output.publish(payload.data(), payload.size()), nullptr);
    EXPECT_EQ(output.pendingWrite(), payload.size());

    int write_ops = 0;
    while (output.pendingWrite() > 0) {
        const int n = output.write();
        ASSERT_GT(n, 0);
        ++write_ops;
    }
    EXPECT_GT(write_ops, 1);
    EXPECT_EQ(output.transport().written, payload);
}

// =============================================================================
// REVIVED — buffer management / flush semantics (was DISABLED_StreamBufferManagement).
// =============================================================================

/**
 * @test flush() removes consumed bytes from the front and the remainder stays intact.
 * @brief Revives DISABLED_StreamBufferManagement deterministically against the scripted transport.
 *        The whole multi-line payload is delivered in one read; we extract and flush() line 1, then
 *        line 2, and assert the residual buffer still holds the later lines in order. The original
 *        relied on a second file read mid-way; here a single scripted read makes the buffer state
 *        fully deterministic.
 */
TEST(StreamTemplates, FlushConsumesFrontAndPreservesRemainder) {
    const std::string content = "Line 1\nLine 2\nLine 3\nLine 4\nLine 5\n";

    qb::io::istream<ScriptedStreamTransport> input;
    input.transport() = ScriptedStreamTransport{content};

    ASSERT_EQ(input.read(), static_cast<int>(content.size()));
    EXPECT_EQ(input.pendingRead(), content.size());

    // Extract and flush line 1. in().begin() is the start of the *valid* region
    // (it advances with flush()); in().data() is the raw buffer base and would
    // keep pointing at the already-flushed front.
    auto first_line_len = [&]() -> std::size_t {
        const char *data = input.in().begin();
        std::size_t  end = 0;
        while (end < input.in().size() && data[end] != '\n')
            ++end;
        return end + 1; // include the newline
    };

    std::size_t len1 = first_line_len();
    EXPECT_EQ(std::string(input.in().begin(), len1), "Line 1\n");
    input.flush(len1);

    // Line 2 is now at the front.
    std::size_t len2 = first_line_len();
    EXPECT_EQ(std::string(input.in().begin(), len2), "Line 2\n");
    input.flush(len2);

    // The remainder holds exactly lines 3-5, in order.
    const std::string remaining(input.in().begin(), input.in().size());
    EXPECT_EQ(remaining, "Line 3\nLine 4\nLine 5\n");
}
