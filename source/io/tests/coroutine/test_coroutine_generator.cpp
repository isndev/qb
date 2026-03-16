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

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <vector>

using namespace qb::io::async;

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
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
