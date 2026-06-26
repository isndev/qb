/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/uuid-threadsafety.cpp
 * @brief `qb::generate_random_uuid()` — concurrent uniqueness of the v4 UUID generator.
 *
 * `generate_random_uuid()` (qb/uuid.h) is called from arbitrary threads across the framework
 * (session ids, message ids), so its underlying RNG must be thread-safe and collision-free under
 * contention. This drives raw `std::thread`s only — NO `qb::Main`, NO socket, NO event loop — so
 * despite the threading it is a `unit` test (the `threadsafe` label marks the contention intent).
 *
 * Migrated from system/test-io.cpp::SocketUtils.UUIDThreadSafety (spec D4). It is daemon-free and
 * deterministic in its oracle: N threads each generate a fixed count, and the assertion is that the
 * total set of stringified UUIDs has exactly N*count distinct elements — a single collision (or a
 * torn/duplicated value from a data race) drops the set size and fails loudly. No sleeps, no wall
 * clock; the `join()` barrier is the only synchronization.
 */

#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/uuid.h>

/**
 * @test 8 threads × 500 UUIDs each → 4000 globally-distinct values.
 * @brief Any collision or data-race-induced duplicate shrinks the deduplicated set below the
 *        expected total, so the exact-count assertion is a precise failure signal.
 */
TEST(UuidThreadSafety, ConcurrentGenerationProducesNoCollisions) {
    constexpr int num_threads      = 8;
    constexpr int uuids_per_thread = 500;

    std::vector<std::vector<qb::uuid>> results(num_threads);
    std::vector<std::thread>           threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&results, t]() {
            results[t].reserve(uuids_per_thread);
            for (int i = 0; i < uuids_per_thread; ++i)
                results[t].push_back(qb::generate_random_uuid());
        });
    }

    for (auto &th : threads)
        th.join();

    std::set<std::string> all_uuids;
    for (const auto &vec : results)
        for (const auto &u : vec)
            all_uuids.insert(uuids::to_string(u));

    EXPECT_EQ(all_uuids.size(), static_cast<std::size_t>(num_threads * uuids_per_thread))
        << "every generated UUID must be globally unique under concurrency";
}
