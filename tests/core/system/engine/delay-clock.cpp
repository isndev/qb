/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/delay-clock.cpp
 * @brief The delays qb-core measures are measured on the monotonic clock, not on the time of day (Huly QB-1199).
 *
 * Until 3.4 six of them read `Actor::time()` -- the core's sample of the wall clock, `qb::unix_nanos(qb::wall_now())`:
 * the activation deadline of an `onInit()` that suspends, a `Supervisor`'s sliding restart window, a `qb::deadline`
 * (`deadline_in`, `remaining`, `ask_by`), and the clock `ask_guarded` and `rate_limiter::acquire(ctx)` feed a
 * `CircuitBreaker` and a `rate_limiter`. A step of the time of day (an NTP step, `settimeofday`, a manual change) moved
 * each by the step. Forward: an activation failed at once, a window forgot the restarts it held, a chain's budget was
 * spent, a limiter was refilled and a breaker's cooldown skipped. Back: an activation outlived its deadline, a window
 * held restarts past it, a budget outlived itself, a limiter stopped refilling until the time of day caught up, and a
 * breaker's cooldown ended at once (its unsigned `now - opened_at` wrapped; since QB-1237 an older reading saturates at
 * zero, and the step would hold the breaker open as long as it is instead).
 *
 * Here the time of day is INJECTED, never changed on the system: this binary defines the clock every `qb::wall_now()`
 * it links reads -- qb-core's included -- as the system's plus a step the test sets. Each case proves first that the
 * step reached the core (`Actor::time()` moved by it), then that the delay did not: no earlier than its span on
 * `qb::mono_now()` after a step forward, no later than a bounded wait after a step back.
 *
 * The clock is replaced at link time: the executable's definition of `std::chrono::system_clock::now()` is the one
 * qb-core's calls bind to when qb-core is a static library, or a shared one on ELF (an executable's definition
 * interposes); not a shared qb-core on Mach-O, which binds libc++'s own, nor MSVC's STL, which reads the clock in its
 * header. The CMake registration is gated on that. libc++ on macOS computes a `std::condition_variable` timed wait's end
 * from `system_clock::now()`, which libpthread turns back into a relative wait with its own reading of the time of day,
 * so a clock injected here moved such a wait by the injected offset until qb timed it on the monotonic clock there (Huly
 * QB-1227): the parked-core cases at the end inject it on purpose -- what a time mock does to a process.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <thread>

#include <sys/resource.h>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/icallback.h>
#include <qb/main.h>
#include <qb/testkit.h>

#include "../../shared/InitFixtures.h"
#include "../../shared/ProbeResponders.h"

namespace delay_clock_test {

/// The step of the time of day a test injects, in nanoseconds; the system's clock is never changed.
inline std::atomic<std::int64_t> time_of_day_step{0};

} // namespace delay_clock_test

#if defined(_WIN32)
#error "delay-clock replaces std::chrono::system_clock::now() at link time: MSVC's STL defines it in the header"
#endif

/// The time of day of this executable -- qb-core's `qb::wall_now()` included: the system's (`CLOCK_REALTIME`, what the
/// standard library reads) plus the injected step.
std::chrono::system_clock::time_point
std::chrono::system_clock::now() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::chrono::nanoseconds const since_epoch{
        static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec + delay_clock_test::time_of_day_step.load(std::memory_order_relaxed)
    };
    return time_point{std::chrono::duration_cast<duration>(since_epoch)};
}

using namespace std::chrono_literals;

namespace delay_clock_test {

constexpr std::int64_t kSeconds = 1'000'000'000;

[[nodiscard]] inline std::int64_t
ns(qb::duration const d) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

[[nodiscard]] inline std::int64_t
mono_ns() noexcept {
    return ns(qb::mono_now().time_since_epoch());
}

/// How far `Actor::time()` moved across the step, less what `qb::mono_now()` moved: the step, as the core saw it.
[[nodiscard]] inline std::int64_t
landed(std::uint64_t wall_before, std::uint64_t wall_after, qb::mono_time mono_before, qb::mono_time mono_after) noexcept {
    return static_cast<std::int64_t>(wall_after - wall_before) - ns(mono_after - mono_before);
}

/// Wait up to `budget` on the monotonic clock for `done`, then end the run whatever it did, and lift the step after it.
inline void
end_run(qb::Main &main, std::atomic<bool> const &done, qb::duration const budget) {
    auto const until = qb::mono_now() + budget;
    while (!done && qb::mono_now() < until)
        std::this_thread::sleep_for(5ms);
    if (!done)
        qb::Main::stop(); // the run never concluded: end it, and let the case's assertions name what is missing
    main.join();
    time_of_day_step = 0;
}

// ===========================================================================
// The activation deadline of a suspended onInit()
// ===========================================================================

struct activation_outcome {
    std::atomic<bool>         ended{false}; ///< the deadline cancelled the init
    std::atomic<std::int64_t> began{0};     ///< `qb::mono_now()` when onInit() started, ns
    std::atomic<std::int64_t> at{0};        ///< `qb::mono_now()` when the cancellation reached it, ns
    std::atomic<std::int64_t> landed{0};    ///< the step as `Actor::time()` saw it
};

/// An init that suspends, steps the time of day once the core has armed its activation deadline (at that first
/// suspension), then waits for the deadline to cancel it.
class SteppedInit : public qb::Actor {
    std::int64_t                        _step;
    std::shared_ptr<activation_outcome> _out;

public:
    SteppedInit(std::int64_t step, std::shared_ptr<activation_outcome> out)
        : _step(step)
        , _out(std::move(out)) {}

    qb::io::async::task<bool>
    onInit() override {
        auto const out  = _out;
        auto const step = _step;
        auto       ctx  = context();
        out->began      = mono_ns();
        auto const wall = time();
        auto const mono = qb::mono_now();
        co_await ctx.sleep(1ms); // the first suspension: the core arms the activation deadline here
        time_of_day_step = step;
        try {
            co_await ctx.until_cancelled(); // only the activation deadline ends this init
        } catch (...) {
            // A later pass than the step's: its `time()` reads the stepped time of day.
            out->landed = landed(wall, ctx.time(), mono, qb::mono_now());
            out->at     = mono_ns();
            out->ended  = true;
            throw;
        }
        co_return true;
    }
};

/// Run one `SteppedInit` under an activation deadline of `deadline`. `start()` returns once the activation has ended
/// -- the engine waits for the inits it was given -- and `budget` bounds the run after it, which ends on its own.
[[nodiscard]] inline std::shared_ptr<activation_outcome>
activate_across_a_step(std::int64_t step, qb::duration deadline, qb::duration budget) {
    auto const                     out = std::make_shared<activation_outcome>();
    qb::test::ScopedDeadline const knob(static_cast<std::uint64_t>(ns(deadline)));
    qb::Main                       main;
    main.addActor<SteppedInit>(0, step, out);
    main.start();
    end_run(main, out->ended, budget);
    return out;
}

// ===========================================================================
// A supervisor's sliding restart window
// ===========================================================================

struct Crash : qb::Event {};
struct StepNow : qb::Event {};

class Child : public qb::SupervisedActor {
public:
    Child(qb::ActorId supervisor, std::size_t slot, std::uint64_t generation)
        : qb::SupervisedActor(supervisor, slot, generation) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Crash>(*this);
        co_return true;
    }

    void
    on(Crash const &) {
        stop();
    }
};

struct window_outcome {
    std::atomic<bool> done{false};
    std::size_t       before    = 0;     ///< restarts in the window just before the step
    std::size_t       after     = 0;     ///< restarts in the window once the step had landed
    bool              escalated = false; ///< the third failure escalated
    bool              restarted = false; ///< the third failure was restarted
    std::int64_t      landed    = 0;     ///< the step as `Actor::time()` saw it
};

/// Two restarts recorded, then a step of the time of day, then -- `wait` later on `qb::mono_now()` -- a third failure
/// against a cap of two restarts within `window`.
class SteppedSupervisor
    : public qb::Supervisor
    , public qb::ICallback {
    std::int64_t                    _step;
    qb::duration                    _wait;
    std::shared_ptr<window_outcome> _out;
    int                             _spawns = 0;
    std::uint64_t                   _wall   = 0;
    qb::mono_time                   _mono{};

public:
    SteppedSupervisor(qb::duration window, std::int64_t step, qb::duration wait, std::shared_ptr<window_outcome> out)
        : qb::Supervisor(qb::restart_strategy::one_for_one, 1, /*max_restarts*/ 2, window)
        , _step(step)
        , _wait(wait)
        , _out(std::move(out)) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StepNow>(*this);
        co_return co_await qb::Supervisor::onInit();
    }

    void
    on(StepNow const &) {
        _out->before     = restarts_in_window();
        _wall            = time();
        _mono            = qb::mono_now();
        time_of_day_step = _step;
        registerCallback(*this);
    }

    void
    on(qb::LoopEvent const &) override {
        if (qb::mono_now() - _mono < _wait)
            return; // a later pass than the step's, `_wait` after it
        unregisterCallback();
        _out->landed = landed(_wall, time(), _mono, qb::mono_now());
        _out->after  = restarts_in_window();
        push<Crash>(child(0));
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t generation) override {
        ++_spawns;
        auto const id = addRefActor<Child>(this->id(), slot, generation).id();
        if (_spawns <= 2)
            push<Crash>(id); // the first two incarnations go down at once: two restarts recorded
        else if (_spawns == 3)
            push<StepNow>(this->id());
        else
            finish(false); // the third failure was restarted
        return id;
    }

    void
    on_escalate() override {
        finish(true);
    }

private:
    void
    finish(bool escalated) {
        _out->escalated = escalated;
        _out->restarted = !escalated;
        _out->done      = true;
        qb::Main::stop();
    }
};

[[nodiscard]] inline std::shared_ptr<window_outcome>
supervise_across_a_step(qb::duration window, std::int64_t step, qb::duration wait) {
    auto const out = std::make_shared<window_outcome>();
    qb::Main   main;
    main.addActor<SteppedSupervisor>(0, window, step, wait, out);
    main.start();
    end_run(main, out->done, 30s);
    EXPECT_FALSE(main.hasError());
    return out;
}

// ===========================================================================
// The public delays: a qb::deadline chain, a rate limiter, a circuit breaker
// ===========================================================================

using qb::test::Probe;

enum class public_case { chain_forward, chain_back, limiter_forward, limiter_back, breaker_forward, breaker_back };

struct public_outcome {
    std::atomic<bool> ran{false};
    std::int64_t      landed    = 0;     ///< the step as `ctx.time()` saw it
    std::int64_t      left      = -1;    ///< `qb::remaining` after the step, ns, of a deadline from `deadline_in` ...
    std::int64_t      left_at   = -1;    ///< ... and of the same one from `deadline_at`
    bool              answered  = false; ///< the `ask_by` / `ask_guarded` after the step got its reply
    bool              timed_out = false; ///< ... threw `timeout_error`
    bool              refused   = false; ///< ... threw `circuit_open_error`
    bool              acquired  = false; ///< the limiter's acquire after the step returned within its budget
    bool              admitted  = false; ///< the breaker's last `ask_guarded`, two cooldowns after the step, got its reply
    bool              kept_shut = false; ///< ... threw `circuit_open_error`
};

/// One scenario, in one coroutine of its own actor: build the delay, step the time of day, wait on the monotonic clock,
/// then judge the delay.
class PublicDelays : public qb::Actor {
    public_case                     _case;
    std::int64_t                    _step;
    qb::ActorId                     _fast;
    qb::ActorId                     _silent;
    std::shared_ptr<public_outcome> _out;

public:
    PublicDelays(public_case c, std::int64_t step, qb::ActorId fast, qb::ActorId silent, std::shared_ptr<public_outcome> out)
        : _case(c)
        , _step(step)
        , _fast(fast)
        , _silent(silent)
        , _out(std::move(out)) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this); // the replies to its asks come back through its own handler
        spawn([which = _case, step = _step, fast = _fast, silent = _silent, out = _out](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto const wall = c.time();
            auto const mono = qb::mono_now();
            switch (which) {
                case public_case::chain_forward: {
                    auto const dl    = qb::deadline_in(c, 60s);
                    auto const at    = qb::deadline_at(qb::mono_now() + 60s);
                    time_of_day_step = step;
                    co_await c.sleep(1ms);
                    out->landed  = landed(wall, c.time(), mono, qb::mono_now());
                    out->left    = ns(qb::remaining(dl, c));
                    out->left_at = ns(qb::remaining(at, c));
                    try {
                        auto const r  = co_await qb::ask_by(c, fast, Probe{5}, dl);
                        out->answered = r.response == 6;
                    } catch (qb::io::async::timeout_error const &) {
                        out->timed_out = true;
                    }
                    break;
                }
                case public_case::chain_back: {
                    auto const dl    = qb::deadline_in(c, 200ms);
                    auto const at    = qb::deadline_at(qb::mono_now() + 200ms);
                    time_of_day_step = step;
                    co_await c.sleep(300ms);
                    out->landed  = landed(wall, c.time(), mono, qb::mono_now());
                    out->left    = ns(qb::remaining(dl, c));
                    out->left_at = ns(qb::remaining(at, c));
                    break;
                }
                case public_case::limiter_forward:
                case public_case::limiter_back: {
                    // One token, one more every `per_token`: the first acquire takes it; after the step the second must
                    // wait for its token -- `per_token` on the monotonic clock, no less, no more.
                    bool const fwd       = which == public_case::limiter_forward;
                    auto const per_token = fwd ? qb::duration{10s} : qb::duration{50ms};
                    auto const limiter   = std::make_shared<qb::rate_limiter>(1.0, per_token);
                    co_await limiter->acquire(c);
                    time_of_day_step = step;
                    co_await c.sleep(1ms);
                    out->landed = landed(wall, c.time(), mono, qb::mono_now());
                    try {
                        co_await qb::io::async::coro_with_timeout(limiter->acquire(c), fwd ? qb::duration{300ms} : qb::duration{2s});
                        out->acquired = true;
                    } catch (qb::io::async::timeout_error const &) {
                        out->timed_out = true;
                    }
                    break;
                }
                case public_case::breaker_forward:
                case public_case::breaker_back: {
                    // One failure opens it, for 60 s forward and 2 s back: after the step, a call must still be refused;
                    // back, a call two cooldowns later on the monotonic clock must be admitted -- the step neither cuts the
                    // cooldown short nor stretches it. Back, the cooldown is long enough for the call 1 ms in to land inside
                    // it on a loaded host, and short enough for the last one to land well inside the 30 s step.
                    bool const fwd     = which == public_case::breaker_forward;
                    auto const breaker = std::make_shared<qb::CircuitBreaker>(1u, fwd ? qb::duration{60s} : qb::duration{2s});
                    try {
                        (void) co_await qb::ask_guarded(c, breaker, silent, Probe{1}, 20ms);
                    } catch (qb::io::async::timeout_error const &) {
                    }
                    time_of_day_step = step;
                    co_await c.sleep(1ms);
                    out->landed = landed(wall, c.time(), mono, qb::mono_now());
                    try {
                        (void) co_await qb::ask_guarded(c, breaker, fast, Probe{1}, 1s);
                        out->answered = true;
                    } catch (qb::circuit_open_error const &) {
                        out->refused = true;
                    } catch (qb::io::async::timeout_error const &) {
                        out->timed_out = true;
                    }
                    if (!fwd) {
                        co_await c.sleep(4s); // twice the cooldown, on the monotonic clock
                        try {
                            (void) co_await qb::ask_guarded(c, breaker, fast, Probe{1}, 1s); // the half-open trial
                            out->admitted = true;
                        } catch (qb::circuit_open_error const &) {
                            out->kept_shut = true;
                        } catch (qb::io::async::timeout_error const &) {
                        }
                    }
                    break;
                }
            }
            out->ran = true;
            qb::Main::stop();
        });
        co_return true;
    }

    void
    on(Probe &e) {
        resolve_ask(e);
    }
};

[[nodiscard]] inline std::shared_ptr<public_outcome>
public_delay_across_a_step(public_case which, std::int64_t step) {
    auto const out = std::make_shared<public_outcome>();
    qb::Main   main;
    auto const fast   = main.addActor<qb::test::FastResponder>(0);
    auto const silent = main.addActor<qb::test::SilentResponder>(0);
    main.addActor<PublicDelays>(0, which, step, fast, silent, out);
    main.start();
    end_run(main, out->ran, 30s);
    EXPECT_FALSE(main.hasError());
    return out;
}

} // namespace delay_clock_test

TEST(DelayClock, ATimeOfDayStepForwardDoesNotCutAnActivationShort) {
    // 30 s forward under a 1 s activation deadline: measured on the time of day, the init failed at once.
    using namespace delay_clock_test;
    auto const r = activate_across_a_step(30 * kSeconds, 1s, 20s);
    ASSERT_TRUE(r->ended) << "the activation deadline never cancelled the init";
    ASSERT_GT(r->landed.load(), 15 * kSeconds) << "the injected step did not reach the core's Actor::time()";
    EXPECT_GE(r->at - r->began, 1 * kSeconds) << "the activation deadline fired " << (kSeconds - (r->at - r->began))
                                              << " ns early: the step forward was counted";
}

TEST(DelayClock, ATimeOfDayStepBackDoesNotExtendAnActivation) {
    // 30 s back under a 1 s activation deadline: measured on the time of day, the init outlived it by the step.
    using namespace delay_clock_test;
    auto const r = activate_across_a_step(-30 * kSeconds, 1s, 20s);
    ASSERT_TRUE(r->ended) << "the activation deadline never cancelled the init";
    ASSERT_LT(r->landed.load(), -15 * kSeconds) << "the injected step did not reach the core's Actor::time()";
    EXPECT_GE(r->at - r->began, 1 * kSeconds) << "the activation deadline fired early";
    EXPECT_LT(r->at - r->began, 20 * kSeconds) << "the activation deadline fired " << (r->at - r->began) / 1'000'000
                                               << " ms after the init began: it waited for the time of day to catch up with the step";
}

TEST(DelayClock, ATimeOfDayStepForwardDoesNotEmptyARestartWindow) {
    // Two restarts in a 60 s window, then 120 s forward: measured on the time of day, the window forgot both and the
    // third failure, the cap's, was restarted instead of escalated.
    using namespace delay_clock_test;
    auto const r = supervise_across_a_step(60s, 120 * kSeconds, 1ms);
    ASSERT_TRUE(r->done) << "the third failure was neither restarted nor escalated";
    ASSERT_GT(r->landed, 60 * kSeconds) << "the injected step did not reach the core's Actor::time()";
    EXPECT_EQ(r->before, 2u);
    EXPECT_EQ(r->after, 2u) << "the step forward pushed the restarts out of the window";
    EXPECT_TRUE(r->escalated) << "the third failure within the window was restarted, past the cap";
}

TEST(DelayClock, ATimeOfDayStepBackDoesNotHoldRestartsPastTheirWindow) {
    // Two restarts in a 1 s window, 30 s back, then 1.5 s: both have left the window. Measured on the time of day, they
    // were still in it, and the third failure escalated.
    using namespace delay_clock_test;
    auto const r = supervise_across_a_step(1s, -30 * kSeconds, 1500ms);
    ASSERT_TRUE(r->done) << "the third failure was neither restarted nor escalated";
    ASSERT_LT(r->landed, -15 * kSeconds) << "the injected step did not reach the core's Actor::time()";
    EXPECT_EQ(r->before, 2u);
    EXPECT_EQ(r->after, 0u) << "the step back held the restarts in the window";
    EXPECT_TRUE(r->restarted) << "the third failure, its window empty, escalated";
}

TEST(DelayClock, ATimeOfDayStepForwardDoesNotSpendADeadlinesBudget) {
    // A 60 s budget, then 120 s forward: measured on the time of day, nothing was left and ask_by failed unsent.
    using namespace delay_clock_test;
    auto const r = public_delay_across_a_step(public_case::chain_forward, 120 * kSeconds);
    ASSERT_TRUE(r->ran);
    ASSERT_GT(r->landed, 60 * kSeconds) << "the injected step did not reach the core's ctx.time()";
    EXPECT_GT(r->left, 59 * kSeconds) << "a 60 s budget read " << r->left << " ns left 1 ms in: the step forward spent it";
    EXPECT_GT(r->left_at, 59 * kSeconds) << "a deadline_at 60 s ahead read " << r->left_at << " ns left 1 ms in";
    EXPECT_TRUE(r->answered) << "ask_by on the budget failed (timed out: " << r->timed_out << ")";
}

TEST(DelayClock, ATimeOfDayStepBackDoesNotExtendADeadline) {
    // A 200 ms budget, 30 s back, then 300 ms: measured on the time of day, 29.9 s were left.
    using namespace delay_clock_test;
    auto const r = public_delay_across_a_step(public_case::chain_back, -30 * kSeconds);
    ASSERT_TRUE(r->ran);
    ASSERT_LT(r->landed, -15 * kSeconds) << "the injected step did not reach the core's ctx.time()";
    EXPECT_EQ(r->left, 0) << "a 200 ms budget read " << r->left / 1'000'000 << " ms left 300 ms in: the step back extended it";
    EXPECT_EQ(r->left_at, 0) << "a deadline_at 200 ms ahead read " << r->left_at / 1'000'000 << " ms left 300 ms in";
}

TEST(DelayClock, ATimeOfDayStepForwardDoesNotRefillARateLimiter) {
    // One token every 10 s, 120 s forward: measured on the time of day, the next token was granted at once.
    using namespace delay_clock_test;
    auto const r = public_delay_across_a_step(public_case::limiter_forward, 120 * kSeconds);
    ASSERT_TRUE(r->ran);
    ASSERT_GT(r->landed, 60 * kSeconds) << "the injected step did not reach the core's ctx.time()";
    EXPECT_FALSE(r->acquired) << "a token due in 10 s was granted 1 ms in: the step forward refilled the bucket";
}

TEST(DelayClock, ATimeOfDayStepBackDoesNotStallARateLimiter) {
    // One token every 50 ms, 30 s back: measured on the time of day, no token came until it caught up 30 s later.
    using namespace delay_clock_test;
    auto const r = public_delay_across_a_step(public_case::limiter_back, -30 * kSeconds);
    ASSERT_TRUE(r->ran);
    ASSERT_LT(r->landed, -15 * kSeconds) << "the injected step did not reach the core's ctx.time()";
    EXPECT_TRUE(r->acquired) << "a token due in 50 ms was not granted in 2 s: the bucket waits for the time of day to catch up";
}

TEST(DelayClock, ATimeOfDayStepForwardDoesNotCutABreakersCooldownShort) {
    // A breaker open for 60 s, 120 s forward: measured on the time of day, the cooldown had passed.
    using namespace delay_clock_test;
    auto const r = public_delay_across_a_step(public_case::breaker_forward, 120 * kSeconds);
    ASSERT_TRUE(r->ran);
    ASSERT_GT(r->landed, 60 * kSeconds) << "the injected step did not reach the core's ctx.time()";
    EXPECT_TRUE(r->refused) << "a breaker open for 60 s admitted a call 1 ms in (answered: " << r->answered << ")";
}

TEST(DelayClock, ATimeOfDayStepBackNeitherCutsNorStretchesABreakersCooldown) {
    // A breaker open for 2 s, 30 s back. Measured on the time of day, `now - opened_at` wrapped and the cooldown ended
    // at once; with an older reading saturated at zero (QB-1237), the step would hold it open 30 s longer instead.
    using namespace delay_clock_test;
    auto const r = public_delay_across_a_step(public_case::breaker_back, -30 * kSeconds);
    ASSERT_TRUE(r->ran);
    ASSERT_LT(r->landed, -15 * kSeconds) << "the injected step did not reach the core's ctx.time()";
    EXPECT_TRUE(r->refused) << "a breaker open for 2 s admitted a call 1 ms in (answered: " << r->answered << ")";
    EXPECT_TRUE(r->admitted) << (r->kept_shut ? "a breaker open for 2 s still refused a call 4 s later: the step back stretched its cooldown"
                                              : "a breaker open for 2 s admitted a call 4 s later that never got its reply");
}

TEST(DelayClock, ACoreParksWhileAnActorIsActivating) {
    // A core with latency parks while an actor is Activating, its park capped by the activation deadline: the cap and the
    // deadline must be read on one clock. Read on two, the time left reads 0 and the core spins through the activation, or
    // reads decades and the core sleeps a whole park, a second, past the deadline; without the cap it sleeps that too.
    using namespace delay_clock_test;
    auto const                     out = std::make_shared<activation_outcome>();
    qb::test::ScopedDeadline const knob(static_cast<std::uint64_t>(ns(500ms)));
    qb::Main                       main;
    main.core(0).setLatency(3s);
    main.addActor<SteppedInit>(0, std::int64_t{0}, out); // no step: the time of day is the system's
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    main.start();
    end_run(main, out->ended, 20s);
    getrusage(RUSAGE_SELF, &after);
    auto const cpu = [](rusage const &u) {
        return std::int64_t{u.ru_utime.tv_sec + u.ru_stime.tv_sec} * kSeconds + std::int64_t{u.ru_utime.tv_usec + u.ru_stime.tv_usec} * 1000;
    };
    ASSERT_TRUE(out->ended) << "the activation deadline never cancelled the init";
    auto const span = out->at - out->began;
    EXPECT_GE(span, ns(500ms)) << "the activation deadline fired early";
    EXPECT_LT(span, ns(800ms)) << "the activation deadline fired " << span / 1'000'000 << " ms in: the parked core slept past it";
    EXPECT_LT(cpu(after) - cpu(before), span / 2) << "the process ran " << (cpu(after) - cpu(before)) / 1'000'000 << " ms of CPU over a "
                                                  << span / 1'000'000 << " ms activation: the core spun instead of parking until its deadline";
}

TEST(DelayClock, AClockNeverSetLeavesTheDateCheckSilent) {
    // A host whose clock was never set reads about its uptime as the time of day -- as the monotonic clock does -- so the
    // debug check that names a date cannot tell one from a deadline, and must not fire on a deadline built right.
    using namespace delay_clock_test;
    auto const now     = qb::mono_now();
    time_of_day_step   = ns(now.time_since_epoch()) - ns(std::chrono::system_clock::now().time_since_epoch()); // reads the uptime
    bool const stepped = qb::unix_nanos(qb::wall_now()) < std::int64_t{978'307'200'000'000'000};               // before 2001: a clock never set
    bool const near    = qb::detail::near_the_time_of_day(qb::detail::mono_nanos() + static_cast<std::uint64_t>(ns(500ms)));
    time_of_day_step   = 0;
    ASSERT_TRUE(stepped) << "the injected time of day did not read the uptime";
    EXPECT_FALSE(near) << "a deadline 500 ms ahead on the monotonic clock read as a date";
}

// ===========================================================================
// A core parked on its mailbox's condition variable (Huly QB-1227)
// ===========================================================================

namespace delay_clock_test {

struct park_outcome {
    std::atomic<bool>         ran{false};
    std::atomic<std::int64_t> landed{0}; ///< the step as `ctx.time()` saw it
    std::atomic<std::int64_t> took{-1};  ///< the ask's arm to its `timeout_error`, on `qb::mono_now()`, ns
};

/// On a core with a latency -- one that parks on its mailbox's condition variable between passes, each park capped by
/// the latency and by the earliest request deadline -- sets this process's time of day ahead of the kernel's, as a time
/// mock does, then asks a silent responder for 300 ms.
class ParkedAsker : public qb::Actor {
    std::int64_t                  _step;
    qb::ActorId                   _silent;
    std::shared_ptr<park_outcome> _out;

public:
    ParkedAsker(std::int64_t step, qb::ActorId silent, std::shared_ptr<park_outcome> out)
        : _step(step)
        , _silent(silent)
        , _out(std::move(out)) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::Probe>(*this);
        spawn([step = _step, silent = _silent, out = _out](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto const wall  = c.time();
            auto const mono  = qb::mono_now();
            time_of_day_step = step;
            co_await c.sleep(1ms);
            out->landed   = landed(wall, c.time(), mono, qb::mono_now());
            auto const t0 = qb::mono_now();
            try {
                (void) co_await qb::ask(c, silent, qb::test::Probe{1}, 300ms); // no io watcher: the core parks on the cv
            } catch (qb::io::async::timeout_error const &) {
            }
            out->took = ns(qb::mono_now() - t0);
            out->ran  = true;
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(qb::test::Probe &e) {
        resolve_ask(e);
    }
};

[[nodiscard]] inline std::shared_ptr<park_outcome>
parked_ask_across_a_step(std::int64_t step, std::int64_t &cpu_ns) {
    auto const out = std::make_shared<park_outcome>();
    qb::Main   main;
    main.core(0).setLatency(1ms);
    auto const silent = main.addActor<qb::test::SilentResponder>(0);
    main.addActor<ParkedAsker>(0, step, silent, out);
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    main.start();
    end_run(main, out->ran, 30s);
    getrusage(RUSAGE_SELF, &after);
    auto const cpu = [](rusage const &u) {
        return std::int64_t{u.ru_utime.tv_sec + u.ru_stime.tv_sec} * kSeconds + std::int64_t{u.ru_utime.tv_usec + u.ru_stime.tv_usec} * 1000;
    };
    cpu_ns = cpu(after) - cpu(before);
    EXPECT_FALSE(main.hasError());
    return out;
}

} // namespace delay_clock_test

TEST(DelayClock, ACoreParkedOnItsMailboxWaitsOnTheMonotonicClock) {
    // This process's time of day 5 s ahead of the kernel's while a core with a 1 ms latency parks through a 300 ms ask: a
    // park whose end is computed from `system_clock::now()` -- libc++'s condition_variable on Apple -- lasts 5 s longer,
    // and the ask times out 5.3 s in. Timed on the monotonic clock, at 300 ms.
    using namespace delay_clock_test;
    std::int64_t cpu = 0;
    auto const   r   = parked_ask_across_a_step(5 * kSeconds, cpu);
    ASSERT_TRUE(r->ran);
    ASSERT_GT(r->landed, 4 * kSeconds) << "the injected step did not reach the core's ctx.time()";
    EXPECT_GE(r->took, ns(300ms)) << "the ask timed out early";
    EXPECT_LT(r->took, ns(2500ms)) << "the ask timed out " << r->took / 1'000'000 << " ms in: the parked core waited on the time of day";
    std::printf("        [park] the ask timed out %lld ms in, %lld us of CPU\n", static_cast<long long>(r->took / 1'000'000),
                static_cast<long long>(cpu / 1000));
    EXPECT_LT(cpu, r->took / 4) << "the process ran " << cpu / 1'000'000 << " ms of CPU over a " << r->took / 1'000'000
                                << " ms ask: the core spun instead of parking (a park takes ~4 % of it)";
}

TEST(DelayClock, ACoreParkedOnItsMailboxWaitsItsDeadlineWithoutAStep) {
    // The positive control: the same parked ask with the system's time of day.
    using namespace delay_clock_test;
    std::int64_t cpu = 0;
    auto const   r   = parked_ask_across_a_step(0, cpu);
    ASSERT_TRUE(r->ran);
    EXPECT_GE(r->took, ns(300ms)) << "the ask timed out early";
    EXPECT_LT(r->took, ns(2500ms)) << "the ask timed out " << r->took / 1'000'000 << " ms in";
    std::printf("        [park] the ask timed out %lld ms in, %lld us of CPU\n", static_cast<long long>(r->took / 1'000'000),
                static_cast<long long>(cpu / 1000));
    EXPECT_LT(cpu, r->took / 4) << "the process ran " << cpu / 1'000'000 << " ms of CPU over a " << r->took / 1'000'000
                                << " ms ask: the core spun instead of parking (a park takes ~4 % of it)";
}

TEST(DelayClock, ATestKitWatchdogWaitsOnTheMonotonicClock) {
    // The TestKit's watchdog is a condition-variable wait on the test's own thread (Huly QB-1227): with this process's time
    // of day 5 s ahead of the kernel's, a wait whose end is computed from `system_clock::now()` fires 5 s late.
    using namespace delay_clock_test;
    struct lift_the_step {
        ~lift_the_step() {
            time_of_day_step = 0;
        }
    } const lift;
    qb::test::TestKit kit;
    time_of_day_step = 5 * kSeconds;
    timespec real{};
    ::clock_gettime(CLOCK_REALTIME, &real);
    auto const ahead = ns(std::chrono::system_clock::now().time_since_epoch()) - (std::int64_t{real.tv_sec} * kSeconds + real.tv_nsec);
    ASSERT_GT(ahead, 4 * kSeconds) << "the injected time of day did not reach system_clock::now()";
    auto const t0   = qb::mono_now();
    auto const r    = kit.run([](qb::ScopedCoroContext c) -> qb::io::async::task<void> { co_await c.sleep(30s); }, 300ms);
    auto const took = ns(qb::mono_now() - t0);
    EXPECT_TRUE(r.timed_out) << "a scenario sleeping 30 s ended before a 300 ms watchdog";
    EXPECT_GE(took, ns(300ms)) << "the watchdog fired early";
    EXPECT_LT(took, ns(2500ms)) << "the watchdog fired " << took / 1'000'000 << " ms in: its end was computed from the time of day";
}

TEST(DelayClock, ATestKitWatchdogOfDurationMaxWaitsForTheScenario) {
    // A watchdog of `qb::duration::max()` -- no watchdog -- must not overflow the wait's end into the past: the run waits
    // for the scenario, which ends on its own.
    using namespace delay_clock_test;
    qb::test::TestKit kit;
    auto const        t0   = qb::mono_now();
    auto const        r    = kit.run([](qb::ScopedCoroContext c) -> qb::io::async::task<void> { co_await c.sleep(10ms); }, qb::duration::max());
    auto const        took = ns(qb::mono_now() - t0);
    EXPECT_FALSE(r.timed_out) << "a watchdog of duration::max() fired: its end overflowed";
    EXPECT_TRUE(r.completed);
    EXPECT_GE(took, ns(10ms));
}

TEST(DelayClock, ACoreWithTheLongestLatencyStillSeesAStop) {
    // `setLatency(qb::duration::max())`: one park lasts a second at most (Huly QB-1227) -- an untimed park would never
    // see `qb::Main::stop()`, which wakes no parked core, and one past the clock's range returned at once. A hang here
    // ends the binary after 10 s rather than the run.
    std::atomic<bool> joined{false};
    std::thread       watchdog([&joined] {
        auto const until = qb::mono_now() + 10s;
        while (!joined && qb::mono_now() < until)
            std::this_thread::sleep_for(10ms);
        if (!joined) {
            std::fprintf(stderr, "DelayClock.ACoreWithTheLongestLatencyStillSeesAStop: the stop was never observed\n");
            std::_Exit(3);
        }
    });
    {
        qb::Main main;
        main.core(0).setLatency(qb::duration::max());
        main.addActor<qb::test::SilentResponder>(0);
        main.start();
        std::this_thread::sleep_for(50ms);
        qb::Main::stop();
        main.join();
        joined = true;
        EXPECT_FALSE(main.hasError());
    }
    watchdog.join();
}

// ===========================================================================
// A TestKit run of no watchdog when the engine stops before its scenario ends (Huly QB-1227)
// ===========================================================================

namespace delay_clock_test {

struct Boom : qb::Event {};

/// Throws out of its handler: the exception unwinds its core's thread and stops every actor on it.
class Thrower : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Boom>(*this);
        co_return true;
    }
    void
    on(Boom const &) {
        throw std::runtime_error("boom");
    }
};

/// Throws from its constructor: its core's factories stop there, and an actor registered after it is never built.
class ThrowsWhenBuilt : public qb::Actor {
public:
    ThrowsWhenBuilt() {
        throw std::runtime_error("not built");
    }
};

/// Runs @p body; a body still running after @p limit ends the binary with exit code 3 -- a hang is a failure, not a
/// stuck suite.
template <typename Body>
void
or_exit_after(std::chrono::seconds const limit, char const *what, Body &&body) {
    std::atomic<bool> done{false};
    std::thread       watchdog([&done, limit, what] {
        auto const until = qb::mono_now() + limit;
        while (!done && qb::mono_now() < until)
            std::this_thread::sleep_for(10ms);
        if (!done) {
            std::fprintf(stderr, "%s: the run never returned\n", what);
            std::_Exit(3);
        }
    });
    body();
    done = true;
    watchdog.join();
}

} // namespace delay_clock_test

// A run given no watchdog (`qb::duration::max()`) waits for its scenario without end since QB-1227. An engine that stops
// before the scenario ends destroys the scenario's frame with its core, and no catch block of it runs: the driver's
// destructor ends the wait then, the scenario reported as not run to its end. Before, such a run never returned.
TEST(DelayClock, ATestKitRunOfNoWatchdogEndsWhenItsScenarioStopsTheEngineAndSuspends) {
    using namespace delay_clock_test;
    or_exit_after(20s, "DelayClock.ATestKitRunOfNoWatchdogEndsWhenItsScenarioStopsTheEngineAndSuspends", [] {
        qb::test::TestKit kit;
        auto const        run = kit.run(
            [](qb::ScopedCoroContext) -> qb::io::async::task<void> {
                qb::Main::stop();
                co_await qb::io::async::sleep(std::chrono::hours(1)); // no cancellation reaches it: the frame dies with its core
            },
            qb::duration::max());
        EXPECT_FALSE(run);
        EXPECT_FALSE(run.completed);
        EXPECT_FALSE(run.timed_out) << "no watchdog, nothing to time out";
        EXPECT_FALSE(run.error) << "the frame was destroyed, never resumed: nothing thrown";
    });
}

TEST(DelayClock, ATestKitRunOfNoWatchdogEndsWhenAnotherActorsExceptionStopsTheEngine) {
    using namespace delay_clock_test;
    or_exit_after(20s, "DelayClock.ATestKitRunOfNoWatchdogEndsWhenAnotherActorsExceptionStopsTheEngine", [] {
        qb::test::TestKit kit;
        auto const        thrower = kit.add<Thrower>(0);
        auto const        run     = kit.run(
            [thrower](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                ctx.push_to<Boom>(thrower);
                co_await ctx.sleep(std::chrono::hours(1)); // the driver's core dies of the exception under it
            },
            qb::duration::max());
        EXPECT_FALSE(run);
        EXPECT_TRUE(run.engine_error) << run.describe();
        EXPECT_FALSE(run.completed);
        EXPECT_FALSE(run.timed_out);
    });
}

TEST(DelayClock, ATestKitRunOfNoWatchdogEndsWhenTheEngineFailsToStart) {
    using namespace delay_clock_test;
    or_exit_after(20s, "DelayClock.ATestKitRunOfNoWatchdogEndsWhenTheEngineFailsToStart", [] {
        qb::test::TestKit kit;
        (void) kit.engine().core(1); // a registered core with no actor: the engine refuses to start
        auto const run = kit.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, qb::duration::max());
        EXPECT_FALSE(run);
        EXPECT_TRUE(run.engine_error) << run.describe();
        EXPECT_FALSE(run.completed);
    });
}

TEST(DelayClock, ATestKitRunOfNoWatchdogEndsWhenItsDriverIsNeverBuilt) {
    // An actor registered before the driver throws from its constructor: the startup stops at it, the driver is never
    // built, and no destructor of it can end the wait -- `run()` does, on the refused start.
    using namespace delay_clock_test;
    or_exit_after(20s, "DelayClock.ATestKitRunOfNoWatchdogEndsWhenItsDriverIsNeverBuilt", [] {
        qb::test::TestKit kit;
        (void) kit.add<ThrowsWhenBuilt>(0);
        auto const run = kit.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, qb::duration::max());
        EXPECT_FALSE(run);
        EXPECT_TRUE(run.engine_error) << run.describe();
        EXPECT_FALSE(run.completed);
        EXPECT_FALSE(run.timed_out);
    });
}
