/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/sibling-api-parity.cpp
 * @brief Asserts sibling coroutine APIs against EACH OTHER, and compiles the documentation.
 *
 * ## Why this file exists (the missing test class)
 *
 * Every defect this file pins was found by writing example programs, not by the suite —
 * and each was invisible to the suite for the same structural reason: **the two halves of
 * a pair were only ever tested alone.** `take()` had tests; `ag_take()` had tests; nothing
 * asked whether they agree. `from_generator` had a test, but it used an infinite source and
 * a `take(5)`, so "can this factory terminate at all?" was never a question. `collect_to_vector`
 * had six call sites, all on named lvalues, so "every sibling takes by value and this one
 * does not" never surfaced.
 *
 * A test that exercises one API against its own expectations cannot see a disagreement
 * between two APIs. This file is the counterpart: each test names a PAIR and asserts the
 * property that must hold across it. Where a pair must genuinely differ (`check_cancelled`
 * vs `yield_or_cancel`; the two `mpsc::dequeue` overloads, in
 * core/unit/lockfree/mpsc-dequeue-parity.cpp), the test pins the difference so it stays
 * deliberate and cannot drift back into an accident.
 *
 * ## Second purpose: the documentation is code here
 *
 * The other half of the root cause is that a doc example is prose to every guard in the
 * repo — `verify.sh` checks that names exist, that citations land on their symbol, that
 * digests are unchanged, that paths resolve. Nothing compiles the `@code` blocks in the
 * shipped headers, so a doc could assert a shape the API does not support and stay green
 * forever. Several did. The `DocExampleCompiles` cases below are those snippets, copied from
 * the headers they document, so that the compiler is the reviewer.
 */

#include <atomic>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

// A NAMED namespace, never an anonymous one: fixtures here are handed to framework
// templates that spawn a lambda into a frame defined in a qb header, and an anonymous
// namespace gives that frame a field of no-linkage type (-Werror=subobject-linkage).
namespace sibling_api_parity_test {

class SiblingApiParity : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/// A source that counts how many times it was actually resumed.
generator<int>
counting_source(int &pulls) {
    for (int i = 0;; ++i) {
        ++pulls; // runs once per resume, so this counts pulls exactly
        co_yield i;
    }
}

async_generator<int>
counting_async_source(int &pulls) {
    for (int i = 0;; ++i) {
        ++pulls;
        co_yield i;
    }
}

/// A finite async source yielding 0..n-1, so an async_generator fold can be compared with the
/// async_stream one over data of the same shape.
async_generator<int>
async_range(int n) {
    for (int i = 0; i < n; ++i)
        co_yield i;
}

// ===========================================================================
// PAIR 1 — take(generator) vs ag_take(async_generator)
//
// Same idea, two implementations. The property that must hold across the pair:
// taking N pulls exactly min(N, size) values from the source and NOT ONE MORE.
// Over `iota` an extra pull is free, which is why it hid; over a source with a
// side effect (a cursor row, a socket byte) it is a lost item.
// ===========================================================================

TEST_F(SiblingApiParity, TakePullsExactlyNFromASyncGenerator) {
    int  pulls = 0;
    auto taken = collect_to_vector(take(counting_source(pulls), 3));

    EXPECT_EQ(taken, (std::vector<int>{0, 1, 2}));
    // The defect: the loop resumed the source, THEN tested the limit, so a 4th value was
    // produced and silently discarded. Before the fix this reads 4.
    EXPECT_EQ(pulls, 3) << "take(gen, 3) must resume the source exactly 3 times";
}

TEST_F(SiblingApiParity, TakeZeroDoesNotTouchTheSource) {
    // begin() itself resumes the coroutine once to reach the first co_yield, so a count
    // of 0 has to return before iteration starts at all.
    int  pulls = 0;
    auto taken = collect_to_vector(take(counting_source(pulls), 0));

    EXPECT_TRUE(taken.empty());
    EXPECT_EQ(pulls, 0) << "take(gen, 0) must not resume the source at all";
}

TEST_F(SiblingApiParity, TakeAgreesWithAgTakeOnPullCount) {
    // The parity assertion proper: drive both halves of the pair over equivalent sources
    // and require the SAME observable pull count. This is the assertion no per-API test
    // could make, and the one that would have caught the defect.
    int  sync_pulls = 0;
    auto sync_out   = collect_to_vector(take(counting_source(sync_pulls), 4));

    int               async_pulls = 0;
    std::vector<int>  async_out;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        async_out = co_await ag_collect(ag_take(counting_async_source(async_pulls), 4));
        done.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "ag_take pipeline never completed";

    EXPECT_EQ(sync_out, async_out);
    EXPECT_EQ(sync_pulls, async_pulls) << "take() and ag_take() must agree on how much of the source they consume";
    EXPECT_EQ(sync_pulls, 4);
}

TEST_F(SiblingApiParity, TakeStopsEarlyOnAShortSource) {
    // The fix must not change the finite case: a source shorter than the count still ends
    // cleanly and yields everything it had.
    auto three = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    };
    EXPECT_EQ(collect_to_vector(take(three(), 10)), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(collect_to_vector(take(three(), 3)), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(collect_to_vector(take(three(), 1)), (std::vector<int>{1}));
}

TEST_F(SiblingApiParity, SkipStillConsumesWhatItSkips) {
    // `skip` is take's neighbour and is NOT symmetric: it must pull what it discards.
    // Pinned so the take() fix is not "helpfully" generalised onto it.
    int  pulls = 0;
    auto out   = collect_to_vector(take(skip(counting_source(pulls), 3), 2));
    EXPECT_EQ(out, (std::vector<int>{3, 4}));
    EXPECT_EQ(pulls, 5) << "skip(3) must consume the 3 values it drops";
}

// ===========================================================================
// PAIR 2 — collect_to_vector's parameter convention vs every sibling helper
//
// from_range / take / skip / concat all take their generator BY VALUE, so they
// compose over temporaries. collect_to_vector took a non-const lvalue reference,
// so it was the one link in the chain that could not be written.
// ===========================================================================

TEST_F(SiblingApiParity, CollectToVectorAcceptsATemporaryLikeEverySibling) {
    auto fibonacci = [](int n) -> generator<int> {
        int a = 0, b = 1;
        for (int i = 0; i < n; ++i) {
            co_yield a;
            const int next = a + b;
            a              = b;
            b              = next;
        }
    };

    // Before the rvalue overload this line did not compile at all.
    EXPECT_EQ(collect_to_vector(fibonacci(7)), (std::vector<int>{0, 1, 1, 2, 3, 5, 8}));

    // The composed form is the one that actually matters: every transform returns by
    // value, so without the overload no pipeline could be drained in one expression.
    EXPECT_EQ(collect_to_vector(take(range(0, 100), 4)), (std::vector<int>{0, 1, 2, 3}));
    EXPECT_EQ(collect_to_vector(skip(range(0, 6), 4)), (std::vector<int>{4, 5}));
    EXPECT_EQ(collect_to_vector(concat(range(0, 2), range(10, 12))), (std::vector<int>{0, 1, 10, 11}));
}

TEST_F(SiblingApiParity, CollectToVectorStillTakesAnLvalueAndLeavesItAlive) {
    // The additive overload must not disturb the existing spelling: a named generator is
    // still passed by reference and is still a live (drained) object afterwards.
    auto g = range(0, 3);
    EXPECT_EQ(collect_to_vector(g), (std::vector<int>{0, 1, 2}));
    EXPECT_FALSE(g.has_next()) << "the lvalue overload drains in place, as it always did";
    EXPECT_TRUE(collect_to_vector(g).empty()) << "draining an exhausted generator yields nothing";
}

// ===========================================================================
// PAIR 3 — from_generator vs the other async_stream factories
//
// from_vector / single / empty all terminate. from_generator wrapped f() in an
// engaged optional unconditionally, so it could only ever build an infinite
// stream — its name and its siblings both said otherwise.
// ===========================================================================

TEST_F(SiblingApiParity, FromGeneratorCanEndWhenFReturnsOptional) {
    std::vector<int>  out;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        int  counter = 0;
        auto stream  = from_generator([counter]() mutable -> std::optional<int> {
            if (counter >= 3)
                return std::nullopt; // end-of-stream — impossible to express before
            return counter++;
        });
        out          = co_await stream.collect();
        done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "finite from_generator stream never completed";
    EXPECT_EQ(out, (std::vector<int>{0, 1, 2}));
}

TEST_F(SiblingApiParity, FromGeneratorKeepsTheInfiniteMeaningForAPlainReturn) {
    // The pre-existing spelling must behave exactly as before: endless, bounded by take().
    std::vector<int>  out;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        int  counter = 0;
        auto stream  = from_generator([counter]() mutable -> int { return counter++; });
        out          = co_await stream.take(5).collect();
        done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "infinite from_generator stream never completed";
    EXPECT_EQ(out, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(SiblingApiParity, FromGeneratorAgreesWithFromVectorOnAFiniteSequence) {
    // Parity proper: the same finite sequence, built through two different factories, must
    // collect to the same thing. Before the fix the from_generator side could not be
    // written at all.
    const std::vector<int> src{7, 8, 9};

    std::vector<int>  via_vector;
    std::vector<int>  via_generator;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        via_vector = co_await async_stream<int>::from_vector(src).collect();

        std::size_t idx    = 0;
        auto        stream = from_generator([idx, src]() mutable -> std::optional<int> {
            if (idx >= src.size())
                return std::nullopt;
            return src[idx++];
        });
        via_generator      = co_await stream.collect();
        done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "one of the two streams never completed";
    EXPECT_EQ(via_vector, via_generator);
    EXPECT_EQ(via_generator, src);
}

// ===========================================================================
// PAIR 4 — with_lock / with_semaphore take a SYNCHRONOUS callable
//
// This pair must differ from the rest of the coroutine layer, and the danger was
// that the difference was silent. Handing them a coroutine compiled fine and did
// NOTHING: task<T> is lazy, so the inner task was constructed suspended, returned
// as a value, never awaited, and destroyed. A static_assert rejects it now.
// ===========================================================================

TEST_F(SiblingApiParity, LazyTaskNeverRunsUnlessAwaited) {
    // The mechanism behind the silent no-op, pinned on its own. This is WHY the
    // static_assert has to exist: nothing about an unawaited task is observable at
    // runtime, so only a compile-time rejection can catch the mistake.
    bool body_ran = false;
    {
        auto make = [&]() -> task<void> {
            body_ran = true;
            co_return;
        };
        auto t = make(); // constructed suspended; never awaited
        EXPECT_FALSE(body_ran) << "task<T> is lazy — its body must not run on construction";
    } // destroyed here, still never run
    EXPECT_FALSE(body_ran) << "an unawaited task's body never runs at all — the silent no-op";
}

TEST_F(SiblingApiParity, IsTaskTraitDrivesTheRejection) {
    // The predicate the static_assert is written against. Positive AND negative, so a
    // trait that silently stopped matching would fail here rather than quietly disarm the
    // diagnostic.
    static_assert(detail::is_task_v<task<void>>);
    static_assert(detail::is_task_v<task<int>>);
    static_assert(detail::is_task_v<task<std::string>>);
    // Legitimate return types that must NOT be rejected:
    static_assert(!detail::is_task_v<void>);
    static_assert(!detail::is_task_v<int>);
    static_assert(!detail::is_task_v<std::string>);
    static_assert(!detail::is_task_v<std::optional<int>>);
    static_assert(!detail::is_task_v<std::vector<int>>);
    static_assert(!detail::is_task_v<generator<int>>);
    SUCCEED();
}

struct ReturnsFive {
    int
    operator()() const {
        return 5;
    }
};

TEST_F(SiblingApiParity, WithLockAndWithSemaphoreStillAcceptEveryLegitimateShape) {
    // The other half of the negative proof: confirm the new diagnostic rejects nothing it
    // should accept. Value-returning, void-returning, mutable, and a functor object.
    async_mutex       mtx;
    semaphore         sem(1);
    int               sink = 0;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        const auto a = co_await with_lock(mtx, [] { return 1; });                      // value
        const auto b = co_await with_semaphore(sem, [] { return std::string("xy"); }); // non-trivial value
        co_await with_lock(mtx, [&] { sink += 10; });                                  // void
        int        counter = 0;
        const auto c       = co_await with_semaphore(sem, [counter]() mutable { return ++counter; }); // mutable
        const auto d       = co_await with_lock(mtx, ReturnsFive{});                                  // functor object
        sink += a + static_cast<int>(b.size()) + c + d;
        done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "with_lock/with_semaphore pipeline never completed";
    EXPECT_EQ(sink, 19); // 10 + 1 + 2 + 1 + 5
}

// ===========================================================================
// PAIR 5 — check_cancelled vs yield_or_cancel
//
// These two MUST differ, and the trap is that the name says otherwise:
// check_cancelled reads like a poll and is a wait. The pair is pinned so the
// difference stays deliberate rather than becoming a surprise again.
// ===========================================================================

TEST_F(SiblingApiParity, CheckCancelledWaitsWhileYieldOrCancelFallsThrough) {
    cancellation_token token;

    std::atomic<bool> past_yield{false};
    std::atomic<bool> past_check{false};
    std::atomic<bool> check_threw{false};

    // yield_or_cancel on a LIVE token: re-enqueues and resumes, so execution continues.
    coro_scheduler().spawn([&]() -> task<void> {
        co_await yield_or_cancel(token);
        past_yield.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return past_yield.load(); })) << "yield_or_cancel never resumed";

    // check_cancelled on the SAME live token: parks, and must NOT fall through.
    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await check_cancelled(token);
            past_check.store(true); // only reachable if it wrongly behaved as a poll
        } catch (cancelled_error const &) {
            check_threw.store(true);
        }
    });
    EXPECT_FALSE(pump_until([&] { return past_check.load(); }, 120ms))
        << "check_cancelled must SUSPEND on a live token, not fall through like a poll";
    EXPECT_FALSE(past_check.load());

    // The non-suspending poll that does exist, for the same question.
    EXPECT_NO_THROW(token.throw_if_cancelled());

    // Cancelling is what completes the parked branch — the whole point of the awaiter.
    token.cancel();
    EXPECT_TRUE(pump_until([&] { return check_threw.load(); })) << "cancel() must wake the parked check_cancelled";
    EXPECT_FALSE(past_check.load());
    EXPECT_THROW(token.throw_if_cancelled(), cancelled_error);
}

// ===========================================================================
// Compiled documentation — @code blocks, from the headers they document
//
// These exist so the compiler reviews the documentation. Each corresponds to a
// snippet a reader is invited to copy; if the API drifts away from what the doc
// claims, this file stops building instead of staying quietly wrong.
// ===========================================================================

int
compute() {
    return 42;
}

task<void>
doc_work() {
    co_return;
}

/// From qb/io/async/coroutine/DOCUMENTATION_GUIDE.md — "RAII helpers".
TEST_F(SiblingApiParity, DocExampleCompilesSyncPrimitivesRaiiHelpers) {
    async_mutex       mtx;
    semaphore         sem(1);
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await with_semaphore(sem, [] { return compute(); });
        co_await with_lock(mtx, [] { return compute(); });
        {
            auto g = co_await mtx.scoped_lock();
            co_await doc_work();
        }
        {
            auto g = co_await sem.scoped_acquire();
            co_await doc_work();
        }
        done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "documented RAII-helper sequence never completed";
}

/// From qb/io/async/coroutine/DOCUMENTATION_GUIDE.md — "Sync helpers".
TEST_F(SiblingApiParity, DocExampleCompilesGeneratorSyncHelpers) {
    auto fibonacci = [](int n) -> generator<int> {
        int a = 0, b = 1;
        for (int i = 0; i < n; ++i) {
            co_yield a;
            const int next = a + b;
            a              = b;
            b              = next;
        }
    };
    const std::vector<int> my_vector{1, 2, 3};

    auto vec  = collect_to_vector(fibonacci(10));
    auto gen  = from_range(my_vector);
    auto gen2 = range(0, 100);
    auto gen3 = iota(0); // infinite

    EXPECT_EQ(vec.size(), 10u);
    EXPECT_EQ(collect_to_vector(gen), my_vector);
    EXPECT_TRUE(gen2.has_next());
    EXPECT_TRUE(gen3.has_next());
}

/// From qb/io/async/coroutine/cancellation.h — the check_cancelled @code block.
TEST_F(SiblingApiParity, DocExampleCompilesCancellationIdioms) {
    cancellation_token token;
    async_event        ev;
    std::atomic<bool>  raced{false};
    std::atomic<bool>  looped{false};
    int                steps = 0;

    coro_scheduler().spawn([&]() -> task<void> {
        // Named locals, not temporaries in the when_any(...) full-expression: task's
        // initial_suspend is suspend_always, so each body starts on a later run_ready().
        auto wait_op = [&ev]() -> task<void> {
            co_await ev.wait();
        };
        auto cancel_op = [token]() -> task<void> {
            co_await check_cancelled(token);
        };
        auto result = co_await when_any(wait_op(), cancel_op());
        EXPECT_EQ(result.index, 0u) << "the event branch must win — ev.set() happens, cancel never does";
        raced.store(true);
    });

    coro_scheduler().spawn([&]() -> task<void> {
        while (steps < 3) {
            co_await yield_or_cancel(token);
            ++steps;
        }
        looped.store(true);
    });

    token.throw_if_cancelled(); // the non-suspending poll
    ev.set();

    ASSERT_TRUE(pump_until([&] { return raced.load() && looped.load(); })) << "documented cancellation idioms never completed";
    EXPECT_EQ(steps, 3);
}

// ===========================================================================
// PAIR 6 — ag_reduce(gen, init, f) vs async_stream::reduce(init, f)
//
// One fold, two spellings, and until 3.0 they disagreed twice over: the stream
// method took (f, initial) while ag_reduce took (init, reducer) — the order
// std::accumulate and std::ranges::fold_left also use — and the stream method
// pinned the accumulator to T, the element type, so folding into anything else
// was simply not expressible. Neither divergence could be seen from inside
// either test, because each family was only ever tested against itself.
// ===========================================================================

TEST_F(SiblingApiParity, ReduceTakesTheSeedFirstInBothFamilies) {
    std::atomic<bool> done{false};
    int               via_stream = -1;
    int               via_ag     = -1;

    coro_scheduler().spawn([&]() -> task<void> {
        // SAME data, SAME seed, SAME reducer, identical argument order — so equality is the
        // parity assertion rather than two constants that could each be wrong on their own.
        // `acc * 2 + v` is neither commutative nor associative, so a reversed fold or a
        // swapped seed changes the answer instead of hiding in it.
        via_stream = co_await async_stream<int>::from_vector({0, 1, 2, 3}).reduce(100, [](int acc, int v) { return acc * 2 + v; });
        via_ag     = co_await ag_reduce(async_range(4), 100, [](int acc, int v) { return acc * 2 + v; });
        done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "reduce parity pipeline never completed";
    EXPECT_EQ(via_stream, via_ag) << "the two spellings of one fold must agree";
    // ((((100*2+0)*2+1)*2+2)*2+3) = 1611
    EXPECT_EQ(via_stream, 1611) << "async_stream::reduce must fold left from the seed";
}

TEST_F(SiblingApiParity, ReduceAcceptsAnAccumulatorUnlikeTheElementType) {
    // The capability half. ag_reduce always allowed Acc != T; async_stream::reduce returned
    // task<T> and therefore could not fold ints into a string, a checksum or a count-with-
    // context. Nothing in the suite noticed, because nobody had tried to write it.
    std::atomic<bool> done{false};
    std::string       joined_stream;
    std::string       joined_ag;

    coro_scheduler().spawn([&]() -> task<void> {
        joined_stream = co_await async_stream<int>::from_vector({0, 1, 2}).reduce(std::string{}, [](std::string acc, int v) {
            if (!acc.empty())
                acc += ",";
            return acc + std::to_string(v);
        });
        joined_ag     = co_await ag_reduce(async_range(3), std::string{}, [](std::string acc, int v) {
            if (!acc.empty())
                acc += ",";
            return acc + std::to_string(v);
        });
        done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "heterogeneous reduce never completed";
    EXPECT_EQ(joined_stream, joined_ag) << "both families must fold the same data into the same accumulator";
    EXPECT_EQ(joined_stream, "0,1,2") << "async_stream::reduce must fold into an arbitrary accumulator";
}

} // namespace sibling_api_parity_test
