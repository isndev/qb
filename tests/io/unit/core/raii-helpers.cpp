/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/raii-helpers.cpp
 * @brief `qb::resource` (unique_ptr + custom deleter) and `qb::scope_guard` (deferred cleanup).
 *
 * Two tiny RAII utilities from qb/system/cpu.h: `qb::resource(handle, deleter)` wraps a raw handle
 * (typed or `void*`) in a `unique_ptr` with a custom deleter; `qb::scope_guard(fn)` runs `fn` once
 * on scope exit unless dismissed, and transfers that obligation on move. Pure value semantics — no
 * engine, no I/O — a strict `unit` test.
 *
 * Split out of system/test-cpu.cpp (spec §2). Strengthened per the spec:
 *   - `scope_guard` move semantics are pinned at COMPILE time: it is move-constructible but
 *     move-ASSIGNMENT (and copy) are deleted (static_assert), so the "move transfers the obligation"
 *     runtime test cannot be silently reinterpreted by a future move-assign overload;
 *   - `resource` gains the two contracts the old smoke test missed — the deleter is NOT invoked
 *     when the wrapped handle is null (no-delete-on-empty), and a moved-from `resource` does NOT
 *     double-delete (the obligation transfers exactly once).
 */

#include <cstdlib>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <qb/system/cpu.h>

// =============================================================================
// qb::resource — unique_ptr with a custom deleter
// =============================================================================

/**
 * @test A typed handle is wrapped, dereferenceable, and its deleter fires exactly once on reset.
 * @brief Folded from test-cpu.cpp::WrapsPointerWithCustomDeleter.
 */
TEST(Resource, WrapsTypedHandleAndInvokesDeleterOnce) {
    int  delete_count = 0;
    auto ptr          = qb::resource(new int(42), [&delete_count](int *value) {
        ++delete_count;
        delete value;
    });

    ASSERT_NE(ptr.get(), nullptr);
    EXPECT_EQ(*ptr, 42);

    ptr.reset();
    EXPECT_EQ(delete_count, 1) << "the custom deleter must run exactly once on reset()";
    EXPECT_EQ(ptr.get(), nullptr);
}

/**
 * @test A `void*` handle is wrapped and freed through its deleter.
 * @brief Folded from test-cpu.cpp::WrapsVoidPointerWithCustomDeleter.
 */
TEST(Resource, WrapsVoidHandleAndInvokesDeleter) {
    bool  deleted = false;
    void *raw     = std::malloc(8);
    ASSERT_NE(raw, nullptr);

    auto ptr = qb::resource(raw, [&deleted](void *value) {
        deleted = true;
        std::free(value);
    });

    ASSERT_NE(ptr.get(), nullptr);
    ptr.reset();
    EXPECT_TRUE(deleted);
}

/**
 * @test The deleter is NOT invoked when the wrapped handle is null.
 * @brief Spec strengthening: a `unique_ptr` with a null pointer must not call its deleter on
 *        destruction. Pins that `qb::resource(nullptr, ...)` is a safe no-op (no spurious free).
 */
TEST(Resource, NullHandleDoesNotInvokeDeleter) {
    int delete_count = 0;
    {
        auto ptr = qb::resource(static_cast<int *>(nullptr), [&delete_count](int *value) {
            ++delete_count;
            delete value;
        });
        EXPECT_EQ(ptr.get(), nullptr);
    } // scope exit: destructor must NOT call the deleter for a null handle
    EXPECT_EQ(delete_count, 0) << "a resource over a null handle must never delete";
}

/**
 * @test Moving a `resource` transfers ownership exactly once — the deleter fires a single time.
 * @brief Spec strengthening: the moved-from wrapper is emptied, so destroying both the source and
 *        the destination must NOT double-delete the handle.
 */
TEST(Resource, MoveTransfersOwnershipWithoutDoubleDelete) {
    int delete_count = 0;
    {
        auto src = qb::resource(new int(7), [&delete_count](int *value) {
            ++delete_count;
            delete value;
        });
        ASSERT_NE(src.get(), nullptr);

        auto dst = std::move(src);
        EXPECT_EQ(src.get(), nullptr) << "moved-from resource must be emptied";
        ASSERT_NE(dst.get(), nullptr);
        EXPECT_EQ(*dst, 7);
        EXPECT_EQ(delete_count, 0) << "moving must not delete";
    } // both src (empty) and dst (owning) destroyed here
    EXPECT_EQ(delete_count, 1) << "the handle must be deleted exactly once after a move";
}

// =============================================================================
// qb::scope_guard — deferred cleanup with move-transfer of the obligation
// =============================================================================

namespace {

// The deduced scope_guard type over a capturing lambda, used to pin its special-member contract.
using guard_t = decltype(qb::scope_guard(std::declval<std::function<void()>>()));

static_assert(std::is_move_constructible_v<guard_t>, "scope_guard must be move-constructible (the obligation transfers)");
static_assert(!std::is_move_assignable_v<guard_t>, "scope_guard move-ASSIGNMENT is deleted by contract");
static_assert(!std::is_copy_constructible_v<guard_t>, "scope_guard must not be copyable");
static_assert(!std::is_copy_assignable_v<guard_t>, "scope_guard copy-assignment is deleted");

} // namespace

/**
 * @test The guard runs its callable once on scope exit, and `dismiss()` cancels it.
 * @brief Folded from test-cpu.cpp::RunsOnScopeExitUnlessDismissed.
 */
TEST(ScopeGuard, RunsOnScopeExitUnlessDismissed) {
    int counter = 0;
    {
        qb::scope_guard guard([&counter] { ++counter; });
    }
    EXPECT_EQ(counter, 1) << "an undismissed guard must fire exactly once on scope exit";

    {
        qb::scope_guard guard([&counter] { ++counter; });
        guard.dismiss();
    }
    EXPECT_EQ(counter, 1) << "a dismissed guard must not fire";
}

/**
 * @test Move-constructing a guard transfers the cleanup obligation: it fires once, from the
 *       destination only (the moved-from source is dismissed).
 * @brief Folded from test-cpu.cpp::MoveTransfersOwnership; the no-double-fire is now explicit and
 *        the move-assign-deleted contract above guarantees this is the only move path.
 */
TEST(ScopeGuard, MoveTransfersTheCleanupObligationExactlyOnce) {
    int counter = 0;
    {
        qb::scope_guard first([&counter] { ++counter; });
        qb::scope_guard second = std::move(first);
        // `first` is now dismissed; only `second` carries the obligation.
        EXPECT_EQ(counter, 0) << "moving must not fire the cleanup";
    } // both destroyed; only `second` runs
    EXPECT_EQ(counter, 1) << "the cleanup must run exactly once after a move (no double-fire)";
}
