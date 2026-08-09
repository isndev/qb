/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/stream-transforms.cpp
 * @brief `qb::io::async::async_stream<T>` deterministic data pipeline — factories/transforms/terminals.
 *
 * The unit half of the former test-coroutine-stream.cpp split (the clock/loop-driven
 * factories — timer/interval/throttle/backpressure and live channel producers — live in
 * system/coroutine/stream-async-sources.cpp). Everything here operates over *scripted,
 * deterministic in-memory data* (from_vector / range_stream / repeat_value / from_generator /
 * single / empty) and the pure functional operators built on it — map/filter/take/skip/chain/
 * buffer transforms, the count/first/for_each/any/all/find/reduce terminals, and the
 * merge_streams/zip combinators. No timers, no sleeps, no wall-clock spacing: the only reason
 * these run on the event loop at all is that the terminal consumers are coroutines, so each
 * test pumps the loop until completion via the shared `qb::io::test::pump_until` and then
 * asserts the exact result.
 *
 * Correctness gap closed: the original Creation/Transform/Operation/Utility tests asserted
 * *inside the coroutine body* with NO post-pump `done` flag — if the coroutine silently never
 * ran, the in-body `EXPECT_*` never fired and the test passed vacuously green. Every test here
 * now hoists its result out and asserts an explicit `done` flag AFTER `pump_until`, so a
 * stream that fails to run fails the test loudly. Merges the deterministic cases from the
 * former test-coroutine-stream-advanced.cpp (suspending for_each, the Record filter→map→reduce
 * pipeline, merge/zip) and re-homes the deterministic stream regressions
 * (collect-gathers-all, map+filter pipeline, zip-short-circuit).
 */

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace stream_transforms_test {

class StreamTransforms : public ::testing::Test {
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

struct Record {
    int         id{};
    std::string name;
};

} // namespace stream_transforms_test
using namespace stream_transforms_test;

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

TEST_F(StreamTransforms, FromVectorRoundTripsTheVector) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "from_vector collect never ran";
    EXPECT_EQ(result, (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST_F(StreamTransforms, EmptyStreamYieldsNothing) {
    std::vector<int>  result{99}; // seeded non-empty so a non-running coroutine fails
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::empty().collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "empty stream collect never ran";
    EXPECT_TRUE(result.empty());
}

TEST_F(StreamTransforms, SingleStreamYieldsOneValue) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::single(42).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "single stream collect never ran";
    EXPECT_EQ(result, (std::vector<int>{42}));
}

TEST_F(StreamTransforms, RangeStreamYieldsHalfOpenInterval) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await range_stream(5, 10).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "range_stream collect never ran";
    EXPECT_EQ(result, (std::vector<int>{5, 6, 7, 8, 9}));
}

TEST_F(StreamTransforms, RepeatValueWithTakeYieldsFixedCount) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await repeat_value(7).take(3).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "repeat_value take collect never ran";
    EXPECT_EQ(result, (std::vector<int>{7, 7, 7}));
}

TEST_F(StreamTransforms, FromGeneratorSyncCounterTakesN) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        int  counter = 0;
        auto stream  = from_generator([counter]() mutable -> int { return counter++; });
        result       = co_await stream.take(5).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "from_generator collect never ran";
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

// ---------------------------------------------------------------------------
// Transforms: map / filter / take / skip / chain / buffer
// ---------------------------------------------------------------------------

TEST_F(StreamTransforms, MapAppliesFunctionToEachElement) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3}).map([](int x) { return x * 2; }).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "map collect never ran";
    EXPECT_EQ(result, (std::vector<int>{2, 4, 6}));
}

TEST_F(StreamTransforms, FilterKeepsOnlyMatchingElements) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).filter([](int x) { return x % 2 == 0; }).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "filter collect never ran";
    EXPECT_EQ(result, (std::vector<int>{2, 4}));
}

TEST_F(StreamTransforms, TakeLimitsElementCount) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await range_stream(0, 100).take(5).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "take collect never ran";
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(StreamTransforms, SkipDropsFirstN) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await range_stream(0, 10).skip(7).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "skip collect never ran";
    EXPECT_EQ(result, (std::vector<int>{7, 8, 9}));
}

TEST_F(StreamTransforms, ChainedFilterMapTake) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5, 6, 7, 8, 9, 10})
                     .filter([](int x) { return x % 2 == 0; }) // 2,4,6,8,10
                     .map([](int x) { return x * x; })         // 4,16,36,64,100
                     .take(3)                                  // 4,16,36
                     .collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "chained transform collect never ran";
    EXPECT_EQ(result, (std::vector<int>{4, 16, 36}));
}

TEST_F(StreamTransforms, DeepChainedMapFilterSkipTake) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5, 6, 7, 8, 9, 10})
                     .map([](int v) { return v * 2; })         // 2..20
                     .filter([](int v) { return v % 4 == 0; }) // 4,8,12,16,20
                     .skip(1)                                  // 8,12,16,20
                     .take(3)                                  // 8,12,16
                     .collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "deep chained transform collect never ran";
    EXPECT_EQ(result, (std::vector<int>{8, 12, 16}));
}

TEST_F(StreamTransforms, ChainConcatenatesTwoStreams) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto s1 = async_stream<int>::from_vector({1, 2, 3});
        auto s2 = async_stream<int>::from_vector({4, 5, 6});
        result  = co_await s1.chain(std::move(s2)).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "chain collect never ran";
    EXPECT_EQ(result, (std::vector<int>{1, 2, 3, 4, 5, 6}));
}

TEST_F(StreamTransforms, BufferBatchesElements) {
    std::vector<std::vector<int>> batches;
    std::atomic<bool>             done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        batches = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5, 6, 7}).buffer(3).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "buffer collect never ran";
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0], (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(batches[1], (std::vector<int>{4, 5, 6}));
    EXPECT_EQ(batches[2], (std::vector<int>{7}));
}

// ---------------------------------------------------------------------------
// Terminals: count / first / for_each / any / all / find / reduce
// ---------------------------------------------------------------------------

TEST_F(StreamTransforms, CountReturnsElementCount) {
    size_t            count = 0;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        count = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).count();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "count never ran";
    EXPECT_EQ(count, 5u);
}

TEST_F(StreamTransforms, FirstReturnsFrontElement) {
    std::optional<int> first;
    std::atomic<bool>  done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        first = co_await async_stream<int>::from_vector({42, 2, 3}).first();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "first never ran";
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 42);
}

TEST_F(StreamTransforms, FirstOnEmptyStreamReturnsNullopt) {
    std::optional<int> first{7}; // seeded so a non-running coroutine fails
    std::atomic<bool>  done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        first = co_await async_stream<int>::empty().first();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "first-on-empty never ran";
    EXPECT_FALSE(first.has_value());
}

TEST_F(StreamTransforms, ForEachVisitsEachElement) {
    int               sum = 0;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        co_await async_stream<int>::from_vector({1, 2, 3}).for_each([&sum](int x) { sum += x; });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "for_each never ran";
    EXPECT_EQ(sum, 6);
}

TEST_F(StreamTransforms, ForEachWithSuspendingCallbackVisitsInOrder) {
    // Merged from the former stream-advanced suite: the async callback yields between items.
    std::vector<int>  visited;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        co_await range_stream(0, 4).for_each([&](int value) -> task<void> {
            co_await sleep(0ms);
            visited.push_back(value);
        });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "suspending for_each never ran";
    EXPECT_EQ(visited, (std::vector<int>{0, 1, 2, 3}));
}

TEST_F(StreamTransforms, AnyReflectsPredicateMatch) {
    bool              has_even = false, has_negative = true;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        has_even     = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).any([](int x) { return x % 2 == 0; });
        has_negative = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).any([](int x) { return x < 0; });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "any never ran";
    EXPECT_TRUE(has_even);
    EXPECT_FALSE(has_negative);
}

TEST_F(StreamTransforms, AllReflectsPredicateMatch) {
    bool              all_even = false, all_positive = false;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        all_even     = co_await async_stream<int>::from_vector({2, 4, 6, 8}).all([](int x) { return x % 2 == 0; });
        all_positive = co_await async_stream<int>::from_vector({2, 4, 6, 8}).all([](int x) { return x > 0; });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "all never ran";
    EXPECT_TRUE(all_even);
    EXPECT_TRUE(all_positive);
}

TEST_F(StreamTransforms, FindReturnsFirstMatchOrNullopt) {
    std::optional<int> found, not_found{0};
    std::atomic<bool>  done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        found     = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).find([](int x) { return x > 3; });
        not_found = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).find([](int x) { return x > 10; });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "find never ran";
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, 4);
    EXPECT_FALSE(not_found.has_value());
}

TEST_F(StreamTransforms, ReduceAggregatesWithFunction) {
    int               sum = -1;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        sum = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).reduce([](int a, int b) { return a + b; }, 0);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "reduce never ran";
    EXPECT_EQ(sum, 15);
}

TEST_F(StreamTransforms, ComplexRecordsSurviveFilterMapReducePipeline) {
    // Merged from the former stream-advanced suite: non-trivial element type through the chain.
    std::string       joined;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<Record> records{{1, "skip"}, {2, "alpha"}, {3, "skip"}, {4, "beta"}};
        joined = co_await async_stream<Record>::from_vector(records)
                     .filter([](const Record &r) { return r.id % 2 == 0; })
                     .map([](Record r) { return r.name; })
                     .reduce(
                         [](std::string acc, std::string value) {
                             if (!acc.empty())
                                 acc += ",";
                             acc += value;
                             return acc;
                         },
                         std::string{});
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "record pipeline never ran";
    EXPECT_EQ(joined, "alpha,beta");
}

TEST_F(StreamTransforms, MapFilterPipelineProducesExactValues) {
    // Re-homed deterministic stream regression.
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5, 6})
                     .map([](int v) { return v * 2; })
                     .filter([](int v) { return v > 6; })
                     .collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "map+filter pipeline never ran";
    EXPECT_EQ(result, (std::vector<int>{8, 10, 12}));
}

// ---------------------------------------------------------------------------
// Combinators: merge_streams / zip
// ---------------------------------------------------------------------------

TEST_F(StreamTransforms, MergeStreamsInterleavesRoundRobin) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<async_stream<int>> vec;
        vec.push_back(async_stream<int>::from_vector({1, 3, 5}));
        vec.push_back(async_stream<int>::from_vector({2, 4, 6}));
        result = co_await merge_streams(std::move(vec)).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "merge_streams never ran";
    EXPECT_EQ(result, (std::vector<int>{1, 2, 3, 4, 5, 6})) << "round-robin interleave of two equal-length streams";
}

TEST_F(StreamTransforms, MergeEmptyStreamsYieldsNothing) {
    std::vector<int>  result{1}; // seeded so a non-running coroutine fails
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<async_stream<int>> vec;
        result = co_await merge_streams(std::move(vec)).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "merge of empty stream-set never ran";
    EXPECT_TRUE(result.empty());
}

TEST_F(StreamTransforms, ZipPairsElements) {
    std::vector<std::pair<int, std::string>> result;
    std::atomic<bool>                        done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto s1 = async_stream<int>::from_vector({1, 2, 3});
        auto s2 = async_stream<std::string>::from_vector({"a", "b", "c"});
        result  = co_await zip(std::move(s1), std::move(s2)).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "zip never ran";
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], std::make_pair(1, std::string{"a"}));
    EXPECT_EQ(result[2], std::make_pair(3, std::string{"c"}));
}

TEST_F(StreamTransforms, ZipStopsAtShorterStreamSecond) {
    std::vector<std::pair<int, int>> result;
    std::atomic<bool>                done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto s1 = async_stream<int>::from_vector({1, 2, 3, 4, 5}); // longer
        auto s2 = async_stream<int>::from_vector({10, 20});        // shorter (second)
        result  = co_await zip(std::move(s1), std::move(s2)).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "zip (shorter second) never ran";
    EXPECT_EQ(result, (std::vector<std::pair<int, int>>{{1, 10}, {2, 20}}));
}

TEST_F(StreamTransforms, ZipShortCircuitsWhenFirstStreamEndsFirst) {
    // Re-homed regression (finding 2.C.16): when the FIRST stream is the shorter one, zip
    // bails as soon as it ends and never blocks pulling the (longer) second stream further.
    std::vector<std::pair<int, int>> result;
    std::atomic<bool>                done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto shorter = async_stream<int>::from_vector({1, 2, 3}); // first, shorter
        auto longer  = async_stream<int>::from_vector({10, 11, 12, 13, 14});
        result       = co_await zip(std::move(shorter), std::move(longer)).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "zip (shorter first) never ran";
    EXPECT_EQ(result, (std::vector<std::pair<int, int>>{{1, 10}, {2, 11}, {3, 12}}));
}

// ---------------------------------------------------------------------------
// Degenerate transform arguments fail loudly at the call site (pure sync, no scheduler).
// ---------------------------------------------------------------------------

TEST_F(StreamTransforms, BufferZeroThrows) {
    // batch_size 0 → the fill loop `while (size < 0)` never pulls the source → silent total data loss.
    auto src = async_stream<int>([]() -> task<std::optional<int>> { co_return std::nullopt; });
    EXPECT_THROW({ [[maybe_unused]] auto &&discarded_ = src.buffer(0); }, std::invalid_argument);
    EXPECT_NO_THROW({ [[maybe_unused]] auto &&discarded_ = src.buffer(1); });
}

TEST_F(StreamTransforms, BackpressureZeroThrows) {
    // max_buffer 0 with no caller semaphore → a 0-permit semaphore → permanent deadlock.
    auto src = async_stream<int>([]() -> task<std::optional<int>> { co_return std::nullopt; });
    EXPECT_THROW({ [[maybe_unused]] auto &&discarded_ = src.backpressure(0); }, std::invalid_argument);
    // A caller-supplied semaphore drives backpressure itself → a 0 buffer (pure rendezvous) is legal.
    EXPECT_NO_THROW({ [[maybe_unused]] auto &&discarded_ = src.backpressure(0, std::make_shared<semaphore>(1)); });
    EXPECT_NO_THROW({ [[maybe_unused]] auto &&discarded_ = src.backpressure(4); });
}
