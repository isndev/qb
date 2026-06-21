/**
 * @file test-actor-coroutine.cpp
 * @brief Integration tests for Actor + Coroutine functionality
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io/async/coroutine.h>
#include <atomic>
#include <chrono>

using namespace qb;
using namespace std::chrono_literals;

// Test Events
struct StartCoroEvent : public qb::Event {};
struct CoroCompletedEvent : public qb::Event {
    int result{0};
    CoroCompletedEvent() = default;
    explicit CoroCompletedEvent(int r)
        : result(r) {}
};
struct MultiCoroStartEvent : public qb::Event {};
struct CoroIncrementEvent : public qb::Event {};

/**
 * @brief Test actor for basic coroutine functionality
 */
class BasicCoroActor : public qb::Actor {
public:
    bool
    onInit() override {
        registerEvent<CoroCompletedEvent>(*this);

        // Spawn coroutine immediately
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(std::chrono::milliseconds(10));
            ctx.template push<CoroCompletedEvent>(42);
        });

        return true;
    }

    void
    on(const CoroCompletedEvent &ev) {
        EXPECT_EQ(ev.result, 42);
        kill();
    }
};

/**
 * @brief Test actor for multiple concurrent coroutines
 */
class MultiCoroActor : public qb::Actor {
    static constexpr int EXPECTED_COUNT = 5;
    int                  completed_{0};

public:
    bool
    onInit() override {
        registerEvent<CoroIncrementEvent>(*this);

        // Spawn multiple coroutines
        for (int i = 0; i < EXPECTED_COUNT; ++i) {
            spawn_detached([i](auto ctx) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(std::chrono::milliseconds(5 * (i + 1)));
                ctx.template push<CoroIncrementEvent>();
            });
        }

        return true;
    }

    void
    on(const CoroIncrementEvent &ev) {
        ++completed_;
        if (completed_ >= EXPECTED_COUNT) {
            EXPECT_EQ(completed_, EXPECTED_COUNT);
            kill();
        }
    }
};

/**
 * @brief Test actor for exception handling in coroutines
 */
class ExceptionCoroActor : public qb::Actor {
    bool caught_exception_{false};

public:
    bool
    onInit() override {
        registerEvent<CoroCompletedEvent>(*this);

        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::io::async::sleep(std::chrono::milliseconds(5));
                throw std::runtime_error("Test exception");
            } catch (const std::exception &) {
                // Exception caught - send completion
                ctx.template push<CoroCompletedEvent>(1);
            }
        });

        return true;
    }

    void
    on(const CoroCompletedEvent &ev) {
        EXPECT_EQ(ev.result, 1);
        caught_exception_ = true;
        kill();
    }

    ~ExceptionCoroActor() {
        EXPECT_TRUE(caught_exception_);
    }
};

/**
 * @brief Test actor for nested coroutine spawning
 */
class NestedCoroActor : public qb::Actor {
    int depth_{0};

public:
    bool
    onInit() override {
        registerEvent<CoroIncrementEvent>(*this);

        spawn_detached([this](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(std::chrono::milliseconds(5));

            // Spawn nested coroutine
            spawn_detached([](auto ctx2) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(std::chrono::milliseconds(5));
                ctx2.template push<CoroIncrementEvent>();
                ctx2.template push<CoroIncrementEvent>(); // Send twice
            });
        });

        return true;
    }

    void
    on(const CoroIncrementEvent &ev) {
        ++depth_;
        if (depth_ >= 2) {
            EXPECT_EQ(depth_, 2);
            kill();
        }
    }
};

/**
 * @brief Test actor with coroutine that uses CoroContext safely
 */
class SafetyTestActor : public qb::Actor {
public:
    bool
    onInit() override {
        registerEvent<CoroCompletedEvent>(*this);

        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            // Verify CoroContext provides safe interface
            EXPECT_TRUE(ctx.id().is_valid());
            EXPECT_GT(ctx.time(), 0);

            co_await qb::io::async::sleep(std::chrono::milliseconds(5));

            // Can only push events (safe)
            ctx.template push<CoroCompletedEvent>(99);
        });

        return true;
    }

    void
    on(const CoroCompletedEvent &ev) {
        EXPECT_EQ(ev.result, 99);
        kill();
    }
};

/**
 * @brief Test actor that spawns coroutine and kills immediately
 */
class QuickKillActor : public qb::Actor {
public:
    bool
    onInit() override {
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(std::chrono::seconds(10));
            // This should never execute - actor killed before completion
        });

        // Kill immediately after spawning
        kill();
        return true;
    }
};

// Tests
TEST(ActorCoroutine, BasicCoroutineExecution) {
    qb::Main main;
    main.addActor<BasicCoroActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorCoroutine, MultipleCoroutinesPerActor) {
    qb::Main main;
    main.addActor<MultiCoroActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorCoroutine, ExceptionHandlingInCoroutine) {
    qb::Main main;
    main.addActor<ExceptionCoroActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorCoroutine, NestedCoroutineSpawn) {
    qb::Main main;
    main.addActor<NestedCoroActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorCoroutine, CoroContextSafety) {
    qb::Main main;
    main.addActor<SafetyTestActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorCoroutine, ActorCleanupWithActiveCoroutines) {
    qb::Main main;
    main.addActor<QuickKillActor>(0);
    main.start(false);
    main.join();
    // Test passes if no crash/leak during cleanup
    EXPECT_FALSE(main.hasError());
}
