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
