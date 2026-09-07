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
 * signal: an intrusively refcounted `state` (a plain, NON-atomic count — every copy lives on the
 * owning thread) holding a `cancelled` bool, a keyed `(id, callback)` list and an intrusive list of
 * embedded `cancel_hook` nodes. None
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
 *   - the refcount: copy = +1, move = transfer, self-assignment is safe, the state lives exactly as
 *     long as its last handle, and `cancel()` survives a callback dropping the last other handle.
 *   - `cancel_hook`: `link` is O(1) and allocates nothing; a hook fires once, is detached BEFORE it
 *     fires (so `unlink()` from inside a firing hook — of itself or of a sibling — is legal and a
 *     sibling unlinked that way never fires); hooks fire before `on_cancel` callbacks; `link` on an
 *     already-cancelled token fires inline and returns false; on an empty token it returns false
 *     and never fires; `unlink` is idempotent; a link/unlink loop leaves the list empty.
 *
 * Split out of the former coroutine/test-coroutine-cancellation.cpp (unit half). The vacuous
 * `SUCCEED()` no-op-safety test is strengthened to assert observable callback-set state instead.
 */

#include <atomic>
#include <cstdint>
#include <utility>

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

// ---------------------------------------------------------------------------
// Intrusive refcount — same-thread, non-atomic, exact lifetime
// ---------------------------------------------------------------------------

TEST(CancellationToken, RefcountFollowsCopiesAndMoves) {
    cancellation_token a;
    auto *const        st = a.get_state();
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->refs, 1u);

    cancellation_token b = a; // copy: +1
    EXPECT_EQ(st->refs, 2u);
    EXPECT_EQ(b.get_state(), st);

    cancellation_token c = std::move(b); // move: transfer, no change
    EXPECT_EQ(st->refs, 2u);
    EXPECT_EQ(c.get_state(), st);
    EXPECT_EQ(b.get_state(), nullptr) << "a moved-from token is empty";
    EXPECT_FALSE(static_cast<bool>(b));

    {
        cancellation_token d;
        d = a; // copy-assign over an owned state: releases d's own, acquires a's
        EXPECT_EQ(st->refs, 3u);
        cancellation_token &alias = d;
        d                         = alias; // self-assignment must neither free nor leak
        EXPECT_EQ(st->refs, 3u);
        EXPECT_EQ(d.get_state(), st);
        d = std::move(c); // move-assign: c's reference transfers, d's own is released
        EXPECT_EQ(st->refs, 2u);
        EXPECT_EQ(c.get_state(), nullptr);
    } // d dies: 2 -> 1
    EXPECT_EQ(st->refs, 1u) << "the state outlives exactly its handles";
}

TEST(CancellationToken, CancelSurvivesCallbackDroppingLastOtherHandle) {
    // A callback may release the last reference to the very token object `cancel()` runs on.
    // The state must outlive the walk regardless: cancel() holds a reference of its own.
    auto              *heap = new cancellation_token;
    cancellation_token keep = *heap; // second handle, so `delete heap` is not the last release
    auto *const        st   = keep.get_state();
    EXPECT_EQ(st->refs, 2u);

    int  fired           = 0;
    bool released_inside = false;
    (void) heap->on_cancel([&]() {
        ++fired;
        delete heap; // the token object being cancelled goes away mid-walk
        released_inside = true;
    });
    (void) keep.on_cancel([&]() { ++fired; }); // a second callback, after the deletion
    heap->cancel();
    EXPECT_TRUE(released_inside);
    EXPECT_EQ(fired, 2) << "the walk completes after its own token object was destroyed";
    EXPECT_TRUE(keep.is_cancelled());
    EXPECT_EQ(st->refs, 1u) << "only `keep` remains";
}

// ---------------------------------------------------------------------------
// cancel_hook — embedded, zero-allocation registration
// ---------------------------------------------------------------------------

namespace {

struct HookProbe {
    cancellation_token::cancel_hook hook{};
    int                             fired{0};
    HookProbe                      *sibling_to_unlink{nullptr}; // when set, the hook unlinks this sibling as it fires
    HookProbe() {
        hook.fire = &HookProbe::on_fire;
        hook.ctx  = this;
    }
    static void
    on_fire(void *ctx) noexcept {
        auto *self = static_cast<HookProbe *>(ctx);
        ++self->fired;
        self->hook.unlink(); // legal from inside: already detached, so a no-op
        if (self->sibling_to_unlink)
            self->sibling_to_unlink->hook.unlink();
    }
};

bool
hooks_empty(const cancellation_token &t) {
    const auto &head = t.get_state()->hooks;
    return head.next == &head && head.prev == &head;
}

} // namespace

TEST(CancellationToken, HookLinksFiresOnceAndIsDetachedBeforeFiring) {
    cancellation_token token;
    HookProbe          probe;
    EXPECT_FALSE(probe.hook.linked());
    EXPECT_TRUE(token.link(probe.hook));
    EXPECT_TRUE(probe.hook.linked());
    EXPECT_FALSE(hooks_empty(token));

    token.cancel();
    EXPECT_EQ(probe.fired, 1);
    EXPECT_FALSE(probe.hook.linked()) << "cancel() detaches a hook before invoking it";
    EXPECT_TRUE(hooks_empty(token));

    token.cancel(); // idempotent: the hook is gone, nothing fires twice
    EXPECT_EQ(probe.fired, 1);
}

TEST(CancellationToken, HookUnlinkedBeforeCancelNeverFires) {
    cancellation_token token;
    HookProbe          probe;
    ASSERT_TRUE(token.link(probe.hook));
    probe.hook.unlink();
    EXPECT_FALSE(probe.hook.linked());
    EXPECT_TRUE(hooks_empty(token));
    probe.hook.unlink(); // idempotent
    token.cancel();
    EXPECT_EQ(probe.fired, 0) << "an unlinked hook is not the token's to fire";
}

TEST(CancellationToken, HookFiringMayUnlinkASiblingWhichThenNeverFires) {
    // The ask_awaiter contract: a hook torn down by an earlier hook (an awaiter whose frame the
    // first one destroyed) must not fire. Link order is a stack, so `second` fires first.
    cancellation_token token;
    HookProbe          first, second;
    ASSERT_TRUE(token.link(first.hook));
    ASSERT_TRUE(token.link(second.hook));
    second.sibling_to_unlink = &first;

    token.cancel();
    EXPECT_EQ(second.fired, 1);
    EXPECT_EQ(first.fired, 0) << "unlinked from inside a firing sibling: never invoked";
    EXPECT_FALSE(first.hook.linked());
    EXPECT_TRUE(hooks_empty(token));
}

TEST(CancellationToken, HooksFireBeforeOnCancelCallbacks) {
    cancellation_token token;
    int                order = 0, hook_at = 0, callback_at = 0;
    (void) token.on_cancel([&]() { callback_at = ++order; });
    HookProbe probe;
    ASSERT_TRUE(token.link(probe.hook));
    // Rebind the hook to record its position (the probe's own fire is not needed here).
    struct Rec {
        int *order, *at;
    } rec{&order, &hook_at};
    probe.hook.fire = [](void *ctx) noexcept {
        auto *r = static_cast<Rec *>(ctx);
        *r->at  = ++*r->order;
    };
    probe.hook.ctx = &rec;

    token.cancel();
    EXPECT_EQ(hook_at, 1);
    EXPECT_EQ(callback_at, 2) << "embedded hooks run before the std::function callbacks";
}

TEST(CancellationToken, LinkOnCancelledTokenFiresInlineAndReturnsFalse) {
    cancellation_token token;
    token.cancel();
    HookProbe probe;
    EXPECT_FALSE(token.link(probe.hook));
    EXPECT_EQ(probe.fired, 1) << "same contract as on_cancel on a cancelled token: inline";
    EXPECT_FALSE(probe.hook.linked());
}

TEST(CancellationToken, LinkOnEmptyTokenReturnsFalseAndNeverFires) {
    cancellation_token token{null_token};
    HookProbe          probe;
    EXPECT_FALSE(token.link(probe.hook));
    EXPECT_FALSE(probe.hook.linked());
    token.cancel();
    EXPECT_EQ(probe.fired, 0);
}

TEST(CancellationToken, HooksDoNotAccumulateAcrossLinkUnlinkLoop) {
    // The `qb::ask` shape: link on every suspension, unlink on every completion, on one long-lived
    // actor-scope token. The list must be empty after each round trip and nothing else may fire.
    cancellation_token token;
    for (int i = 0; i < 10000; ++i) {
        HookProbe probe;
        ASSERT_TRUE(token.link(probe.hook));
        EXPECT_FALSE(hooks_empty(token));
        probe.hook.unlink();
        EXPECT_TRUE(hooks_empty(token));
    }
    HookProbe last;
    ASSERT_TRUE(token.link(last.hook));
    token.cancel();
    EXPECT_EQ(last.fired, 1) << "only the single live hook survives to fire";
    EXPECT_TRUE(token.get_state()->callbacks.empty());
}
