/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/core/main-core-range.cpp
 * @brief `Main::core(idx)` bounds checking + `qb::NoAffinity` sentinel handling.
 *
 * `Main::core(idx)` exposes a per-core builder; `idx` must be a valid `CoreId` in the half-open
 * range [0, qb::MaxCores). Out-of-range indices throw `std::range_error` at build time (before the
 * engine starts). Separately, `setAffinity(CoreIdSet{NoAffinity})` must treat the sentinel as
 * "no pinning" and filter it before it ever reaches an OS affinity call.
 */

#include <stdexcept>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {
class TerminateImmediatelyActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};
} // namespace

// --- Main::core(idx) range check (boundary is half-open: [0, MaxCores)) ---

TEST(MainCoreRange, RejectsExactlyAtMaxCores) {
    qb::Main main;
    EXPECT_THROW(main.core(static_cast<qb::CoreId>(qb::MaxCores)).addActor<TerminateImmediatelyActor>(),
                 std::range_error)
        << "core(MaxCores) must throw — the boundary is half-open";
}

TEST(MainCoreRange, RejectsAboveMaxCores) {
    qb::Main main;
    EXPECT_THROW(main.core(static_cast<qb::CoreId>(qb::MaxCores + 100)).addActor<TerminateImmediatelyActor>(),
                 std::range_error);
}

TEST(MainCoreRange, AcceptsLastValidCoreId) {
    qb::Main main;
    EXPECT_NO_THROW(main.core(static_cast<qb::CoreId>(qb::MaxCores - 1)).addActor<TerminateImmediatelyActor>())
        << "MaxCores - 1 is the last legal CoreId; must not throw";
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

// --- qb::NoAffinity sentinel: filtered, never passed to OS pinning APIs ---

TEST(NoAffinity, SetAffinityWithSentinelOnlyDoesNotCrash) {
    qb::Main main;
    main.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity}).addActor<TerminateImmediatelyActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError()) << "CoreIdSet{NoAffinity} must be filtered out, not passed to OS APIs";
}

TEST(NoAffinity, SetAffinityWithMixedSentinelAndRealCoreIsSafe) {
    qb::Main main;
    // Real CPU id 0 is always present; the sentinel must be filtered, pinning still targets core 0.
    main.core(0)
        .setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(0), qb::NoAffinity})
        .addActor<TerminateImmediatelyActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
}
