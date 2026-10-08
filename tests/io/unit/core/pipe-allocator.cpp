/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/pipe-allocator.cpp
 * @brief `qb::allocator::pipe<T>` — the framework's growable contiguous byte/element buffer.
 *
 * `pipe<T>` (qb/system/allocator/pipe.h) is the backing store under every qb-io stream: it appends
 * at the back, releases consumed bytes from the front (`free_front`), and reorders/grows on demand.
 * Pure in-memory data structure — NO socket, NO event loop — so this is a strict `unit` test.
 *
 * Migrated wholesale from system/test-io.cpp::PipeRegression.* and PipeRobustness.* (spec D4):
 *   - the regression cluster pins the `free_front`-offset bugs (copy/assign/multi-free copied from
 *     the wrong base offset, leaking the freed prefix);
 *   - the robustness cluster covers the empty pipe, swap, resize grow/shrink, reorder-after-free,
 *     every typed `put` overload, reserve-keeps-size, the bad_alloc overflow guard, and move
 *     construct/assign (with the moved-from source left empty).
 *
 * No assertion was weak here; the work is the move from a 1339-LOC catch-all to a focused file.
 */

#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <qb/system/allocator/pipe.h>

// =============================================================================
// REGRESSION: free_front offset bugs (copy / assign / multi-free)
// =============================================================================

/**
 * @test Copying a pipe after `free_front` copies the LIVE bytes, not the freed prefix.
 * @brief Regression: the copy ctor read from the buffer base instead of the post-free offset, so
 *        the copy resurrected the consumed prefix.
 */
TEST(PipeAllocatorRegression, CopyAfterFreeFront) {
    qb::allocator::pipe<char> src;
    src.put("GARBAGE_PREFIX_HELLO_WORLD", 26);
    src.free_front(15);
    ASSERT_EQ(src.size(), 11u);
    EXPECT_EQ(src.view(), "HELLO_WORLD");

    qb::allocator::pipe<char> dst(src);
    EXPECT_EQ(dst.size(), 11u);
    EXPECT_EQ(dst.view(), "HELLO_WORLD");
}

/**
 * @test Copy-ASSIGNING a pipe after `free_front` overwrites the target with the live bytes only.
 */
TEST(PipeAllocatorRegression, AssignAfterFreeFront) {
    qb::allocator::pipe<char> src;
    src.put("PREFIX_DATA_PAYLOAD", 19);
    src.free_front(12);
    ASSERT_EQ(src.view(), "PAYLOAD");

    qb::allocator::pipe<char> dst;
    dst.put("overwritten", 11);
    dst = src;

    EXPECT_EQ(dst.size(), 7u);
    EXPECT_EQ(dst.view(), "PAYLOAD");
}

/**
 * @test The offset is tracked across MULTIPLE `free_front` calls before a copy.
 */
TEST(PipeAllocatorRegression, CopyAfterMultipleFreeFronts) {
    qb::allocator::pipe<char> p;
    for (int i = 0; i < 5; ++i)
        p.put("ABCDEFGHIJ", 10);
    p.free_front(40);
    ASSERT_EQ(p.size(), 10u);
    EXPECT_EQ(p.view(), "ABCDEFGHIJ");

    auto copy = p;
    EXPECT_EQ(copy.size(), 10u);
    EXPECT_EQ(copy.view(), "ABCDEFGHIJ");
}

// =============================================================================
// ROBUSTNESS: empty / swap / resize / reorder
// =============================================================================

TEST(PipeAllocatorRobustness, EmptyPipeOperations) {
    qb::allocator::pipe<char> p;
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.size(), 0u);
    EXPECT_EQ(p.begin(), p.end());
    EXPECT_EQ(p.view(), "");
    EXPECT_EQ(p.str(), "");
    EXPECT_GT(p.capacity(), 0u) << "a default pipe pre-reserves capacity";
}

TEST(PipeAllocatorRobustness, SwapExchangesContentsAndSizes) {
    qb::allocator::pipe<int> a;
    int                      vals_a[] = {1, 2, 3};
    a.put(vals_a, 3);

    qb::allocator::pipe<int> b;
    int                      vals_b[] = {10, 20, 30, 40};
    b.put(vals_b, 4);

    a.swap(b);
    EXPECT_EQ(a.size(), 4u);
    EXPECT_EQ(a.begin()[0], 10);
    EXPECT_EQ(a.begin()[3], 40);
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(b.begin()[0], 1);
    EXPECT_EQ(b.begin()[2], 3);
}

TEST(PipeAllocatorRobustness, SwapBothNonEmpty) {
    qb::allocator::pipe<int> a;
    int                      vals_a[] = {1, 2, 3, 4, 5};
    a.put(vals_a, 5);

    qb::allocator::pipe<int> b;
    int                      vals_b[] = {100, 200};
    b.put(vals_b, 2);

    a.swap(b);
    EXPECT_EQ(a.size(), 2u);
    EXPECT_EQ(a.begin()[0], 100);
    EXPECT_EQ(a.begin()[1], 200);
    EXPECT_EQ(b.size(), 5u);
    EXPECT_EQ(b.begin()[0], 1);
    EXPECT_EQ(b.begin()[4], 5);
}

TEST(PipeAllocatorRobustness, ResizeGrowKeepsExistingBytes) {
    qb::allocator::pipe<char> p;
    p.put("ABC", 3);
    EXPECT_EQ(p.size(), 3u);

    p.resize(10);
    EXPECT_EQ(p.size(), 10u);
    EXPECT_EQ(std::string_view(p.begin(), 3), "ABC");
}

TEST(PipeAllocatorRobustness, ResizeShrinkTruncates) {
    qb::allocator::pipe<char> p;
    p.put("ABCDEFGHIJ", 10);
    EXPECT_EQ(p.size(), 10u);

    p.resize(5);
    EXPECT_EQ(p.size(), 5u);
    EXPECT_EQ(std::string_view(p.begin(), 5), "ABCDE");
}

TEST(PipeAllocatorRobustness, ReorderAfterFreeFrontCompactsLiveBytes) {
    qb::allocator::pipe<char> p;
    p.put("HEADERPAYLOAD", 13);
    p.free_front(6);
    EXPECT_EQ(p.view(), "PAYLOAD");

    p.reorder();
    EXPECT_EQ(p.view(), "PAYLOAD");
    EXPECT_EQ(p.size(), 7u);
}

// =============================================================================
// ROBUSTNESS: typed put overloads
// =============================================================================

TEST(PipeAllocatorRobustness, PutStringView) {
    qb::allocator::pipe<char> p;
    const std::string_view    sv = "hello from string_view";
    p.put(sv);
    EXPECT_EQ(p.view(), sv);
}

TEST(PipeAllocatorRobustness, PutCString) {
    qb::allocator::pipe<char> p;
    p.put("c-string data");
    EXPECT_EQ(p.view(), "c-string data");
}

TEST(PipeAllocatorRobustness, PutStdString) {
    qb::allocator::pipe<char> p;
    const std::string         s = "std::string content";
    p.put(s);
    EXPECT_EQ(p.view(), s);
}

TEST(PipeAllocatorRobustness, PutPipe) {
    qb::allocator::pipe<char> src;
    src.put("source_pipe_data", 16);

    qb::allocator::pipe<char> dst;
    dst.put(src);
    EXPECT_EQ(dst.view(), "source_pipe_data");
}

TEST(PipeAllocatorRobustness, PutPipeAfterFreeFrontCopiesLiveBytes) {
    qb::allocator::pipe<char> src;
    src.put("GARBAGE_REAL_DATA", 17);
    src.free_front(8);

    qb::allocator::pipe<char> dst;
    dst.put(src);
    EXPECT_EQ(dst.view(), "REAL_DATA");
}

// =============================================================================
// ROBUSTNESS: reserve / overflow / move
// =============================================================================

TEST(PipeAllocatorRobustness, ReserveDoesNotChangeSize) {
    qb::allocator::pipe<char> p;
    p.put("data", 4);
    const auto old_size = p.size();
    p.reserve(1000);
    EXPECT_EQ(p.size(), old_size);
    EXPECT_EQ(p.view(), "data");
}

TEST(PipeAllocatorRobustness, AllocateBackOverflowThrows) {
    qb::allocator::pipe<char> p;
    EXPECT_THROW(p.allocate_back(std::numeric_limits<std::size_t>::max()), std::bad_alloc);
}

TEST(PipeAllocatorRobustness, MoveConstructLeavesSourceEmpty) {
    qb::allocator::pipe<char> src;
    src.put("MOVE_ME", 7);

    qb::allocator::pipe<char> dst(std::move(src));
    EXPECT_EQ(dst.view(), "MOVE_ME");
    EXPECT_EQ(dst.size(), 7u);
    EXPECT_TRUE(src.empty());
}

TEST(PipeAllocatorRobustness, MoveAssignLeavesSourceEmpty) {
    qb::allocator::pipe<char> src;
    src.put("MOVE_ASSIGN", 11);

    qb::allocator::pipe<char> dst;
    dst.put("OLD_DATA", 8);
    dst = std::move(src);

    EXPECT_EQ(dst.view(), "MOVE_ASSIGN");
    EXPECT_TRUE(src.empty());
}

/**
 * @test A self-move-assignment leaves the pipe as it was.
 * @brief It used to free the buffer and then take "rhs"'s fields -- its own, now pointing at the freed
 *        block: the next read was a use-after-free (ASan), the destructor a double free.
 */
TEST(PipeAllocatorRobustness, SelfMoveAssignKeepsTheContents) {
    qb::allocator::pipe<char> p;
    p.put("SELF_MOVE", 9);
    auto &alias = p; // through a reference: a direct `p = std::move(p)` draws -Wself-move
    p           = std::move(alias);
    EXPECT_EQ(p.view(), "SELF_MOVE");
    p.put("+more", 5);
    EXPECT_EQ(p.view(), "SELF_MOVE+more");
}

// =============================================================================
// SELF-ALIAS: appending a view of the pipe to itself (Huly QB-277)
// =============================================================================
//
// The source of a put() may lie inside the pipe it appends to. allocate_back() then moves the live
// bytes on its slow path, and a copy that read the source where it USED to be read moved or freed
// memory: after a growth, the freed allocation (ASan: heap-use-after-free); after a compaction, the
// bytes' old place, which the appended span overlaps (ASan: memcpy-param-overlap). A release build
// usually copies the right bytes anyway -- a compaction never overwrites the old place, a freed block
// keeps its content -- which is why the sanitizer is the witness. Each case is driven into one regime
// and asserts the regime was reached, so a test cannot pass by missing it.

namespace pipe_allocator_test {

// `n` distinct, position-dependent bytes: a copy from the wrong offset cannot reproduce them.
std::string
pattern(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>('A' + (i * 7u) % 26u);
    return s;
}

// A pipe whose live bytes are `live` and start past the middle of a FULL buffer: the next append of
// fewer than capacity/2 bytes must take the reorder path (no room after _end, _begin > capacity/2).
// With `live` just under capacity/2 the appended span overlaps the bytes' old place.
qb::allocator::pipe<char>
pipe_set_for_reorder(std::string const &live) {
    qb::allocator::pipe<char> p;
    const auto                cap = p.capacity();
    p.put(pattern(cap - live.size()).data(), cap - live.size());
    p.put(live.data(), live.size());
    p.free_front(cap - live.size());
    return p;
}

} // namespace pipe_allocator_test

TEST(PipeAllocatorSelfAlias, AppendingAViewOfItselfThatFitsCopiesTheView) {
    qb::allocator::pipe<char> p;
    p.put("0123456789", 10);
    const char *const before = p.begin();
    p.put(std::string_view(p.begin() + 2, 4)); // "2345", with room: the fast path
    ASSERT_EQ(p.begin(), before) << "the fast path must not have moved anything";
    EXPECT_EQ(p.view(), "01234567892345");
}

TEST(PipeAllocatorSelfAlias, AppendingAViewOfItselfDuringAReorderCopiesTheView) {
    using namespace pipe_allocator_test;
    qb::allocator::pipe<char> probe;
    const auto                live = pattern(probe.capacity() / 2 - 52);
    auto                      p    = pipe_set_for_reorder(live);
    const auto                cap  = p.capacity();
    ASSERT_EQ(p.view(), live);

    p.put(std::string_view(p.begin() + 4, live.size() - 6)); // < cap / 2 and no room after _end: reorder
    ASSERT_EQ(p.capacity(), cap) << "the append must have compacted in place, not grown";
    EXPECT_EQ(p.view(), live + live.substr(4, live.size() - 6)) << "the view was read where it used to be";
}

TEST(PipeAllocatorSelfAlias, AppendingAViewOfItselfDuringAGrowthCopiesTheView) {
    using namespace pipe_allocator_test;
    qb::allocator::pipe<char> p;
    const auto                cap  = p.capacity();
    const auto                full = pattern(cap);
    p.put(full.data(), full.size()); // full, _begin == 0: the next append must grow

    p.put(std::string_view(p.begin(), cap)); // the whole pipe, appended to itself
    ASSERT_GT(p.capacity(), cap) << "the append must have grown the buffer";
    EXPECT_EQ(p.view(), full + full) << "the view was read from the freed allocation";
}

TEST(PipeAllocatorSelfAlias, AppendingThePipeToItselfCopiesItsContents) {
    using namespace pipe_allocator_test;
    {
        qb::allocator::pipe<char> probe;
        const auto                live = pattern(probe.capacity() / 2 - 52);
        auto                      p    = pipe_set_for_reorder(live);
        p.put(p); // put<pipe<char>>: size and source must be read before the allocation
        ASSERT_EQ(p.capacity(), probe.capacity()) << "the append must have compacted in place";
        EXPECT_EQ(p.view(), live + live) << "reorder regime";
    }
    {
        qb::allocator::pipe<char> p;
        const auto                full = pattern(p.capacity());
        p.put(full.data(), full.size());
        p.put(p);
        EXPECT_EQ(p.view(), full + full) << "growth regime";
    }
}

TEST(PipeAllocatorSelfAlias, RawPointerAndRangeAppendsOfItselfCopyTheBytes) {
    using namespace pipe_allocator_test;
    qb::allocator::pipe<char> probe;
    const auto                live = pattern(probe.capacity() / 2 - 52);
    const auto                n    = live.size() - 16;
    {
        auto p = pipe_set_for_reorder(live);
        p.put(p.begin() + 4, n); // put(char const *, size)
        EXPECT_EQ(p.view(), live + live.substr(4, n)) << "put(data, size)";
    }
    {
        auto p = pipe_set_for_reorder(live);
        p.write(p.begin() + 4, n);
        EXPECT_EQ(p.view(), live + live.substr(4, n)) << "write(data, size)";
    }
    {
        auto p = pipe_set_for_reorder(live);
        p.put(static_cast<char const *>(p.begin() + 4), static_cast<char const *>(p.begin() + 4 + n)); // put(first, last)
        EXPECT_EQ(p.view(), live + live.substr(4, n)) << "put(first, last)";
    }
}

TEST(PipeAllocatorSelfAlias, TypedPipeAppendsOfItselfCopyTheElements) {
    // The generic pipe<T>: put(data, size) and recycle_back(element) with a source inside the pipe.
    qb::allocator::pipe<int> p;
    const auto               cap = p.capacity();
    for (std::size_t i = 0; i < cap; ++i)
        p.allocate_back(1)[0] = static_cast<int>(i);
    p.put(p.begin(), 2); // full: grows
    ASSERT_GT(p.capacity(), cap);
    ASSERT_EQ(p.size(), cap + 2);
    EXPECT_EQ(p.begin()[cap], 0);
    EXPECT_EQ(p.begin()[cap + 1], 1);

    const auto before = p.capacity();
    while (p.size() < before) // fill to the brim again so recycle_back must grow too
        p.allocate_back(1)[0] = 7;
    p.recycle_back(p.begin()[1]);
    ASSERT_GT(p.capacity(), before);
    EXPECT_EQ(p.begin()[p.size() - 1], 1) << "recycle_back read its element after the growth freed it";
}

// =============================================================================
// CONTRACT: allocate_back INVALIDATES every previously returned pointer
// =============================================================================

/**
 * @test A pointer handed out by `allocate_back` is invalidated by the next `allocate_back` that
 *       has to grow the buffer.
 * @brief The contract of the CONTIGUOUS pipe — the one `qb::io` streams and `pipe<char>` users
 *        rely on: the buffer is a single allocation that grows by reallocating and memcpying, so
 *        every earlier pointer dangles after a growth. It is NOT the contract behind
 *        `qb::Actor::push` / `qb::Pipe::push` any more: since the event pipes moved to
 *        `qb::allocator::segmented_pipe` (3.2), a queued event's reference stays valid until the
 *        handler or callback that obtained it returns, whatever is pushed meanwhile — that
 *        contract is pinned by `SegmentedPipeContract.*` (`segmented-pipe.cpp`) and
 *        `PushReferenceStability.*` (`core/system/messaging/push-reference-stability.cpp`).
 *
 *        Only addresses are compared — the stale pointer is never dereferenced — so the test
 *        proves the hazard without itself committing the UB it documents.
 */
TEST(PipeAllocatorContract, AllocateBackGrowthInvalidatesEarlierPointers) {
    qb::allocator::pipe<char> p;
    const auto                cap0  = p.capacity();
    char *const               first = p.allocate_back(8);
    ASSERT_NE(first, nullptr);

    // Force at least one growth: request strictly more than the initial capacity can hold.
    (void) p.allocate_back(cap0 + 1);
    ASSERT_GT(p.capacity(), cap0) << "the pipe must have grown for this test to prove anything";

    EXPECT_TRUE(first < p.begin() || first >= p.begin() + p.capacity())
        << "after growth the earlier pointer no longer aliases the live buffer — any event "
           "reference obtained from a previous push<>() is dangling at this point";
}

/**
 * @test `reorder()` (the in-place compaction `allocate_back` performs instead of growing when the
 *       freed prefix is large enough) also invalidates earlier pointers.
 * @brief The nastier half of the contiguous contract: compaction keeps the SAME buffer and
 *        memmoves the live bytes down, so a stale pointer stays inside the allocation and silently
 *        aliases DIFFERENT data. No allocator debugger can see that — only this contract can. (It
 *        is one of the two reasons the event pipes no longer use this allocator: a segment is
 *        never compacted, so an event never moves once queued.)
 */
TEST(PipeAllocatorContract, ReorderInvalidatesEarlierPointersWithoutFreeingTheBuffer) {
    qb::allocator::pipe<char> p;
    p.put("0123456789", 10);
    p.free_front(6); // live bytes now start at offset 6
    char *const before = p.begin();
    ASSERT_EQ(p.view(), "6789");

    p.reorder(); // compacts the live bytes back to offset 0 — same buffer, moved contents
    EXPECT_EQ(p.view(), "6789");
    EXPECT_NE(before, p.begin()) << "reorder moved the live bytes: a pointer captured before it "
                                    "now aliases stale/other data inside a still-live allocation";
}
