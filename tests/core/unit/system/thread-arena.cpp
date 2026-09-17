/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */
/**
 * @file tests/core/unit/system/thread-arena.cpp
 * @brief `qb::allocator::thread_arena` — the per-thread size-class arena the actor object
 *        lives in (Huly QB-212): rounding, LIFO reuse per class, the large fall-through, the
 *        over-aligned fall-through, chunk growth from the 64 KiB first chunk to `slab_cache`
 *        slabs, and the thread-exit teardown that hands the slabs back only when nothing is
 *        live.
 */

#include <gtest/gtest.h>
#include <qb/system/allocator/slab.h>
#include <qb/system/allocator/thread_arena.h>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using qb::allocator::slab_cache;
using qb::allocator::thread_arena;

namespace {

[[nodiscard]] bool
aligned_to(void const *const p, std::size_t const a) {
    return (reinterpret_cast<std::uintptr_t>(p) % a) == 0;
}

} // namespace

TEST(ThreadArena, EveryPooledBlockIsGranuleAlignedAndCounted) {
    const std::size_t before = thread_arena::live();
    void *const       a      = thread_arena::allocate(1);
    void *const       b      = thread_arena::allocate(thread_arena::granule);
    void *const       c      = thread_arena::allocate(thread_arena::granule + 1);
    void *const       d      = thread_arena::allocate(136); // the size of a typical derived actor
    void *const       e      = thread_arena::allocate(thread_arena::max_small);
    for (void *const p : {a, b, c, d, e}) {
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(aligned_to(p, thread_arena::granule));
    }
    EXPECT_EQ(thread_arena::live(), before + 5);
    EXPECT_GE(thread_arena::chunks(), 1u);
    thread_arena::deallocate(a, 1);
    thread_arena::deallocate(b, thread_arena::granule);
    thread_arena::deallocate(c, thread_arena::granule + 1);
    thread_arena::deallocate(d, 136);
    thread_arena::deallocate(e, thread_arena::max_small);
    EXPECT_EQ(thread_arena::live(), before);
    thread_arena::deallocate(nullptr, 136); // ignored
    EXPECT_EQ(thread_arena::live(), before);
}

TEST(ThreadArena, AFreedBlockIsTheNextOneOfItsClass) {
    void *const p = thread_arena::allocate(96);
    thread_arena::deallocate(p, 96);
    void *const q = thread_arena::allocate(96); // same class: LIFO reuse, the block still warm
    EXPECT_EQ(q, p);
    void *const r = thread_arena::allocate(136); // another class: never the 96-byte block
    EXPECT_NE(r, p);
    thread_arena::deallocate(q, 96);
    thread_arena::deallocate(r, 136);
}

TEST(ThreadArena, SizesRoundToTheSameClassShareAFreeList) {
    void *const p = thread_arena::allocate(90);
    thread_arena::deallocate(p, 90);
    void *const q = thread_arena::allocate(96); // 90 and 96 both round to the 96-byte class
    EXPECT_EQ(q, p);
    thread_arena::deallocate(q, 96);
}

TEST(ThreadArena, LargeBlocksFallThroughToTheGlobalAllocator) {
    const std::size_t live = thread_arena::live();
    void *const       p    = thread_arena::allocate(thread_arena::max_small + 1);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(thread_arena::live(), live); // not pooled, not counted
    thread_arena::deallocate(p, thread_arena::max_small + 1);
    void *const big = thread_arena::allocate(1u << 20);
    ASSERT_NE(big, nullptr);
    thread_arena::deallocate(big, 1u << 20);
    EXPECT_EQ(thread_arena::live(), live);
}

TEST(ThreadArena, OverAlignedBlocksAreServedAlignedAndNotPooled) {
    const std::size_t live = thread_arena::live();
    void *const       p    = thread_arena::allocate(200, 64);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(aligned_to(p, 64));
    EXPECT_EQ(thread_arena::live(), live);
    thread_arena::deallocate(p, 200, 64);
    void *const q = thread_arena::allocate(200, 16); // granule alignment: the pooled path
    EXPECT_EQ(thread_arena::live(), live + 1);
    thread_arena::deallocate(q, 200, 16);
    EXPECT_EQ(thread_arena::live(), live);
}

TEST(ThreadArena, GrowthGoesFromTheFirstChunkToSlabs) {
    // A fresh thread: its first chunk is 64 KiB from the global allocator, and the first
    // allocation the chunk cannot hold takes a 2 MB slab from the process-wide cache.
    std::size_t chunks_after_first = 0, bytes_after_first = 0, chunks_after_burst = 0, bytes_after_burst = 0;
    std::size_t live_after_frees = 1;
    std::thread([&] {
        std::vector<void *> blocks;
        blocks.push_back(thread_arena::allocate(1024));
        chunks_after_first = thread_arena::chunks();
        bytes_after_first  = thread_arena::chunk_bytes();
        for (int i = 0; i < 200; ++i) // 200 KiB of 1 KiB blocks: past the first chunk
            blocks.push_back(thread_arena::allocate(1024));
        chunks_after_burst = thread_arena::chunks();
        bytes_after_burst  = thread_arena::chunk_bytes();
        for (void *const p : blocks)
            thread_arena::deallocate(p, 1024);
        live_after_frees = thread_arena::live();
    }).join();
    EXPECT_EQ(chunks_after_first, 1u);
    EXPECT_EQ(bytes_after_first, thread_arena::first_chunk_bytes);
    EXPECT_GE(chunks_after_burst, 2u);
    EXPECT_EQ(bytes_after_burst, thread_arena::first_chunk_bytes + (chunks_after_burst - 1) * slab_cache::slab_bytes);
    EXPECT_EQ(live_after_frees, 0u);
}

/// Slabs the process holds outside the cache: what a leak would move.
[[nodiscard]] static std::size_t
slabs_in_use() noexcept {
    return slab_cache::mapped() - slab_cache::cached();
}

TEST(ThreadArena, AThreadThatFreedEverythingHandsItsSlabsBackToTheCacheAtExit) {
    const std::size_t in_use_before = slabs_in_use();
    std::size_t       slabs_taken   = 0;
    std::thread([&] {
        std::vector<void *> blocks;
        for (int i = 0; i < 300; ++i) // ~300 KiB: at least one slab beyond the first chunk
            blocks.push_back(thread_arena::allocate(1024));
        slabs_taken = thread_arena::chunks() - 1;
        for (void *const p : blocks)
            thread_arena::deallocate(p, 1024);
    }).join();
    ASSERT_GE(slabs_taken, 1u);
    // Released, not unmapped: nothing stays in use, and the next thread (or engine) that grows
    // takes the slabs back warm.
    EXPECT_EQ(slabs_in_use(), in_use_before);
    EXPECT_GE(slab_cache::cached(), slabs_taken);
}

TEST(ThreadArena, AThreadThatLeavesABlockLiveLeaksItsChunksInsteadOfRecyclingThem) {
    const std::size_t in_use_before = slabs_in_use();
    std::size_t       slabs_taken   = 0;
    std::thread([&] {
        std::vector<void *> blocks;
        for (int i = 0; i < 300; ++i)
            blocks.push_back(thread_arena::allocate(1024));
        slabs_taken = thread_arena::chunks() - 1;
        for (std::size_t i = 1; i < blocks.size(); ++i) // blocks[0] stays live past the thread
            thread_arena::deallocate(blocks[i], 1024);
    }).join();
    ASSERT_GE(slabs_taken, 1u);
    // The contract was broken (a block outlived its thread): the slabs are NOT back in the
    // cache -- still in use from its point of view -- so the live block can never be handed to
    // another thread through it.
    EXPECT_EQ(slabs_in_use(), in_use_before + slabs_taken);
}
