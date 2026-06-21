/**
 * @file test-actor-coroutine-resilience.cpp
 * @brief Tests for the resilience layer built on `ask`:
 *        the `CircuitBreaker` state machine, `ScopedCoroContext::ask_retry`
 *        (retry + exponential backoff), and `ScopedCoroContext::ask_guarded`
 *        (circuit-breaker-protected ask).
 *
 * Coverage:
 *   - CircuitBreaker: closed/open/half-open transitions, threshold, cooldown, reset
 *     (pure unit tests, no engine);
 *   - ask_retry: succeeds first try, succeeds after transient timeouts, exhausts -> timeout_error,
 *     and a kill aborts the retry loop with cancelled_error;
 *   - ask_guarded: closed passes through, trips open after the failure threshold then fails fast
 *     with circuit_open_error, and a kill is NOT counted as a breaker failure.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <atomic>
#include <chrono>
#include <memory>

using namespace qb;
using namespace std::chrono_literals;

// ===========================================================================
// CircuitBreaker — pure unit tests (drive time explicitly, no engine).
// ===========================================================================
namespace {
constexpr uint64_t MS = 1'000'000ull; // one millisecond in nanoseconds
using State           = qb::CircuitBreaker::State;
} // namespace

TEST(CircuitBreakerUnit, StartsClosedAndAllows) {
    qb::CircuitBreaker cb(3, 100ms);
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_TRUE(cb.allow(0));
    EXPECT_EQ(cb.failure_count(), 0u);
}

TEST(CircuitBreakerUnit, StaysClosedBelowThreshold) {
    qb::CircuitBreaker cb(3, 100ms);
    cb.on_failure(0);
    cb.on_failure(0); // 2 < 3
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_TRUE(cb.allow(0));
}

TEST(CircuitBreakerUnit, OpensAtThreshold) {
    qb::CircuitBreaker cb(3, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);
    cb.on_failure(0); // 3 == threshold -> open
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(0)); // within cooldown -> fail fast
}

TEST(CircuitBreakerUnit, SuccessResetsFailures) {
    qb::CircuitBreaker cb(3, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);
    cb.on_success();
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_EQ(cb.state(), State::closed);
}

TEST(CircuitBreakerUnit, FailsFastDuringCooldown) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0); // open at t=0
    EXPECT_FALSE(cb.allow(50 * MS)); // 50ms < 100ms cooldown
    EXPECT_EQ(cb.state(), State::open);
}

TEST(CircuitBreakerUnit, HalfOpensAfterCooldown) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);                 // open at t=0
    EXPECT_TRUE(cb.allow(100 * MS));  // cooldown elapsed -> admit a trial
    EXPECT_EQ(cb.state(), State::half_open);
}

TEST(CircuitBreakerUnit, HalfOpenSuccessCloses) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);
    (void) cb.allow(100 * MS); // -> half_open
    cb.on_success();           // trial succeeded
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_TRUE(cb.allow(110 * MS));
}

TEST(CircuitBreakerUnit, HalfOpenFailureReopens) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);             // open at t=0
    (void) cb.allow(100 * MS);    // -> half_open
    cb.on_failure(100 * MS);      // trial failed -> reopen at t=100ms
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(150 * MS)); // 50ms into the new cooldown
    EXPECT_TRUE(cb.allow(200 * MS));  // 100ms after reopen -> trial again
}

// ===========================================================================
// Shared event + responders for the integration tests.
// ===========================================================================
struct Ping : public qb::Request<int> {
    int seq{0};
    Ping() = default;
    explicit Ping(int s)
        : seq(s) {}
};

// Always answers (response = seq * 2).
class Echoer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &p) {
        qb::answer(*this, p, [](Ping const &r) { return r.seq * 2; });
    }
};

// Never answers -> every ask times out.
class Silent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &) {}
};

// Ignores the first (reply_on - 1) requests, then answers — models transient failures.
class Flaky : public qb::Actor {
    int _count{0};
    int _reply_on;

public:
    explicit Flaky(int reply_on)
        : _reply_on(reply_on) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &p) {
        if (++_count >= _reply_on)
            qb::answer(*this, p, [](Ping const &r) { return r.seq * 2; });
        // else: drop it -> the asker times out and retries
    }
};

// ===========================================================================
// ask_retry
// ===========================================================================
namespace {
std::atomic<int>  g_retry_result{-1};
std::atomic<bool> g_retry_timed_out{false};
std::atomic<bool> g_retry_cancelled{false};

qb::retry_policy
fast_policy(int attempts) {
    qb::retry_policy p;
    p.max_attempts = attempts;
    p.backoff      = 10ms;
    p.multiplier   = 2.0;
    p.max_backoff  = 40ms;
    return p;
}
} // namespace

class RetryClient : public qb::Actor {
    qb::ActorId _target;
    int         _attempts;

public:
    RetryClient(qb::ActorId t, int attempts)
        : _target(t)
        , _attempts(attempts) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto t        = _target;
        auto attempts = _attempts;
        spawn([t, attempts](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                auto r         = co_await qb::ask_retry(ctx, t, Ping{7}, 40ms, fast_policy(attempts));
                g_retry_result = r.response;
            } catch (const qb::io::async::timeout_error &) {
                g_retry_timed_out = true;
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskRetry, SucceedsFirstTry) {
    g_retry_result = -1;
    g_retry_timed_out = false;
    qb::Main main;
    auto     echo = main.addActor<Echoer>(0);
    main.addActor<RetryClient>(0, echo, 3);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_retry_result.load(), 14); // 7 * 2
    EXPECT_FALSE(g_retry_timed_out.load());
}

TEST(ActorAskRetry, SucceedsAfterTransientTimeouts) {
    g_retry_result = -1;
    g_retry_timed_out = false;
    qb::Main main;
    auto     flaky = main.addActor<Flaky>(0, 3); // answers only the 3rd attempt
    main.addActor<RetryClient>(0, flaky, 5);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_retry_result.load(), 14); // eventually answered
    EXPECT_FALSE(g_retry_timed_out.load());
}

TEST(ActorAskRetry, ExhaustsAndThrowsTimeout) {
    g_retry_result = -1;
    g_retry_timed_out = false;
    qb::Main main;
    auto     silent = main.addActor<Silent>(0);
    main.addActor<RetryClient>(0, silent, 3);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_retry_timed_out.load());
    EXPECT_EQ(g_retry_result.load(), -1);
}

class RetryCancelClient : public qb::Actor {
    qb::ActorId _target;

public:
    explicit RetryCancelClient(qb::ActorId t)
        : _target(t) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto t = _target;
        spawn([t](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask_retry(ctx, t, Ping{1}, 100ms, fast_policy(10));
            } catch (const qb::io::async::cancelled_error &) {
                g_retry_cancelled = true;
            }
            qb::Main::stop();
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            25ms);
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskRetry, CancelledOnKillAbortsRetries) {
    g_retry_cancelled = false;
    qb::Main main;
    auto     silent = main.addActor<Silent>(0);
    main.addActor<RetryCancelClient>(0, silent);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_retry_cancelled.load());
}

// ===========================================================================
// ask_guarded (CircuitBreaker-protected ask)
// ===========================================================================
namespace {
std::atomic<int> g_guard_success{0};
std::atomic<int> g_guard_timeout{0};
std::atomic<int> g_guard_open{0};
std::atomic<bool> g_guard_cancelled{false};
} // namespace

class GuardClient : public qb::Actor {
    std::shared_ptr<qb::CircuitBreaker> _breaker;
    qb::ActorId                         _target;
    int                                 _calls;

public:
    GuardClient(std::shared_ptr<qb::CircuitBreaker> b, qb::ActorId t, int calls)
        : _breaker(std::move(b))
        , _target(t)
        , _calls(calls) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto b = _breaker;
        auto t = _target;
        auto n = _calls;
        spawn([b, t, n](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int i = 0; i < n; ++i) {
                try {
                    auto r = co_await qb::ask_guarded(ctx, b, t, Ping{i}, 40ms);
                    (void) r;
                    g_guard_success.fetch_add(1);
                } catch (const qb::circuit_open_error &) {
                    g_guard_open.fetch_add(1);
                } catch (const qb::io::async::timeout_error &) {
                    g_guard_timeout.fetch_add(1);
                }
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskGuarded, ClosedPassesThrough) {
    g_guard_success = g_guard_timeout = g_guard_open = 0;
    auto     breaker = std::make_shared<qb::CircuitBreaker>(2u, 10s);
    qb::Main main;
    auto     echo = main.addActor<Echoer>(0);
    main.addActor<GuardClient>(0, breaker, echo, 3);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_guard_success.load(), 3);
    EXPECT_EQ(g_guard_open.load(), 0);
    EXPECT_EQ(breaker->state(), State::closed);
}

TEST(ActorAskGuarded, TripsOpenThenFailsFast) {
    g_guard_success = g_guard_timeout = g_guard_open = 0;
    auto     breaker = std::make_shared<qb::CircuitBreaker>(2u, 10s); // opens after 2 failures
    qb::Main main;
    auto     silent = main.addActor<Silent>(0);
    main.addActor<GuardClient>(0, breaker, silent, 4); // 2 timeouts trip it, next 2 fail fast
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_guard_success.load(), 0);
    EXPECT_EQ(g_guard_timeout.load(), 2); // first two attempts actually time out
    EXPECT_EQ(g_guard_open.load(), 2);    // remaining two fail fast (no request sent)
    EXPECT_EQ(breaker->state(), State::open);
}

class GuardCancelClient : public qb::Actor {
    std::shared_ptr<qb::CircuitBreaker> _breaker;
    qb::ActorId                         _target;

public:
    GuardCancelClient(std::shared_ptr<qb::CircuitBreaker> b, qb::ActorId t)
        : _breaker(std::move(b))
        , _target(t) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto b = _breaker;
        auto t = _target;
        spawn([b, t](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask_guarded(ctx, b, t, Ping{0}, 500ms);
            } catch (const qb::io::async::cancelled_error &) {
                g_guard_cancelled = true;
            }
            qb::Main::stop();
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            25ms);
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskGuarded, CancelledIsNotCountedAsFailure) {
    g_guard_cancelled = false;
    auto     breaker = std::make_shared<qb::CircuitBreaker>(5u, 10s);
    qb::Main main;
    auto     silent = main.addActor<Silent>(0);
    main.addActor<GuardCancelClient>(0, breaker, silent);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_guard_cancelled.load());
    EXPECT_EQ(breaker->failure_count(), 0u); // kill did not count as a failure
    EXPECT_EQ(breaker->state(), State::closed);
}
