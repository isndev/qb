/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/cancellation-token.cpp
 * @brief `qb::io::async::cancellation_token` state mechanics — pure logic, no event loop.
 *
 * The cancellation_token (qb/io/async/coroutine/cancellation.h) is a single-thread, shared-state
 * signal: a `shared_ptr<state>` holding a `cancelled` bool and a keyed `(id, callback)` list. None
 * of these mechanics need the scheduler or a timer — they are deterministic value/state operations —
 * so every test here is a true UNIT test (no `init()`, no `spawn()`, no `run_for()`). The loop-driven
 * cancellation-aware awaiters (`cancellable_sleep`, `check_cancelled`, `yield_or_cancel`,
 * `make_cancellable`) live in system/async/cancellation-awaiters.cpp; the `with_deadline` combinator
 * lives in system/async/deadline-combinator.cpp.
 *
 * Contracts proven here:
 *   - construct → not cancelled; `cancel()` → cancelled; `cancel()` is idempotent (fires once).
 *   - a copy shares the same state (cancel through one observes through the other).
 *   - `on_cancel` fires on `cancel()`, and fires *inline* (returning id 0) if already cancelled.
 *   - `throw_if_cancelled` is a no-op until cancelled, then throws `cancelled_error`.
 *   - `remove_on_cancel(id)` deregisters exactly one callback by id; it is a safe no-op after
 *     cancellation and for id 0 — this is the core of the unbounded-callback-growth fix, so a
 *     register/remove loop must keep the callback set bounded (size 1 then empty, 10000×).
 *   - the empty/null token (`null_token`) owns no state: never cancels, allocates nothing,
 *     `on_cancel` returns 0 and drops the callback.
 *
 * Split out of the former coroutine/test-coroutine-cancellation.cpp (unit half). The vacuous
 * `SUCCEED()` no-op-safety test is strengthened to assert observable callback-set state instead.
 */

#include <atomic>
#include <cstdint>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

using qb::io::async::cancellation_token;
using qb::io::async::cancelled_error;
using qb::io::async::null_token;

// ---------------------------------------------------------------------------
// Basic cancel / query
// ---------------------------------------------------------------------------

TEST(CancellationToken, StartsUncancelledThenCancels) {
    cancellation_token token;
    EXPECT_FALSE(token.is_cancelled());
    EXPECT_TRUE(static_cast<bool>(token)) << "a default token owns shared state";

    token.cancel();
    EXPECT_TRUE(token.is_cancelled());
}

TEST(CancellationToken, CopySharesState) {
    cancellation_token token1;
    cancellation_token token2 = token1;

    EXPECT_FALSE(token1.is_cancelled());
    EXPECT_FALSE(token2.is_cancelled());

    token1.cancel();

    // Both views observe the cancellation through the shared state.
    EXPECT_TRUE(token1.is_cancelled());
    EXPECT_TRUE(token2.is_cancelled());
}

TEST(CancellationToken, CancelIsIdempotentFiresCallbackOnce) {
    cancellation_token token;
    std::atomic<int>   callback_count{0};

    token.on_cancel([&callback_count]() { ++callback_count; });

    token.cancel();
    token.cancel();
    token.cancel();

    EXPECT_EQ(callback_count.load(), 1) << "repeated cancel() must fire the callback exactly once";
    EXPECT_TRUE(token.is_cancelled());
}

// ---------------------------------------------------------------------------
// on_cancel firing semantics
// ---------------------------------------------------------------------------

TEST(CancellationToken, OnCancelFiresWhenCancelled) {
    cancellation_token token;
    std::atomic<bool>  invoked{false};

    const auto id = token.on_cancel([&invoked]() { invoked = true; });
    EXPECT_NE(id, 0u) << "registering on a live token returns a real id";
    EXPECT_FALSE(invoked.load());

    token.cancel();
    EXPECT_TRUE(invoked.load());
}

TEST(CancellationToken, OnCancelFiresInlineWhenAlreadyCancelledAndReturnsZero) {
    cancellation_token token;
    token.cancel();

    std::atomic<bool> invoked{false};
    const auto        id = token.on_cancel([&invoked]() { invoked = true; });

    EXPECT_TRUE(invoked.load()) << "late subscriber on an already-cancelled token must run inline";
    EXPECT_EQ(id, 0u) << "an inline-fired callback leaves nothing to deregister → id 0";
    EXPECT_TRUE(token.get_state()->callbacks.empty());
}

// ---------------------------------------------------------------------------
// throw_if_cancelled
// ---------------------------------------------------------------------------

TEST(CancellationToken, ThrowIfCancelled) {
    cancellation_token token;
    EXPECT_NO_THROW(token.throw_if_cancelled());

    token.cancel();
    EXPECT_THROW(token.throw_if_cancelled(), cancelled_error);
}

// ---------------------------------------------------------------------------
// remove_on_cancel — the unbounded-growth fix
// ---------------------------------------------------------------------------

TEST(CancellationToken, RemoveOnCancelDeregistersExactlyOne) {
    cancellation_token token;
    std::atomic<int>   a{0}, b{0}, c{0};

    const auto id_a = token.on_cancel([&a]() { ++a; });
    const auto id_b = token.on_cancel([&b]() { ++b; });
    const auto id_c = token.on_cancel([&c]() { ++c; });
    EXPECT_NE(id_a, 0u);
    EXPECT_NE(id_b, 0u);
    EXPECT_NE(id_c, 0u);
    EXPECT_EQ(token.get_state()->callbacks.size(), 3u);

    token.remove_on_cancel(id_b); // drop the middle one
    EXPECT_EQ(token.get_state()->callbacks.size(), 2u);

    token.cancel();
    EXPECT_EQ(a.load(), 1);
    EXPECT_EQ(b.load(), 0) << "the deregistered callback must not fire";
    EXPECT_EQ(c.load(), 1);
}

TEST(CancellationToken, RemoveOnCancelIsSafeAfterCancelAndForZeroId) {
    cancellation_token token;
    std::atomic<bool>  fired{false};
    const auto         id = token.on_cancel([&fired]() { fired = true; });

    token.cancel();
    EXPECT_TRUE(fired.load());
    EXPECT_TRUE(token.get_state()->callbacks.empty()) << "cancel() consumes the callback list";

    // Both of these must be safe no-ops (the entry is already gone; id 0 is reserved).
    token.remove_on_cancel(id);
    token.remove_on_cancel(0);
    EXPECT_TRUE(token.get_state()->callbacks.empty());
}

TEST(CancellationToken, CallbacksDoNotAccumulateAcrossRegisterRemoveLoop) {
    cancellation_token token;
    // Before the fix, on_cancel had no removal path, so a long-lived (actor-scope) token grew one
    // std::function per iteration forever. The set must stay bounded under register/remove churn.
    for (int i = 0; i < 10000; ++i) {
        const auto id = token.on_cancel([]() {});
        EXPECT_EQ(token.get_state()->callbacks.size(), 1u);
        token.remove_on_cancel(id);
        EXPECT_TRUE(token.get_state()->callbacks.empty());
    }

    std::atomic<int> fired{0};
    (void) token.on_cancel([&fired]() { ++fired; });
    token.cancel();
    EXPECT_EQ(fired.load(), 1) << "only the single live callback survives to fire";
}

// ---------------------------------------------------------------------------
// Empty / null token — owns no state, never cancels, allocates nothing
// ---------------------------------------------------------------------------

TEST(CancellationToken, NullTokenOwnsNoStateAndNeverCancels) {
    cancellation_token token{null_token};
    EXPECT_FALSE(static_cast<bool>(token)) << "an empty token owns no shared state";
    EXPECT_FALSE(token.is_cancelled());

    std::atomic<bool> fired{false};
    const auto        id = token.on_cancel([&fired]() { fired = true; });
    EXPECT_EQ(id, 0u) << "an empty token drops the callback and returns id 0";

    token.cancel(); // no-op on an empty token
    EXPECT_FALSE(token.is_cancelled());
    EXPECT_FALSE(fired.load()) << "an empty token never fires its dropped callback";
    EXPECT_NO_THROW(token.throw_if_cancelled());
    EXPECT_EQ(token.get_state(), nullptr);
}
