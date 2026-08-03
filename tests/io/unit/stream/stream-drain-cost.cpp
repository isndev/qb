/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/stream/stream-drain-cost.cpp
 * @brief Draining a large output buffer must cost O(bytes), not O(bytes x turns).
 *
 * `ostream::write()` / `stream::write()` hand the whole pending buffer to the transport and get
 * back however much the kernel accepted. The socket send buffer is finite, so any payload larger
 * than it — an HTTP body, a WebSocket message, a query result; the framework's own
 * `QB_MAX_WRITE_BUFFER_SIZE` default is 200 MB — is necessarily drained over many loop turns.
 *
 * The only sound way to retire the accepted prefix is to advance the read cursor: `free_front()`
 * is O(1), and `begin()`/`size()` stay correct at the new offset. Compacting the buffer on each
 * partial write instead relocates every byte still pending, on every turn, which turns one drain
 * into a quadratic memmove — and it happens on the event-loop thread, which in qb also runs the
 * VirtualCore's actors. Measured on the real `qb::allocator::pipe<char>` with a 64 KiB socket
 * window: 1 MB payload -> 8 MB moved, 16 MB -> 2 GB / 63 ms, 64 MB -> 32 GB / 728 ms.
 *
 * `qb::allocator::pipe` already reclaims the front, lazily and only when it pays: `allocate_back()`
 * compacts once `_begin` passes half the capacity, which bounds the buffer at ~2x the live bytes.
 * Eager compaction is not what makes the buffer bounded — it only makes the drain quadratic.
 *
 * Both cases below are structural and fully deterministic — no wall clock, no socket, no timing
 * threshold. `ScriptedStreamTransport`'s per-call write limits force the partial-write path one
 * call at a time, and the front offset (`out().begin() - out().data()`) states directly whether
 * the pending tail was relocated. The second case keeps a standing backlog precisely so that every
 * one of its turns is a *partial* write: a drain that empties the buffer takes the `reset()` branch
 * and never touches the code under test.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * @ingroup Tests
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/stream.h>

#include "../../shared/scripted_stream_transport.h"

using qb::io::test::ScriptedStreamTransport;

namespace {

constexpr std::size_t kChunk  = 4096u; ///< bytes the scripted "socket" accepts per write()
constexpr std::size_t kChunks = 64u;   ///< turns the drain spans

/// Offset of the still-pending region inside the raw buffer, i.e. how far the cursor has moved.
template <typename Stream>
[[nodiscard]] std::size_t
front_offset(Stream &s) noexcept {
    return static_cast<std::size_t>(s.out().begin() - s.out().data());
}

[[nodiscard]] std::string
make_payload(std::size_t size) {
    std::string p(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        p[i] = static_cast<char>('a' + (i % 26));
    return p;
}

} // namespace

/**
 * @test A partial write retires its prefix by advancing the cursor, never by moving the tail.
 *
 * With the cursor advancing, the front offset after k partial writes is exactly k * kChunk: no
 * `allocate_back()` runs during a pure drain, so nothing can compact behind our back. If the
 * buffer is compacted on every partial write the offset is pinned at 0 and this fails on the very
 * first turn — while the wire image stays correct either way, which is precisely why the defect is
 * invisible to a correctness-only test.
 */
TEST(StreamDrainCost, PartialWritesAdvanceTheCursorInsteadOfRelocatingThePendingTail) {
    const auto payload = make_payload(kChunk * kChunks);

    qb::io::ostream<ScriptedStreamTransport> output;
    output.transport() = ScriptedStreamTransport{"", std::vector<std::size_t>(kChunks, kChunk)};
    ASSERT_NE(output.publish(payload.data(), payload.size()), nullptr);
    ASSERT_EQ(front_offset(output), 0u);

    std::size_t sent = 0u;
    while (output.pendingWrite() > 0) {
        const int n = output.write();
        ASSERT_GT(n, 0);
        sent += static_cast<std::size_t>(n);

        if (output.pendingWrite() == 0u)
            break; // fully drained: the buffer is reset(), offset 0 is correct here

        ASSERT_EQ(front_offset(output), sent) << "after " << sent << " bytes accepted the pending tail sits at offset " << front_offset(output)
                                              << " instead of " << sent
                                              << ": the buffer is being compacted on every partial write, so each turn memmoves all "
                                                 "the bytes still queued and draining one payload costs O(bytes x turns)";
    }

    EXPECT_EQ(sent, payload.size());
    EXPECT_EQ(output.transport().written, payload) << "the wire image must stay byte-exact";
}

/**
 * @test The lazy policy keeps a steadily-streamed output buffer bounded, at a bounded cost.
 *
 * Boundedness is the property eager compaction would exist to provide, so it has to be proven
 * without it: a producer publishing while the socket drains must not grow the buffer without bound.
 * The pipe's own heuristic covers it — `allocate_back()` compacts once the retired front passes
 * half the capacity — so capacity must settle and then stay put, not creep up with the bytes moved.
 *
 * Boundedness alone does not discriminate — compacting on every turn is bounded too — so the same
 * run budgets the work as well. A standing backlog (`kBacklog` bytes the socket has not taken yet)
 * makes every single write() a *partial* one, and a turn whose front offset does not simply advance
 * by the accepted chunk is a turn that moved every live byte back to the front. With the cursor
 * advancing that happens only when the pipe itself compacts — which `allocate_back()` does once per
 * bufferful, not once per turn — so the bytes relocated stay under the bytes streamed. Compacting
 * on each partial write instead relocates the whole backlog on every one of the `kRounds` turns:
 * measured here, 128 MiB moved to stream 16 MiB, i.e. 8x the budget this test allows.
 */
TEST(StreamDrainCost, ASteadilyStreamedOutputBufferStaysBounded) {
    constexpr std::size_t kRounds  = 4096u;       ///< drain turns
    constexpr std::size_t kBacklog = 8u * kChunk; ///< bytes left queued behind the socket every turn
    const auto            chunk    = make_payload(kChunk);

    qb::io::ostream<ScriptedStreamTransport> output;
    output.transport() = ScriptedStreamTransport{"", std::vector<std::size_t>(kRounds, kChunk)};

    // Prime the backlog. Without it publish and drain cancel out exactly, write() takes the
    // `reset()` branch every turn and the partial-write path under test is never entered.
    for (std::size_t i = 0; i < kBacklog / kChunk; ++i)
        ASSERT_NE(output.publish(chunk.data(), chunk.size()), nullptr);

    std::size_t settled   = 0u;
    std::size_t relocated = 0u; // bytes memmoved by the pipe, summed over the run
    std::size_t offset    = front_offset(output);
    for (std::size_t round = 0; round < kRounds; ++round) {
        ASSERT_NE(output.publish(chunk.data(), chunk.size()), nullptr);
        ASSERT_EQ(output.write(), static_cast<int>(kChunk));
        ASSERT_EQ(output.pendingWrite(), kBacklog) << "round " << round << ": the socket took the whole buffer, so this turn never reached the "
                                                   << "partial-write path the test exists to measure";

        const auto now = front_offset(output);
        if (now != offset + kChunk) // the tail was moved back to the front instead of the cursor forward
            relocated += output.pendingWrite();
        offset = now;

        if (round == kRounds / 8u) {
            settled = output.out().capacity(); // past the initial growth
        } else if (round > kRounds / 8u) {
            // Braced deliberately: ASSERT_EQ expands to an if/else, so leaving it bare under this
            // `else if` is a -Wdangling-else error under GCC's -Werror (clang stays silent).
            ASSERT_EQ(output.out().capacity(), settled)
                << "the output buffer grew at round " << round << " (" << output.out().capacity() << " vs " << settled
                << " bytes) although publish and drain are perfectly matched: retiring bytes by "
                   "advancing the cursor is leaking capacity instead of being reclaimed by "
                   "allocate_back()'s compaction";
        }
    }

    ASSERT_GT(settled, 0u);
    EXPECT_LE(settled, 8u * kBacklog) << "steady-state capacity should stay a small multiple of the live bytes";
    EXPECT_LE(relocated, kRounds * kChunk)
        << "draining " << (kRounds * kChunk) << " bytes moved " << relocated
        << " bytes of still-pending tail: the buffer is being compacted on (nearly) every partial write, so the "
           "cost of one drain grows with the number of turns it spans instead of with the bytes it carries";

    ASSERT_EQ(output.transport().written.size(), kRounds * kChunk);
    const std::string_view wire{output.transport().written};
    for (std::size_t i = 0; i < kRounds; ++i)
        ASSERT_EQ(wire.substr(i * kChunk, kChunk), chunk) << "the wire image diverged at chunk " << i;
}
