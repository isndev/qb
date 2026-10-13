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
 * @brief `qb::allocator::region_cache` and `region_allocator` -- where an engine's per-core
 *        buffers live, so the next engine finds them mapped (Huly QB-1012).
 *
 * The cache (qb/system/allocator/region.h) hands out page-granular platform mappings; a region a
 * container gives back when it dies stays mapped for the next request of its rounded size, a
 * region a container outgrows is unmapped at once, a container that grows takes the largest
 * cached region it can use, the cache holds at most twice the high water of what was lent and
 * unmaps the oldest first past it, and `trim()` unmaps every cached region. A lookup files and finds
 * regions by size (Huly QB-203): a size is found under any number of newer regions of others, the
 * regions of one size are lent newest first, the bound cuts the oldest region of a size and keeps
 * its newer ones, and the table of sizes grows past a page. A RESERVED region (`region_acquire_reserved`,
 * QB-203 -- the engine's mailbox blocks on Windows) is lent again as a reserved region, what was
 * committed in it kept, and on Windows a reserved region and a committed one of the same size are
 * never lent for each other: POSIX has the one kind. The allocator makes a
 * request of at least 64 KiB a region and gives a smaller one to the global allocator. No engine
 * here; the engine-level half is `ActorArena.AnEngineCachesNothingWhileItRunsAndTheNextOneMapsNothing`
 * (tests/core/system/engine/actor-arena.cpp) and the cost it removes is counted by
 * tests/core/system/engine/engine-restart-faults.cpp.
 *
 * The cache is process-wide: every case works on sizes no other case uses and judges deltas, so
 * the cases hold in any order and under `--gtest_repeat`; a case that needs a size not to be
 * cached, or the high water reset, starts with `trim()`.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/system/allocator/region.h>

namespace {

using qb::allocator::region_allocator;
using qb::allocator::region_cache;
using qb::allocator::region_vector;
namespace detail = qb::allocator::detail;

constexpr std::size_t kPage = detail::region_page_bytes;

/// A size of its own for each case (and each `k` within it): 100 pages and up, never 64 KiB.
constexpr std::size_t
size_of(std::size_t const case_id, std::size_t const k = 0) {
    return (100 + 10 * case_id + k) * kPage;
}

[[nodiscard]] std::size_t
mappings() {
    return detail::region_stats().mappings;
}

// The engine keeps these as members of a layout-sensitive object: an empty allocator and an empty
// deleter must cost nothing, and a region_stack is the vector it extends.
static_assert(sizeof(region_vector<int>) == sizeof(std::vector<int>));
static_assert(sizeof(detail::region_stack<int>) == sizeof(std::vector<int>));
static_assert(sizeof(qb::allocator::region_ptr<std::array<int, 4>>) == sizeof(void *));

} // namespace

TEST(RegionCache, AReleasedRegionIsTheNextAcquireOfItsSizeStillMapped) {
    std::size_t const bytes = size_of(1);
    void *const       first = detail::region_acquire(bytes);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(first) % kPage, 0u);
    std::memset(first, 0xA5, bytes); // every byte is writable
    detail::region_release(first, bytes);

    std::size_t const maps  = mappings();
    void *const       again = detail::region_acquire(bytes);
    EXPECT_EQ(again, first);
    EXPECT_EQ(mappings(), maps);
    EXPECT_EQ(static_cast<unsigned char *>(again)[bytes - 1], 0xA5); // what its last owner left
    detail::region_release(again, bytes);
}

TEST(RegionCache, SizesThatRoundAlikeShareARegion) {
    void *const first = detail::region_acquire(size_of(2) + 1); // rounds up to size_of(2) + one page
    detail::region_release(first, size_of(2) + 1);
    std::size_t const maps  = mappings();
    void *const       again = detail::region_acquire(size_of(2) + kPage);
    EXPECT_EQ(again, first);
    EXPECT_EQ(mappings(), maps);
    detail::region_release(again, size_of(2) + kPage);
    EXPECT_EQ(detail::region_rounded(1), kPage);
    EXPECT_EQ(detail::region_rounded(kPage), kPage);
    EXPECT_EQ(detail::region_rounded(kPage + 1), 2 * kPage);
}

TEST(RegionCache, AnotherSizeIsAFreshMapping) {
    region_cache::trim(); // an earlier repetition of this case left both sizes cached
    void *const first = detail::region_acquire(size_of(3));
    detail::region_release(first, size_of(3));
    std::size_t const maps  = mappings();
    void *const       other = detail::region_acquire(size_of(3, 1));
    EXPECT_NE(other, first);
    EXPECT_EQ(mappings(), maps + 1);
    detail::region_release(other, size_of(3, 1));
}

TEST(RegionCache, TheCacheHoldsAtMostTwiceTheHighWaterAndUnmapsTheOldestFirst) {
    region_cache::trim(); // nothing cached, and the high water is what is lent now
    ASSERT_EQ(detail::region_stats().lent_bytes, 0u) << "a case left a region lent";
    ASSERT_EQ(detail::region_stats().peak_bytes, 0u);
    std::size_t const x = 140 * kPage, y = 70 * kPage, z = 105 * kPage; // none of them a size_of()

    detail::region_release(detail::region_acquire(x), x); // high water x, cached x
    EXPECT_EQ(detail::region_stats().peak_bytes, x);
    detail::region_release(detail::region_acquire(y), y); // cached x + y <= 2x
    EXPECT_EQ(detail::region_stats().cached, 2u);
    detail::region_release(detail::region_acquire(z), z); // x + y + z > 2x: x, the oldest, goes
    EXPECT_EQ(detail::region_stats().cached, 2u);
    EXPECT_EQ(detail::region_stats().cached_bytes, y + z);
    EXPECT_LE(detail::region_stats().cached_bytes, 2 * detail::region_stats().peak_bytes);

    std::size_t const maps = mappings();
    void *const       ry   = detail::region_acquire(y); // still cached
    EXPECT_EQ(mappings(), maps);
    void *const rx = detail::region_acquire(x); // unmapped by the bound: a fresh mapping
    EXPECT_EQ(mappings(), maps + 1);
    detail::region_release(rx, x);
    detail::region_release(ry, y);
    region_cache::trim();
}

TEST(RegionCache, TrimUnmapsEveryCachedRegionNeverALentOneAndForgetsTheHighWater) {
    std::size_t const kept = size_of(5), dropped = size_of(5, 1);
    void *const       lent = detail::region_acquire(kept);
    detail::region_release(detail::region_acquire(dropped), dropped);
    std::size_t const cached = detail::region_stats().cached;
    ASSERT_GE(cached, 1u);

    EXPECT_EQ(region_cache::trim(), cached);
    EXPECT_EQ(detail::region_stats().cached, 0u);
    EXPECT_EQ(detail::region_stats().cached_bytes, 0u);
    EXPECT_EQ(detail::region_stats().peak_bytes, detail::region_stats().lent_bytes);
    EXPECT_GE(detail::region_stats().lent_bytes, kept);
    std::memset(lent, 0x5A, kept); // the lent region is still mapped

    std::size_t const maps = mappings();
    detail::region_release(detail::region_acquire(dropped), dropped); // it was unmapped: mapped again
    EXPECT_EQ(mappings(), maps + 1);
    detail::region_release(lent, kept);
}

TEST(RegionCache, ARegionAContainerOutgrowsIsUnmappedNotCached) {
    region_cache::trim(); // what earlier cases left: past the bound, the last release would push one of them out
    std::size_t const bytes  = size_of(6);
    void *const       region = detail::region_acquire(bytes);
    std::size_t const before = detail::region_stats().cached_bytes;
    std::size_t const lent   = detail::region_stats().lent_bytes;
    {
        detail::region_outgrow const scope; // what a growth leaves behind
        detail::region_release(region, bytes);
    }
    EXPECT_EQ(detail::region_stats().cached_bytes, before);     // not cached...
    EXPECT_EQ(detail::region_stats().lent_bytes, lent - bytes); // ...and no longer lent: unmapped
    std::size_t const maps = mappings();
    detail::region_release(detail::region_acquire(bytes), bytes); // nothing of that size waits: a fresh mapping
    EXPECT_EQ(mappings(), maps + 1);
    EXPECT_EQ(detail::region_stats().cached_bytes, before + bytes); // outside a growth, a release is cached again
}

TEST(RegionCache, AGrowingContainerTakesTheLargestCachedRegionItCanUse) {
    region_cache::trim();
    std::size_t const small = size_of(7), large = size_of(7, 5), too_large = size_of(7, 9);
    // All three lent at once, then released: the high water is their sum, so the cache's bound keeps
    // every one of them (released one by one, the third would have pushed the first out).
    void *const s = detail::region_acquire(small);
    void *const l = detail::region_acquire(large);
    void *const t = detail::region_acquire(too_large);
    detail::region_release(s, small);
    detail::region_release(l, large);
    detail::region_release(t, too_large);
    EXPECT_EQ(detail::region_largest_cached(kPage, large), large);
    EXPECT_EQ(detail::region_largest_cached(small, too_large), too_large);
    EXPECT_EQ(detail::region_largest_cached(too_large + kPage, too_large * 2), 0u);

    // A vector of 8-byte elements that must grow to at least `small` bytes, at most `large`: it
    // reserves the whole of the cached `large` region, and that region is what it gets.
    region_vector<std::uint64_t> v;
    std::size_t const            maps = mappings();
    detail::region_reserve(v, small / sizeof(std::uint64_t), large / sizeof(std::uint64_t));
    EXPECT_EQ(v.capacity(), large / sizeof(std::uint64_t));
    EXPECT_EQ(mappings(), maps); // the cached region, not a fresh mapping
    EXPECT_EQ(detail::region_stats().cached, 2u);

    // Below 64 KiB nothing is asked of the cache: plain doubling.
    region_vector<std::uint64_t> w;
    detail::region_reserve(w, 100, 1u << 20);
    EXPECT_EQ(w.capacity(), 100u);
    detail::region_reserve(w, 101, 1u << 20);
    EXPECT_EQ(w.capacity(), 200u);
    region_cache::trim();
}

TEST(RegionCache, ARegionStackClimbsWithoutCachingItsRungs) {
    region_cache::trim();
    std::size_t const lent0 = detail::region_stats().lent_bytes;
    {
        detail::region_stack<std::uint64_t> stack;
        for (std::uint64_t i = 0; i < 100000; ++i) // 800 KB: the ladder crosses 64 KiB four times
            stack.push_back(i);
        EXPECT_EQ(detail::region_stats().cached_bytes, 0u); // every outgrown rung was unmapped
        EXPECT_EQ(detail::region_stats().lent_bytes, lent0 + detail::region_rounded(stack.capacity() * sizeof(std::uint64_t)));
        for (std::uint64_t i = 0; i < 100000; ++i)
            ASSERT_EQ(stack[i], i);
    } // its final buffer is cached
    EXPECT_EQ(detail::region_stats().cached, 1u);
    region_cache::trim();
}

TEST(RegionCache, TheAllocatorSendsARequestToTheCacheFrom64KiBAndNoSmaller) {
    region_allocator<std::uint64_t> alloc;
    std::size_t const               lent0 = detail::region_stats().lent_bytes;
    std::size_t const               below = detail::region_min_bytes / sizeof(std::uint64_t) - 1;
    std::uint64_t *const            small = alloc.allocate(below);
    EXPECT_EQ(detail::region_stats().lent_bytes, lent0); // the global allocator's
    alloc.deallocate(small, below);

    std::size_t const    at  = detail::region_min_bytes / sizeof(std::uint64_t);
    std::uint64_t *const big = alloc.allocate(at);
    EXPECT_EQ(detail::region_stats().lent_bytes, lent0 + detail::region_min_bytes);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(big) % kPage, 0u);
    alloc.deallocate(big, at);
    EXPECT_EQ(detail::region_stats().lent_bytes, lent0);
    EXPECT_TRUE(alloc == region_allocator<char>{});
}

TEST(RegionCache, MakeRegionValueInitialisesTheObjectEvenInADirtyRegion) {
    using Block = std::array<std::uint64_t, detail::region_min_bytes / sizeof(std::uint64_t)>; // 64 KiB
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
    std::size_t const        lent0 = detail::region_stats().lent_bytes;
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < 1000; ++i) {
                std::size_t const bytes  = size_of(9, t % 2); // two threads per size: they take each other's
                auto *const       region = static_cast<unsigned char *>(detail::region_acquire(bytes));
                region[0]                = static_cast<unsigned char>(i);
                region[bytes - 1]        = static_cast<unsigned char>(t);
                detail::region_release(region, bytes);
            }
        });
    for (auto &thread : threads)
        thread.join();
    EXPECT_EQ(detail::region_stats().lent_bytes, lent0);
}

// ---- the size lists: a lookup finds its size however many regions of others are cached (Huly QB-203) -------------------

TEST(RegionCache, ASizeIsFoundUnderManyNewerRegionsOfAnotherAndEachSizeLendsItsNewestFirst) {
    region_cache::trim();
    constexpr std::size_t kMany = 256; // more than the 240 rings a 16-core engine whose pairs all talked leaves cached
    std::size_t const     ring = 21 * kPage, other = 22 * kPage;
    // All lent at once, so the bound keeps every one; `other` is released first, under all of them.
    void *const         lone = detail::region_acquire(other);
    std::vector<void *> rings(kMany);
    for (auto &r : rings)
        r = detail::region_acquire(ring);
    detail::region_release(lone, other);
    for (void *const r : rings)
        detail::region_release(r, ring);
    ASSERT_EQ(detail::region_stats().cached, kMany + 1);

    std::size_t const maps = mappings();
    EXPECT_EQ(detail::region_acquire(other), lone) << "a size cached under newer regions of another is still found";
    for (std::size_t i = kMany; i-- > 0;)
        ASSERT_EQ(detail::region_acquire(ring), rings[i]) << "the regions of one size are lent newest first";
    EXPECT_EQ(mappings(), maps);
    EXPECT_EQ(detail::region_stats().cached, 0u);

    detail::region_release(lone, other);
    for (void *const r : rings)
        detail::region_release(r, ring);
    region_cache::trim();
}

TEST(RegionCache, TheBoundUnmapsTheOldestRegionOfASizeAndKeepsItsNewerOnes) {
    region_cache::trim(); // nothing cached, and the high water is what is lent now
    ASSERT_EQ(detail::region_stats().lent_bytes, 0u) << "a case left a region lent";
    std::size_t const a = 31 * kPage, c = 60 * kPage, d = 5 * kPage; // sizes no other case uses
    void *const       a1 = detail::region_acquire(a);
    void *const       a2 = detail::region_acquire(a); // high water 2a
    detail::region_release(a1, a);                    // the oldest
    detail::region_release(a2, a);
    detail::region_release(detail::region_acquire(c), c); // 2a + c <= 4a: everything stays
    EXPECT_EQ(detail::region_stats().cached, 3u);
    detail::region_release(detail::region_acquire(d), d); // 2a + c + d > 4a: a1, the oldest, goes
    EXPECT_EQ(detail::region_stats().cached, 3u);
    EXPECT_EQ(detail::region_stats().cached_bytes, a + c + d);

    std::size_t const maps = mappings();
    EXPECT_EQ(detail::region_acquire(a), a2) << "the newer region of the size the bound cut into is still cached";
    EXPECT_EQ(mappings(), maps);
    void *const again = detail::region_acquire(a); // a1 was unmapped: a fresh mapping
    EXPECT_EQ(mappings(), maps + 1);
    detail::region_release(again, a);
    detail::region_release(a2, a);
    region_cache::trim();
}

TEST(RegionCache, TheSizeTableGrowsPastAPageAndTheLargestFitIsFoundAmongEverySize) {
    region_cache::trim();
    constexpr std::size_t kSizes = 200; // a page of the size table holds 170
    std::vector<void *>   regions(kSizes + 1, nullptr);
    for (std::size_t pages = 1; pages <= kSizes; ++pages) // all lent at once: the bound keeps them all
        regions[pages] = detail::region_acquire(pages * kPage);
    for (std::size_t i = 0; i < kSizes; ++i) { // released out of order, so sizes are filed between others
        std::size_t const pages = 1 + (i * 7) % kSizes;
        detail::region_release(regions[pages], pages * kPage);
    }
    ASSERT_EQ(detail::region_stats().cached, kSizes);

    EXPECT_EQ(detail::region_largest_cached(50 * kPage, 120 * kPage), 120 * kPage);
    EXPECT_EQ(detail::region_largest_cached(50 * kPage, 120 * kPage + kPage - 1), 120 * kPage);
    EXPECT_EQ(detail::region_largest_cached(0, (std::numeric_limits<std::size_t>::max)()), kSizes * kPage);
    EXPECT_EQ(detail::region_largest_cached(120 * kPage + 1, 121 * kPage - 1), 0u);
    EXPECT_EQ(detail::region_largest_cached((kSizes + 1) * kPage, (std::numeric_limits<std::size_t>::max)()), 0u);

    std::size_t const maps = mappings();
    for (std::size_t pages = kSizes; pages >= 1; --pages)
        ASSERT_EQ(detail::region_acquire(pages * kPage), regions[pages]) << pages << " pages";
    EXPECT_EQ(mappings(), maps);
    EXPECT_EQ(detail::region_stats().cached, 0u);
    EXPECT_EQ(detail::region_largest_cached(0, (std::numeric_limits<std::size_t>::max)()), 0u) << "an empty cache fits nothing";

    for (std::size_t pages = 1; pages <= kSizes; ++pages)
        detail::region_release(regions[pages], pages * kPage);
    EXPECT_EQ(region_cache::trim(), kSizes);
}

TEST(RegionCache, AReservedRegionIsLentAgainAsOneWithWhatWasCommittedInIt) {
    std::size_t const bytes  = size_of(13);
    void *const       first  = detail::region_acquire_reserved(bytes);
    auto *const       region = static_cast<unsigned char *>(first);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(first) % kPage, 0u);
    region[0] = 0x5A; // the first page is committed: the cache keeps its links there once the region is released
    ASSERT_TRUE(detail::region_commit(region + 10 * kPage + 100, 2 * kPage)); // rounded out: pages 10, 11 and 12
    std::memset(region + 10 * kPage, 0xA5, 3 * kPage);
    detail::region_release_reserved(first, bytes);

    std::size_t const maps  = mappings();
    void *const       again = detail::region_acquire_reserved(bytes);
    ASSERT_EQ(again, first) << "a reserved region was not lent again for its size";
    EXPECT_EQ(mappings(), maps);
    EXPECT_EQ(region[10 * kPage], 0xA5);
    EXPECT_EQ(region[13 * kPage - 1], 0xA5) << "what was committed in a reserved region did not stay with it";
    detail::region_release_reserved(again, bytes);
}

TEST(RegionCache, AReservedRegionAndACommittedOneOfOneSizeAreNeverLentForEachOtherOnWindows) {
    region_cache::trim();
    std::size_t const bytes    = size_of(14);
    void *const       reserved = detail::region_acquire_reserved(bytes);
    ASSERT_NE(reserved, nullptr);
    detail::region_release_reserved(reserved, bytes);
#if defined(_WIN32)
    // A reserved region's pages past the first are not committed: lent for a committed request, its first write there
    // would fault. A growing container is never offered one either.
    EXPECT_EQ(detail::region_largest_cached(bytes, bytes + kPage), 0u) << "a growing container was offered a reserved region";
    std::size_t const maps      = mappings();
    void *const       committed = detail::region_acquire(bytes);
    EXPECT_NE(committed, reserved) << "a reserved region was lent for a committed request";
    EXPECT_EQ(mappings(), maps + 1);
    std::memset(committed, 0xA5, bytes); // every byte writable
    detail::region_release(committed, bytes);
    EXPECT_EQ(detail::region_acquire_reserved(bytes), reserved) << "a committed region was lent for a reserved request";
    detail::region_release_reserved(reserved, bytes);
#else
    // POSIX has one kind: a mapping's pages are the process's once written, so a reservation is an acquisition.
    EXPECT_EQ(detail::region_largest_cached(bytes, bytes + kPage), bytes);
    void *const committed = detail::region_acquire(bytes);
    EXPECT_EQ(committed, reserved);
    std::memset(committed, 0xA5, bytes);
    detail::region_release(committed, bytes);
#endif
    region_cache::trim();
}
