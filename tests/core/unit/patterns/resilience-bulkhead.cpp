/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/patterns/resilience-bulkhead.cpp
 * @brief Non-blocking `qb::bulkhead` permit accounting: `try_enter` / `available` / slot RAII.
 *
 * The cancellation-aware `bulkhead::enter()` needs a running coroutine scope, but the permit
 * bookkeeping underneath it is pure synchronous state on a counting semaphore — testable with no
 * engine (daemon-free, parallel-safe). This pins the documented accounting exactly:
 *   - a fresh bulkhead reports `available() == capacity`;
 *   - `try_enter` decrements the count and hands out a live slot until the pool is empty, then
 *     REFUSES (returns false, no slot leaked);
 *   - `slot::release()` returns a permit early and is idempotent;
 *   - a slot frees its permit on destruction (RAII), and the move leaves the moved-from slot inert
 *     (only the new owner releases — no double-free);
 *   - capacity 0 clamps to 1.
 *
 * The engine-driven `enter()` cap + parked-cancel are covered under
 * system/patterns/resilience-bulkhead.cpp.
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <utility>

TEST(BulkheadAccounting, TryEnterDrainsAndRefusesAtCapacity) {
    qb::bulkhead       bh(2);
    qb::bulkhead::slot s1, s2, s3;
    EXPECT_EQ(bh.available(), 2u);

    EXPECT_TRUE(bh.try_enter(s1));
    EXPECT_EQ(bh.available(), 1u);
    EXPECT_TRUE(bh.try_enter(s2));
    EXPECT_EQ(bh.available(), 0u);
    EXPECT_FALSE(bh.try_enter(s3)) << "full bulkhead must refuse — no permit handed out";
    EXPECT_EQ(bh.available(), 0u); // a refused try_enter must not have consumed anything

    s1.release(); // free one permit early
    EXPECT_EQ(bh.available(), 1u);
    EXPECT_TRUE(bh.try_enter(s3)) << "a freed permit must immediately admit a waiting caller";
    EXPECT_EQ(bh.available(), 0u);

    s1.release(); // idempotent: releasing an already-released slot is a no-op
    EXPECT_EQ(bh.available(), 0u) << "double release() must not over-credit the semaphore";
}

TEST(BulkheadAccounting, SlotReleasesOnScopeExit) {
    qb::bulkhead bh(1);
    EXPECT_EQ(bh.available(), 1u);
    {
        qb::bulkhead::slot s;
        EXPECT_TRUE(bh.try_enter(s));
        EXPECT_EQ(bh.available(), 0u);
    } // s destroyed → permit returned
    EXPECT_EQ(bh.available(), 1u) << "slot RAII must return the permit on scope exit";
}

TEST(BulkheadAccounting, MoveTransfersOwnershipWithoutDoubleRelease) {
    qb::bulkhead       bh(1);
    qb::bulkhead::slot s1;
    EXPECT_TRUE(bh.try_enter(s1));
    EXPECT_EQ(bh.available(), 0u);

    qb::bulkhead::slot s2 = std::move(s1); // ownership moves to s2; s1 is now inert
    EXPECT_EQ(bh.available(), 0u);         // the move itself releases nothing

    s1.release(); // moved-from → no-op
    EXPECT_EQ(bh.available(), 0u) << "moved-from slot must not release the new owner's permit";

    s2.release(); // the real owner frees it exactly once
    EXPECT_EQ(bh.available(), 1u);
}

TEST(BulkheadAccounting, ZeroCapacityClampsToOne) {
    qb::bulkhead       bh(0); // clamped to >= 1
    qb::bulkhead::slot s1, s2;
    EXPECT_EQ(bh.available(), 1u);
    EXPECT_TRUE(bh.try_enter(s1));
    EXPECT_FALSE(bh.try_enter(s2)) << "clamped single-permit bulkhead admits exactly one";
}
