/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/container/unordered-map-contract.cpp
 * @brief The `qb::unordered_map` properties the framework actually leans on, plus the one
 *        place its "drop-in for std::unordered_map" promise used to fail.
 *
 * `qb::unordered_map` is `ska::unordered_map`, unconditionally. Until 3.0.0 the alias resolved
 * to a *different container per build* — ska under `NDEBUG`, `std::unordered_map` otherwise —
 * which made the identity and layout of a public type depend on a build macro and aborted at
 * runtime when a Debug consumer linked a Release libqb. That switch is gone and
 * `qbmModuleConfig.cmake.in` carries a configure-time tripwire against its return, so these
 * tests now pin one container rather than two. (This file's own header claimed the switch was
 * still live until 3.0 — a per-build container is exactly the kind of claim that keeps reading
 * as true long after it stops being.)
 *
 * Three properties are load-bearing, and none was pinned anywhere:
 *
 * 1. **`it = map.erase(it)` while iterating visits every element exactly once.** The HTTP/2 server
 *    walks `_server_streams` and erases as it goes on the GOAWAY path
 *    (`qbm/http/2/protocol/server.h`), and the pattern recurs in the idle-stream sweep and the
 *    pending-flush loops. If the flat map's backward-shift deletion moved a not-yet-visited entry
 *    behind the cursor, streams would be silently skipped — no crash, just a stream that never
 *    gets its error event and never gets reclaimed.
 *
 * 2. **A reference into the map stays valid across a rehash** (node stability). `VirtualCore`
 *    holds `Actor` objects by `unique_ptr` inside the map and hands out raw pointers; the HTTP/2
 *    server takes `Http2ServerStream &` references and keeps using them across calls that can
 *    insert. Iterators are NOT stable across a rehash — only the pointed-to values are — so this
 *    test states both halves explicitly rather than leaving the distinction to folklore.
 *
 * 3. **`contains()` exists and agrees with `find()` and `count()`.** A qb addition to the fork:
 *    C++20 gave every standard associative container a `contains()`, upstream ska predates
 *    C++20, and the alias advertises itself as a drop-in — so its absence was the single point
 *    where that promise failed, and it failed as a compile error naming a template deep inside
 *    the vendored header. The test is a three-way agreement rather than a bare existence check,
 *    because a `contains()` that disagreed with `find()` would be worse than none at all, and it
 *    covers all four aliases: a member added to a shared base reaches map and set together, and
 *    only checking one is how the other silently misses it.
 */

#include <cstdint>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>
#include <qb/system/container/unordered_map.h>
#include <qb/system/container/unordered_set.h>

namespace {

/// Reported on a failure so the message names the container under test.
constexpr const char *
config_name() noexcept {
    return "qb::unordered_map -> ska::unordered_map (unconditional since 3.0.0)";
}

} // namespace

TEST(UnorderedMapContract, EraseWhileIteratingVisitsEveryElementExactlyOnce) {
    // Sized to cross several growth/shrink thresholds, since the hazard (if any) lives in the
    // rehash / backward-shift machinery rather than in any single erase.
    for (const int n : {8, 64, 512, 4096}) {
        qb::unordered_map<std::uint32_t, std::uint32_t> m;
        for (int i = 0; i < n; ++i)
            m[static_cast<std::uint32_t>(i)] = static_cast<std::uint32_t>(i);

        std::set<std::uint32_t> visited;
        for (auto it = m.begin(); it != m.end();) {
            ASSERT_TRUE(visited.insert(it->first).second)
                << config_name() << ", n=" << n << ": key " << it->first << " was visited twice — erase() moved an already-seen entry "
                << "back in front of the cursor";
            if (it->first % 2u == 0u)
                it = m.erase(it);
            else
                ++it;
        }

        EXPECT_EQ(visited.size(), static_cast<std::size_t>(n))
            << config_name() << ", n=" << n << ": the walk skipped " << (static_cast<std::size_t>(n) - visited.size())
            << " element(s) — erase() moved an unvisited entry behind the cursor";
        EXPECT_EQ(m.size(), static_cast<std::size_t>(n / 2)) << config_name() << ", n=" << n;
        for (int i = 0; i < n; ++i) {
            const bool present = m.find(static_cast<std::uint32_t>(i)) != m.end();
            EXPECT_EQ(present, (i % 2) != 0) << config_name() << ", n=" << n << ": key " << i << " survived the wrong way";
        }
    }
}

TEST(UnorderedMapContract, ValuesStayPutAcrossARehashEvenThoughIteratorsDoNot) {
    qb::unordered_map<std::uint32_t, std::uint64_t> m;
    m[1] = 0xAAAA;

    // A raw pointer to the mapped value — the shape VirtualCore and the HTTP/2 server both use.
    std::uint64_t *pinned = &m[1];
    *pinned               = 0xBBBB;

    // Force many rehashes.
    for (std::uint32_t i = 2; i < 4096; ++i)
        m[i] = i;

    ASSERT_NE(m.find(1), m.end());
    EXPECT_EQ(m[1], 0xBBBBu);
    EXPECT_EQ(*pinned, 0xBBBBu) << config_name()
                                << ": a reference into the map did NOT survive a rehash. VirtualCore hands out raw "
                                   "Actor* from its actor map and the HTTP/2 server keeps stream references across "
                                   "inserts — both would become use-after-free.";
    EXPECT_EQ(pinned, &m[1]) << config_name() << ": the mapped value moved during rehash";
}

// ---------------------------------------------------------------------------
// contains() — the drop-in promise, on all four aliases
// ---------------------------------------------------------------------------

namespace {

/// Assert the three lookups agree, on any of the four qb aliases. A `contains()` that
/// disagreed with `find()` would be worse than no `contains()` at all, so the property under
/// test is agreement, not existence.
template <typename Map>
void
expect_map_lookups_agree(const char *which) {
    Map m;
    for (int i = 0; i < 64; ++i)
        m[i] = i * 10;

    for (int i = 0; i < 128; ++i) {
        const bool by_find     = m.find(i) != m.end();
        const bool by_count    = m.count(i) != 0;
        const bool by_contains = m.contains(i);
        ASSERT_EQ(by_contains, by_find) << which << ": contains() disagrees with find() for key " << i;
        ASSERT_EQ(by_contains, by_count) << which << ": contains() disagrees with count() for key " << i;
        ASSERT_EQ(by_contains, i < 64) << which << ": wrong answer for key " << i;
    }

    // Erase must be observed by contains() too — a stale positive is the failure that would
    // matter, since it is the spelling most callers will reach for on a liveness check.
    m.erase(7);
    EXPECT_FALSE(m.contains(7)) << which << ": contains() still reports an erased key";
    EXPECT_EQ(m.find(7), m.end()) << which << ": find() still reports an erased key";

    // And on an empty container, where a bad end() comparison would show.
    Map empty;
    EXPECT_FALSE(empty.contains(0)) << which << ": contains() on an empty map";
}

template <typename Set>
void
expect_set_lookups_agree(const char *which) {
    Set s;
    for (int i = 0; i < 64; ++i)
        s.insert(i);

    for (int i = 0; i < 128; ++i) {
        const bool by_find     = s.find(i) != s.end();
        const bool by_contains = s.contains(i);
        ASSERT_EQ(by_contains, by_find) << which << ": contains() disagrees with find() for key " << i;
        ASSERT_EQ(by_contains, i < 64) << which << ": wrong answer for key " << i;
    }

    s.erase(7);
    EXPECT_FALSE(s.contains(7)) << which << ": contains() still reports an erased key";

    Set empty;
    EXPECT_FALSE(empty.contains(0)) << which << ": contains() on an empty set";
}

} // namespace

TEST(UnorderedMapContract, ContainsAgreesWithFindAndCountOnEveryAlias) {
    // Both node-based and flat variants: the member was added once to each vendored base
    // (sherwood_v10_table and sherwood_v3_table), and each base serves a map and a set.
    expect_map_lookups_agree<qb::unordered_map<int, int>>("qb::unordered_map");
    expect_map_lookups_agree<qb::unordered_flat_map<int, int>>("qb::unordered_flat_map");
    expect_set_lookups_agree<qb::unordered_set<int>>("qb::unordered_set");
    expect_set_lookups_agree<qb::unordered_flat_set<int>>("qb::unordered_flat_set");
}

TEST(UnorderedMapContract, ContainsWorksForANonTrivialKey) {
    // std::string keys exercise the hashing/equality path rather than the identity-hash one,
    // which is what most callers actually use (header names, actor service names, route paths).
    qb::unordered_map<std::string, int> m;
    m["alpha"] = 1;
    m["beta"]  = 2;

    EXPECT_TRUE(m.contains("alpha"));
    EXPECT_TRUE(m.contains("beta"));
    EXPECT_FALSE(m.contains("gamma"));
    EXPECT_EQ(m.contains("alpha"), m.find("alpha") != m.end());
    EXPECT_EQ(m.contains("gamma"), m.find("gamma") != m.end());

    qb::unordered_set<std::string> s{"one", "two"};
    EXPECT_TRUE(s.contains("one"));
    EXPECT_FALSE(s.contains("three"));
}
