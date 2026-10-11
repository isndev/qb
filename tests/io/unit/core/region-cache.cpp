/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/region-cache.cpp
 * @brief `qb::allocator::region_cache` and `region_allocator` — where an engine's per-core
 *        buffers live, so the next engine finds them mapped (Huly QB-1012).
 *
 * The cache (qb/system/allocator/region.h) hands out page-granular platform mappings, keeps a
 * released one mapped for the next request of the same rounded size, unmaps the ones released
 * longest ago once it holds more than twice the high water of what was lent, and unmaps all of
 * them on `trim()`. The allocator sends a request of at least 64 KiB there and a smaller one to
 * the global allocator. No engine here; the engine-level half is
 * `ActorArena.ASecondEngineBuildsItsPerCoreBuffersInTheRegionsTheFirstReleased`
 * (tests/core/system/engine/actor-arena.cpp) and the cost it removes is counted by
 * tests/core/system/engine/engine-restart-faults.cpp.
 *
 * The cache is process-wide: every case works on sizes no other case uses and judges deltas, so
 * the cases hold in any order; the one that needs an empty cache starts with `trim()`.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/system/allocator/region.h>

namespace {

using qb::allocator::region_allocator;
using qb::allocator::region_cache;
using qb::allocator::region_vector;

constexpr std::size_t kPage = region_cache::page_bytes;

/// A size of its own for each case (and each `k` within it): 100 pages and up, never 64 KiB.
constexpr std::size_t
size_of(std::size_t const case_id, std::size_t const k = 0) {
    return (100 + 10 * case_id + k) * kPage;
}

// The engine keeps these as members of a layout-sensitive object: an empty allocator and an empty
// deleter must cost nothing.
static_assert(sizeof(region_vector<int>) == sizeof(std::vector<int>));
static_assert(sizeof(qb::allocator::region_ptr<std::array<int, 4>>) == sizeof(void *));

} // namespace

TEST(RegionCache, AReleasedRegionIsTheNextAcquireOfItsSizeStillMapped) {
    std::size_t const bytes = size_of(1);
    void *const       first = region_cache::acquire(bytes);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(first) % kPage, 0u);
    std::memset(first, 0xA5, bytes); // every byte is writable
    region_cache::release(first, bytes);

    std::size_t const maps  = region_cache::mappings();
    void *const       again = region_cache::acquire(bytes);
    EXPECT_EQ(again, first);
    EXPECT_EQ(region_cache::mappings(), maps);
    EXPECT_EQ(static_cast<unsigned char *>(again)[bytes - 1], 0xA5); // what its last owner left
    region_cache::release(again, bytes);
}

TEST(RegionCache, SizesThatRoundAlikeShareARegion) {
    void *const first = region_cache::acquire(size_of(2) + 1); // rounds up to size_of(2) + one page
    region_cache::release(first, size_of(2) + 1);
    std::size_t const maps  = region_cache::mappings();
    void *const       again = region_cache::acquire(size_of(2) + kPage);
    EXPECT_EQ(again, first);
    EXPECT_EQ(region_cache::mappings(), maps);
    region_cache::release(again, size_of(2) + kPage);
    EXPECT_EQ(region_cache::rounded(1), kPage);
    EXPECT_EQ(region_cache::rounded(kPage), kPage);
    EXPECT_EQ(region_cache::rounded(kPage + 1), 2 * kPage);
}

TEST(RegionCache, AnotherSizeIsAFreshMapping) {
    void *const first = region_cache::acquire(size_of(3));
    region_cache::release(first, size_of(3));
    std::size_t const maps  = region_cache::mappings();
    void *const       other = region_cache::acquire(size_of(3, 1));
    EXPECT_NE(other, first);
    EXPECT_EQ(region_cache::mappings(), maps + 1);
    region_cache::release(other, size_of(3, 1));
}

TEST(RegionCache, TheCacheHoldsAtMostTwiceTheHighWaterAndUnmapsTheOldestFirst) {
    region_cache::trim(); // nothing cached, and the high water is what is lent now
    ASSERT_EQ(region_cache::lent_bytes(), 0u) << "a case left a region lent";
    ASSERT_EQ(region_cache::peak_bytes(), 0u);
    std::size_t const x = 140 * kPage, y = 70 * kPage, z = 105 * kPage; // none of them a size_of()

    region_cache::release(region_cache::acquire(x), x); // high water x, cached x
    EXPECT_EQ(region_cache::peak_bytes(), x);
    region_cache::release(region_cache::acquire(y), y); // cached x + y <= 2x
    EXPECT_EQ(region_cache::cached(), 2u);
    region_cache::release(region_cache::acquire(z), z); // x + y + z > 2x: x, the oldest, goes
    EXPECT_EQ(region_cache::cached(), 2u);
    EXPECT_EQ(region_cache::cached_bytes(), y + z);
    EXPECT_LE(region_cache::cached_bytes(), 2 * region_cache::peak_bytes());

    std::size_t const maps = region_cache::mappings();
    void *const       ry   = region_cache::acquire(y); // still cached
    EXPECT_EQ(region_cache::mappings(), maps);
    void *const rx = region_cache::acquire(x); // unmapped by the bound: a fresh mapping
    EXPECT_EQ(region_cache::mappings(), maps + 1);
    region_cache::release(rx, x);
    region_cache::release(ry, y);
    region_cache::trim();
}

TEST(RegionCache, TrimUnmapsEveryCachedRegionNeverALentOneAndForgetsTheHighWater) {
    std::size_t const kept = size_of(5), dropped = size_of(5, 1);
    void *const       lent = region_cache::acquire(kept);
    region_cache::release(region_cache::acquire(dropped), dropped);
    std::size_t const cached = region_cache::cached();
    ASSERT_GE(cached, 1u);

    EXPECT_EQ(region_cache::trim(), cached);
    EXPECT_EQ(region_cache::cached(), 0u);
    EXPECT_EQ(region_cache::cached_bytes(), 0u);
    EXPECT_EQ(region_cache::peak_bytes(), region_cache::lent_bytes());
    EXPECT_GE(region_cache::lent_bytes(), kept);
    std::memset(lent, 0x5A, kept); // the lent region is still mapped

    std::size_t const maps = region_cache::mappings();
    region_cache::release(region_cache::acquire(dropped), dropped); // it was unmapped: mapped again
    EXPECT_EQ(region_cache::mappings(), maps + 1);
    region_cache::release(lent, kept);
}

TEST(RegionCache, TheAllocatorSendsARequestToTheCacheFrom64KiBAndNoSmaller) {
    region_allocator<std::uint64_t> alloc;
    std::size_t const               lent0 = region_cache::lent_bytes();
    std::size_t const               below = region_cache::min_bytes / sizeof(std::uint64_t) - 1;
    std::uint64_t *const            small = alloc.allocate(below);
    EXPECT_EQ(region_cache::lent_bytes(), lent0); // the global allocator's
    alloc.deallocate(small, below);

    std::size_t const    at  = region_cache::min_bytes / sizeof(std::uint64_t);
    std::uint64_t *const big = alloc.allocate(at);
    EXPECT_EQ(region_cache::lent_bytes(), lent0 + region_cache::min_bytes);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(big) % kPage, 0u);
    alloc.deallocate(big, at);
    EXPECT_EQ(region_cache::lent_bytes(), lent0);
    EXPECT_TRUE(alloc == region_allocator<char>{});
}

TEST(RegionCache, ARegionVectorLendsExactlyTheBufferItHoldsAndKeepsItsElements) {
    std::size_t const     lent0 = region_cache::lent_bytes();
    constexpr std::size_t n     = 100000; // 800 KB: the doubling ladder crosses 64 KiB four times
    {
        region_vector<std::uint64_t> v;
        for (std::uint64_t i = 0; i < n; ++i) {
            v.push_back(i);
            std::size_t const bytes = v.capacity() * sizeof(std::uint64_t);
            ASSERT_EQ(region_cache::lent_bytes(), lent0 + (bytes >= region_cache::min_bytes ? region_cache::rounded(bytes) : 0))
                << "at " << v.size() << " elements";
        }
        for (std::uint64_t i = 0; i < n; ++i)
            ASSERT_EQ(v[i], i);
    }
    EXPECT_EQ(region_cache::lent_bytes(), lent0);
}

TEST(RegionCache, MakeRegionValueInitialisesTheObjectEvenInADirtyRegion) {
    using Block = std::array<std::uint64_t, region_cache::min_bytes / sizeof(std::uint64_t)>; // 64 KiB
    void *first = nullptr;
    {
        auto dirty = qb::allocator::make_region<Block>();
        first      = dirty.get();
        dirty->fill(~std::uint64_t{0});
    } // its region goes back to the cache as it is
    auto clean = qb::allocator::make_region<Block>();
    EXPECT_EQ(static_cast<void *>(clean.get()), first); // the same region, taken back
    EXPECT_TRUE(std::all_of(clean->begin(), clean->end(), [](std::uint64_t const v) { return v == 0; }));
}

TEST(RegionCache, ThreadsShareTheCache) {
    std::size_t const        lent0 = region_cache::lent_bytes();
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < 1000; ++i) {
                std::size_t const bytes  = size_of(9, t % 2); // two threads per size: they take each other's
                auto *const       region = static_cast<unsigned char *>(region_cache::acquire(bytes));
                region[0]                = static_cast<unsigned char>(i);
                region[bytes - 1]        = static_cast<unsigned char>(t);
                region_cache::release(region, bytes);
            }
        });
    for (auto &thread : threads)
        thread.join();
    EXPECT_EQ(region_cache::lent_bytes(), lent0);
}
