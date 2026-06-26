/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/generator-sync.cpp
 * @brief `qb::io::async::generator<T>` synchronous lazy sequences — pure logic, NO event loop.
 *
 * The synchronous half of the former test-coroutine-generator.cpp split (the suspending
 * `async_generator<T>` + `ag_*` combinators live in unit/coroutine/generator-async.cpp).
 * A `generator<T>` is a plain C++20 coroutine that `co_yield`s values lazily and is driven
 * purely by the consumer — range-for, `begin()/end()`, or `has_next()/next()`. NOTHING here
 * touches the qb-io scheduler, a timer, or the event loop: these are deterministic
 * pure-logic unit tests with no `init()`, no `run_for`, no `pump_until`.
 *
 * Covered: the `co_yield` body (simple/fibonacci/empty/single), the factory library
 * (`single_generator`, `range`, `iota`, `repeat`, `repeat_n`, `from_range`, `from_iterator`),
 * the transform helpers (`take`, `skip`, `concat`), the drain helper `collect_to_vector`, and
 * the iteration surfaces (`begin/end`, `has_next/next` with the post-exhaustion empty-optional
 * contract). `iota`/`repeat` are infinite, so they are consumed with an explicit break.
 *
 * Strengthened over the original: the move-semantics test now compares the FULL element
 * vector after the move (not just `.size()`), so a move that silently dropped or duplicated an
 * element fails; the redundant `ManualIteration` vs `GeneratorHasNextAndNext` pair is unified
 * into one `next()`-contract test. The shipped `QB_DEBUG_AGEN`/`QB_DEBUG_SCOPE`/`TLOG`
 * instrumentation from the original file is dropped entirely (it belonged to the async half).
 */

#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

using namespace qb::io::async;

// ---------------------------------------------------------------------------
// co_yield bodies
// ---------------------------------------------------------------------------

TEST(GeneratorSync, SimpleGeneratorYieldsSequentially) {
    auto gen = [](int n) -> generator<int> {
        for (int i = 0; i < n; ++i)
            co_yield i;
    };

    std::vector<int> result;
    for (auto val : gen(5))
        result.push_back(val);

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(GeneratorSync, FibonacciGenerator) {
    auto fibonacci = [](int n) -> generator<int> {
        int a = 0, b = 1;
        for (int i = 0; i < n; ++i) {
            co_yield a;
            int next = a + b;
            a        = b;
            b        = next;
        }
    };

    std::vector<int> result;
    for (auto val : fibonacci(8))
        result.push_back(val);

    EXPECT_EQ(result, (std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13}));
}

TEST(GeneratorSync, EmptyGeneratorYieldsNothing) {
    auto gen = []() -> generator<int> { co_return; };

    std::vector<int> result;
    for (auto val : gen())
        result.push_back(val);

    EXPECT_TRUE(result.empty());
}

TEST(GeneratorSync, SingleValueGenerator) {
    auto gen = single_generator(42);

    std::vector<int> result;
    for (auto val : gen)
        result.push_back(val);

    EXPECT_EQ(result, (std::vector<int>{42}));
}

// ---------------------------------------------------------------------------
// Range / repeat factories
// ---------------------------------------------------------------------------

TEST(GeneratorSync, RangeYieldsHalfOpenInterval) {
    std::vector<int> result;
    for (auto val : range(5, 10))
        result.push_back(val);

    EXPECT_EQ(result, (std::vector<int>{5, 6, 7, 8, 9}));
}

TEST(GeneratorSync, IotaIsInfiniteAndBreakable) {
    auto gen = iota(10);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
        if (result.size() >= 5)
            break;
    }

    EXPECT_EQ(result, (std::vector<int>{10, 11, 12, 13, 14}));
}

TEST(GeneratorSync, RepeatIsInfiniteAndBreakable) {
    auto gen = repeat(7);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
        if (result.size() >= 3)
            break;
    }

    EXPECT_EQ(result, (std::vector<int>{7, 7, 7}));
}

TEST(GeneratorSync, RepeatNYieldsExactlyNTimes) {
    std::vector<int> result;
    for (auto val : repeat_n(5, 3))
        result.push_back(val);

    EXPECT_EQ(result, (std::vector<int>{5, 5, 5}));
}

// ---------------------------------------------------------------------------
// Transforms: take / skip / concat
// ---------------------------------------------------------------------------

TEST(GeneratorSync, TakeLimitsElementCount) {
    auto gen = take(iota(0), 5);

    std::vector<int> result;
    for (auto val : gen)
        result.push_back(val);

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(GeneratorSync, SkipDropsFirstN) {
    auto gen = skip(iota(0), 5);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
        if (result.size() >= 3)
            break;
    }

    EXPECT_EQ(result, (std::vector<int>{5, 6, 7}));
}

TEST(GeneratorSync, ConcatChainsTwoGenerators) {
    auto combined = concat(range(0, 3), range(10, 13));

    std::vector<int> result;
    for (auto val : combined)
        result.push_back(val);

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 10, 11, 12}));
}

// ---------------------------------------------------------------------------
// Drain helpers: collect_to_vector / from_range / from_iterator
// ---------------------------------------------------------------------------

TEST(GeneratorSync, CollectToVectorDrainsGenerator) {
    auto gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    };

    auto g   = gen();
    auto vec = collect_to_vector(g);

    EXPECT_EQ(vec, (std::vector<int>{1, 2, 3}));
}

TEST(GeneratorSync, FromRangeCopiesContainerElements) {
    std::vector<int> source{10, 20, 30};

    std::vector<int> result;
    for (auto val : from_range(source))
        result.push_back(val);

    EXPECT_EQ(result, source);
}

TEST(GeneratorSync, FromRangeWithTemporaryDoesNotDangle) {
    // Regression (finding 2.A.8): from_range takes the range BY VALUE so a temporary
    // container is copied into the coroutine frame and does not dangle at the first suspend.
    auto gen = qb::io::async::from_range(std::vector<int>{1, 2, 3, 4, 5});
    int  sum = 0;
    while (auto v = gen.next())
        sum += *v;
    EXPECT_EQ(sum, 15);
}

TEST(GeneratorSync, FromIteratorYieldsRange) {
    std::vector<int> data{10, 20, 30, 40};
    auto             gen    = from_iterator(data.begin(), data.end());
    auto             result = collect_to_vector(gen);
    EXPECT_EQ(result, (std::vector<int>{10, 20, 30, 40}));
}

TEST(GeneratorSync, FromIteratorEmptyRangeYieldsNothing) {
    std::vector<int> data;
    auto             gen    = from_iterator(data.begin(), data.end());
    auto             result = collect_to_vector(gen);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// Iteration surfaces: begin/end, has_next/next
// ---------------------------------------------------------------------------

TEST(GeneratorSync, IteratorSteppingReachesEnd) {
    auto finite_gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
    };

    auto gen = finite_gen();
    auto it  = gen.begin();
    ASSERT_NE(it, gen.end());
    EXPECT_EQ(*it, 1);
    ++it;
    EXPECT_EQ(*it, 2);
    ++it;
    EXPECT_EQ(it, gen.end());
}

TEST(GeneratorSync, HasNextAndNextHonorExhaustionContract) {
    // Unifies the former duplicate ManualIteration + GeneratorHasNextAndNext tests: drive a
    // generator entirely through has_next()/next() and pin the post-exhaustion contract
    // (next() returns an empty optional and has_next() is false after the last value).
    auto gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
    };

    auto g = gen();

    EXPECT_TRUE(g.has_next());
    auto v1 = g.next();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 1);

    EXPECT_TRUE(g.has_next());
    auto v2 = g.next();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 2);

    // Past the last value: next() yields nullopt and the generator reports done.
    auto v3 = g.next();
    EXPECT_FALSE(v3.has_value());
    EXPECT_FALSE(g.has_next());
}

// ---------------------------------------------------------------------------
// Move semantics — full-element verification (not just size)
// ---------------------------------------------------------------------------

TEST(GeneratorSync, MoveTransfersTheEntireSequence) {
    auto gen1 = range(1, 4);
    auto gen2 = std::move(gen1); // moved-from gen1 must own nothing; gen2 owns the frame
    auto result = collect_to_vector(gen2);
    EXPECT_EQ(result, (std::vector<int>{1, 2, 3})) << "the moved-to generator must yield the full original sequence, "
                                                      "in order, with no dropped or duplicated element";
}

// ---------------------------------------------------------------------------
// Regression: basic sync generator produces values
// ---------------------------------------------------------------------------

TEST(GeneratorSync, GeneratorProducesValuesRegression) {
    auto gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    };

    auto g      = gen();
    auto values = collect_to_vector(g);
    EXPECT_EQ(values, (std::vector<int>{1, 2, 3}));
}
