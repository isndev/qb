/**
 * @file test_coroutine_generator.cpp
 * @brief Generator coroutine tests
 *
 * Tests for:
 * - generator: sync generator with co_yield
 * - async_generator: async generator
 * - utility generators (range, iota, etc.)
 *
 * @author qb - C++ Actor Framework
 */

// Enable async_generator and ag_* helper traces
#define QB_DEBUG_AGEN 1
// Enable scheduler traces
#define QB_DEBUG_SCOPE 1

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <vector>
#include <cstdio>

using namespace qb::io::async;
using namespace std::chrono_literals;

#define TLOG(fmt, ...) std::fprintf(stderr, "[test ] " fmt "\n", ##__VA_ARGS__)

// =============================================================================
// TEST SUITE: Basic Generator
// =============================================================================

class GeneratorBasicTests : public ::testing::Test {};

/**
 * @test Simple generator
 * @brief Yields values sequentially
 */
TEST_F(GeneratorBasicTests, SimpleGenerator) {
    auto gen = [](int n) -> generator<int> {
        for (int i = 0; i < n; ++i) {
            co_yield i;
        }
    };

    std::vector<int> result;
    for (auto val : gen(5)) {
        result.push_back(val);
    }

    EXPECT_EQ(result.size(), 5);
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

/**
 * @test Fibonacci generator
 * @brief Classic fibonacci sequence
 */
TEST_F(GeneratorBasicTests, FibonacciGenerator) {
    auto fibonacci = [](int n) -> generator<int> {
        int a = 0, b = 1;
        for (int i = 0; i < n; ++i) {
            co_yield a;
            int next = a + b;
            a = b;
            b = next;
        }
    };

    std::vector<int> result;
    for (auto val : fibonacci(8)) {
        result.push_back(val);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13}));
}

/**
 * @test Empty generator
 * @brief Yields nothing
 */
TEST_F(GeneratorBasicTests, EmptyGenerator) {
    auto gen = []() -> generator<int> {
        co_return;
    };

    std::vector<int> result;
    for (auto val : gen()) {
        result.push_back(val);
    }

    EXPECT_TRUE(result.empty());
}

/**
 * @test Single value generator
 * @brief Yields one value
 */
TEST_F(GeneratorBasicTests, SingleValueGenerator) {
    auto gen = single_generator(42);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
    }

    EXPECT_EQ(result, (std::vector<int>{42}));
}

// =============================================================================
// TEST SUITE: Range Generators
// =============================================================================

class RangeGeneratorTests : public ::testing::Test {};

/**
 * @test Range generator
 * @brief Generates range [start, end)
 */
TEST_F(RangeGeneratorTests, RangeGenerator) {
    std::vector<int> result;
    for (auto val : range(5, 10)) {
        result.push_back(val);
    }

    EXPECT_EQ(result, (std::vector<int>{5, 6, 7, 8, 9}));
}

/**
 * @test Iota generator
 * @brief Infinite sequence starting from value
 */
TEST_F(RangeGeneratorTests, IotaGenerator) {
    auto gen = iota(10);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
        if (result.size() >= 5) break;
    }

    EXPECT_EQ(result, (std::vector<int>{10, 11, 12, 13, 14}));
}

/**
 * @test Repeat generator
 * @brief Repeats value infinitely
 */
TEST_F(RangeGeneratorTests, RepeatGenerator) {
    auto gen = repeat(7);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
        if (result.size() >= 3) break;
    }

    EXPECT_EQ(result, (std::vector<int>{7, 7, 7}));
}

/**
 * @test Repeat N generator
 * @brief Repeats value N times
 */
TEST_F(RangeGeneratorTests, RepeatNGenerator) {
    std::vector<int> result;
    for (auto val : repeat_n(5, 3)) {
        result.push_back(val);
    }

    EXPECT_EQ(result, (std::vector<int>{5, 5, 5}));
}

// =============================================================================
// TEST SUITE: Generator Transformations
// =============================================================================

class GeneratorTransformTests : public ::testing::Test {};

/**
 * @test Take generator
 * @brief Limit number of elements
 */
TEST_F(GeneratorTransformTests, TakeGenerator) {
    auto gen = take(iota(0), 5);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

/**
 * @test Skip generator
 * @brief Skip first N elements
 */
TEST_F(GeneratorTransformTests, SkipGenerator) {
    auto gen = skip(iota(0), 5);

    std::vector<int> result;
    for (auto val : gen) {
        result.push_back(val);
        if (result.size() >= 3) break;
    }

    EXPECT_EQ(result, (std::vector<int>{5, 6, 7}));
}

/**
 * @test Concat generators
 * @brief Chain two generators
 */
TEST_F(GeneratorTransformTests, ConcatGenerators) {
    auto gen1 = range(0, 3);
    auto gen2 = range(10, 13);
    auto combined = concat(std::move(gen1), std::move(gen2));

    std::vector<int> result;
    for (auto val : combined) {
        result.push_back(val);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 10, 11, 12}));
}

// =============================================================================
// TEST SUITE: Generator Utilities
// =============================================================================

class GeneratorUtilityTests : public ::testing::Test {};

/**
 * @test Collect to vector
 * @brief Drain generator to vector
 */
TEST_F(GeneratorUtilityTests, CollectToVector) {
    auto gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    };

    auto g = gen();
    auto vec = collect_to_vector(g);

    EXPECT_EQ(vec, (std::vector<int>{1, 2, 3}));
}

/**
 * @test From range
 * @brief Create generator from container
 */
TEST_F(GeneratorUtilityTests, FromRange) {
    std::vector<int> source{10, 20, 30};

    std::vector<int> result;
    for (auto val : from_range(source)) {
        result.push_back(val);
    }

    EXPECT_EQ(result, source);
}

/**
 * @test Generator has_next and next
 * @brief Manual iteration
 */
TEST_F(GeneratorUtilityTests, ManualIteration) {
    auto gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
    };

    auto g = gen();

    EXPECT_TRUE(g.has_next());
    auto val1 = g.next();
    EXPECT_TRUE(val1.has_value());
    EXPECT_EQ(*val1, 1);

    EXPECT_TRUE(g.has_next());
    auto val2 = g.next();
    EXPECT_TRUE(val2.has_value());
    EXPECT_EQ(*val2, 2);

    // After consuming the last value, one more next() returns empty and generator is done
    auto val3 = g.next();
    EXPECT_FALSE(val3.has_value());
    EXPECT_FALSE(g.has_next());
}

// =============================================================================
// TEST SUITE: async_generator<T> helpers (ag_*)
// =============================================================================

class AsyncGeneratorHelpersTests : public ::testing::Test {
protected:
    void SetUp() override {
        TLOG("SetUp");
        qb::io::async::init();
    }
    void TearDown() override {
        TLOG("TearDown");
        qb::io::async::listener::current.clear();
    }
};

static async_generator<int> range_async(int n) {
    TLOG("range_async start n=%d", n);
    for (int i = 0; i < n; ++i) {
        TLOG("range_async sleeping i=%d", i);
        co_await sleep(std::chrono::milliseconds(1));
        TLOG("range_async yielding i=%d", i);
        co_yield i;
        TLOG("range_async resumed after yield i=%d", i);
    }
    TLOG("range_async done");
}

TEST_F(AsyncGeneratorHelpersTests, AgForEach_VisitsAll) {
    std::vector<int> visited;
    bool done = false;
    TLOG("AgForEach_VisitsAll: spawning");

    // Use spawn(callable) — no trailing () — to avoid dangling closure
    coro_scheduler().spawn([&visited, &done]() -> task<void> {
        TLOG("AgForEach coroutine start");
        co_await ag_for_each(range_async(5), [&visited](int v) {
            TLOG("AgForEach callback v=%d", v);
            visited.push_back(v);
        });
        TLOG("AgForEach coroutine: ag_for_each awaited, setting done");
        done = true;
        TLOG("AgForEach coroutine: done=true set, returning");
    });
    run_for(100ms);
    TLOG("AgForEach_VisitsAll: after run_for done=%d visited.size=%zu",
         done ? 1 : 0, visited.size());

    EXPECT_TRUE(done);
    EXPECT_EQ(visited, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(AsyncGeneratorHelpersTests, AgCollect_GathersAll) {
    bool done = false;
    std::vector<int> result;
    TLOG("AgCollect_GathersAll: spawning");

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        TLOG("AgCollect coroutine start");
        result = co_await ag_collect(range_async(5));
        TLOG("AgCollect coroutine done result.size=%zu", result.size());
        done = true;
    });
    run_for(100ms);
    TLOG("AgCollect_GathersAll: done=%d result.size=%zu", done ? 1 : 0, result.size());

    EXPECT_TRUE(done);
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(AsyncGeneratorHelpersTests, AgMap_TransformsAll) {
    bool done = false;
    std::vector<int> result;

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_map(range_async(4), [](int v) { return v * 2; });
        done = true;
    });
    run_for(100ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(result, (std::vector<int>{0, 2, 4, 6}));
}

TEST_F(AsyncGeneratorHelpersTests, AgFilter_KeepsMatching) {
    bool done = false;
    std::vector<int> result;

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_filter(range_async(6), [](int v) { return v % 2 == 0; });
        done = true;
    });
    run_for(100ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(result, (std::vector<int>{0, 2, 4}));
}

TEST_F(AsyncGeneratorHelpersTests, AgReduce_SumsAll) {
    bool done = false;
    int  sum  = 0;

    coro_scheduler().spawn([&sum, &done]() -> task<void> {
        sum = co_await ag_reduce(range_async(5), 0, std::plus<int>{});
        done = true;
    });
    run_for(100ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(sum, 0 + 1 + 2 + 3 + 4);
}

TEST_F(AsyncGeneratorHelpersTests, AgTake_LimitsOutput) {
    bool done = false;
    std::vector<int> result;

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_collect(ag_take(range_async(10), 3));
        done = true;
    });
    run_for(100ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2}));
}

TEST_F(AsyncGeneratorHelpersTests, AgSkip_SkipsN) {
    bool done = false;
    std::vector<int> result;

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_collect(ag_skip(range_async(6), 3));
        done = true;
    });
    run_for(100ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(result, (std::vector<int>{3, 4, 5}));
}

TEST_F(AsyncGeneratorHelpersTests, AgForEach_WithAsyncCallback) {
    std::vector<int> visited;
    bool done = false;
    TLOG("AgForEach_WithAsyncCallback: spawning");

    coro_scheduler().spawn([&visited, &done]() -> task<void> {
        TLOG("AgForEach_WithAsyncCallback coroutine start");
        co_await ag_for_each(range_async(3),
            [&visited](int v) -> task<void> {
                TLOG("AgForEach_WithAsyncCallback async_cb v=%d sleeping", v);
                co_await sleep(1ms);
                TLOG("AgForEach_WithAsyncCallback async_cb v=%d pushing", v);
                visited.push_back(v * 10);
            });
        TLOG("AgForEach_WithAsyncCallback coroutine done");
        done = true;
    });
    run_for(200ms);
    TLOG("AgForEach_WithAsyncCallback: done=%d visited.size=%zu",
         done ? 1 : 0, visited.size());

    EXPECT_TRUE(done);
    EXPECT_EQ(visited, (std::vector<int>{0, 10, 20}));
}

TEST_F(AsyncGeneratorHelpersTests, EmptyGenerator) {
    bool done = false;
    std::vector<int> result;

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_collect(range_async(0));
        done = true;
    });
    run_for(50ms);

    EXPECT_TRUE(done);
    EXPECT_TRUE(result.empty());
}

// =============================================================================
// TEST SUITE: Generator Advanced APIs
// =============================================================================

class GeneratorAdvancedTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

TEST_F(GeneratorAdvancedTests, FromIteratorYieldsRange) {
    std::vector<int> data = {10, 20, 30, 40};
    auto gen = from_iterator(data.begin(), data.end());
    auto result = collect_to_vector(gen);
    EXPECT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], 10);
    EXPECT_EQ(result[3], 40);
}

TEST_F(GeneratorAdvancedTests, FromIteratorEmpty) {
    std::vector<int> data;
    auto gen = from_iterator(data.begin(), data.end());
    auto result = collect_to_vector(gen);
    EXPECT_TRUE(result.empty());
}

TEST_F(GeneratorAdvancedTests, GeneratorYieldsThenEnds) {
    auto finite_gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
    };

    auto gen = finite_gen();
    auto it = gen.begin();
    EXPECT_NE(it, gen.end());
    EXPECT_EQ(*it, 1);
    ++it;
    EXPECT_EQ(*it, 2);
    ++it;
    EXPECT_EQ(it, gen.end());
}

TEST_F(GeneratorAdvancedTests, GeneratorHasNextAndNext) {
    auto gen = range(1, 4);
    EXPECT_TRUE(gen.has_next());
    EXPECT_EQ(gen.next(), 1);
    EXPECT_TRUE(gen.has_next());
    EXPECT_EQ(gen.next(), 2);
    EXPECT_TRUE(gen.has_next());
    EXPECT_EQ(gen.next(), 3);
    EXPECT_FALSE(gen.has_next());
}

TEST_F(GeneratorAdvancedTests, GeneratorMoveSemantics) {
    auto gen1 = range(1, 4);
    auto gen2 = std::move(gen1);
    auto result = collect_to_vector(gen2);
    EXPECT_EQ(result.size(), 3u);
}

TEST_F(GeneratorAdvancedTests, AsyncGeneratorMoveSemantics) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto gen1 = range_async(3);
        auto gen2 = std::move(gen1);
        auto result = co_await ag_collect(std::move(gen2));
        EXPECT_EQ(result.size(), 3u);
        done = true;
    }());
    run_for(200ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
