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
 * @brief The two `qb::unordered_map` properties the framework actually leans on.
 *
 * `qb/system/container/unordered_map.h` selects a **different container per build**: the ska
 * `flat_hash_map` under `NDEBUG`, node-based `std::unordered_map` otherwise. Every guarantee the
 * framework relies on therefore has to hold for *both*, and a violation in only one of them is the
 * worst kind — the sanitizer presets use the node-based map, so a flat-map-only defect is
 * structurally invisible there and surfaces only in release. That is exactly how the reap-loop
 * defect behaved (SIGSEGV in release, a silent hang under the sanitizers).
 *
 * Two properties are load-bearing across the codebase, and neither was pinned anywhere:
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
 */

#include <cstdint>
#include <gtest/gtest.h>
#include <set>
#include <vector>
#include <qb/system/container/unordered_map.h>

namespace {

/// Which container the current build actually selected — reported so a failure is unambiguous.
constexpr const char *
config_name() noexcept {
#ifdef NDEBUG
    return "NDEBUG -> ska::flat_hash_map";
#else
    return "!NDEBUG -> std::unordered_map";
#endif
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
