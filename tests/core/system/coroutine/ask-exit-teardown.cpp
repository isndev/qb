/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/ask-exit-teardown.cpp
 * @brief `exit()` from an actor's coroutine destroys no coroutine frame under it, whether the core's drain resumed
 *        that coroutine, its `ask` reply did, inline, or the core resumed an `onInit()` by hand (Huly QB-1246) -- and
 *        the mark the inline resume leaves is exact: a scheduler the resumed coroutine built for itself still
 *        destroys what it owns.
 *
 * `exit()` in a coroutine body destroys the thread's `listener::current` from inside that coroutine, and with it the
 * thread's scheduler. A coroutine of that scheduler is executing, and which root it belongs to is not known, so the
 * scheduler destroys nothing: it abandons the coroutines it owns and reports them on one line, "FROM INSIDE one of
 * its own coroutines". Under the core's drain the scheduler sees its own `run_ready()` on the stack. An `ask` reply
 * resumes the asking coroutine INLINE instead, from the handler that routed the reply (`ask_awaiter::deliver_thunk`),
 * with no drain on the stack: that resume marks the thread for its duration, and a scheduler destroyed meanwhile
 * abandons when it is the thread's listener's or the current one. `VirtualCore::__drive_init__` resumes an actor's
 * `onInit()` the same way, by hand, and marks it too. Until 3.4 every one of these teardowns destroyed coroutines of
 * the core under the executing one: their locals were destroyed under it. And the listener's teardown RESUMES none
 * meanwhile: a coroutine queued behind the one that exits stays queued, and is abandoned with the rest.
 *
 * The exit cases run in a CHILD process (a death test, re-executed): an abandoned coroutine is a leak by design.
 * Their judge is an `atexit` handler registered before the engine is built, so it runs after every destructor the
 * exit runs, and ends the child with `std::_Exit` and its verdict. The engine runs its core on the calling thread
 * (`start(false)`), so that thread's listener is the one the exit destroys.
 */

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace std::chrono_literals;

// A named namespace: these actors hand coroutines to the framework's spawning templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace ask_exit_teardown_test {

int         g_frame_locals      = 0;  ///< locals of the actors' coroutines destroyed
std::size_t g_tally0            = 0;  ///< `abandoned_coroutine_frames_total()` when the case started
int         g_observed_draining = -1; ///< `is_draining_ready()` as the exiting coroutine saw it, once resumed
int         g_expected_draining = -1; ///< ... as the case expects it: 1 under the drain, 0 inline
std::size_t g_expected_tally    = 0;  ///< the coroutines the thread's scheduler owns when the exit comes

/// A coroutine local: destroyed with its frame, or at the end of its body.
struct frame_local {
    frame_local()                               = default;
    frame_local(frame_local const &)            = delete;
    frame_local &operator=(frame_local const &) = delete;
    ~frame_local() {
        ++g_frame_locals;
    }
};

struct Question : qb::Request<int> {};

/// Answers every `Question` with 42.
class Echo final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        co_return true;
    }
    void
    on(Question &q) {
        qb::answer(*this, q, [](Question const &) { return 42; });
    }
};

/// How the coroutine that calls `exit()` was resumed last.
enum class resumed_by {
    drain,     ///< by the core's `run_ready()`, after a `co_await sleep`
    ask_reply, ///< inline, by the handler that routed its `ask` reply
};

/// Spawns a bystander parked on a timer and a coroutine that calls `exit(0)` once resumed as `how` says.
class Exiter final : public qb::Actor {
    qb::ActorId _target;
    resumed_by  _how;

public:
    Exiter(qb::ActorId const target, resumed_by const how)
        : _target(target)
        , _how(how) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        spawn([](qb::ScopedCoroContext) -> qb::io::async::task<void> {
            frame_local local;
            co_await qb::io::async::sleep(std::chrono::hours(1));
        });
        spawn([target = _target, how = _how](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            frame_local local;
            if (how == resumed_by::ask_reply)
                (void) co_await qb::ask(ctx, target, Question{}, std::chrono::hours(1));
            else
                co_await qb::io::async::sleep(1ms);
            // What resumed it, as observed: the thread's scheduler is draining, or the reply resumed it inline.
            g_observed_draining = qb::io::async::coro_scheduler().is_draining_ready() ? 1 : 0;
            std::fprintf(stderr, "the coroutine, resumed with is_draining_ready() %d, calls std::exit(0)\n", g_observed_draining);
            std::fflush(stderr);
            std::exit(0);
        });
        co_return true;
    }
    void
    on(Question &q) {
        (void) resolve_ask(q);
    }
};

void
judge_exit_from_an_actor_coroutine() {
    const std::size_t moved = qb::io::async::abandoned_coroutine_frames_total() - g_tally0;
    std::fprintf(stderr,
                 "judge: resumed with is_draining_ready() %d (%d expected), coroutine locals destroyed %d (0 expected), "
                 "tally moved by %zu (%zu expected)\n",
                 g_observed_draining, g_expected_draining, g_frame_locals, moved, g_expected_tally);
    std::fflush(stderr);
    std::_Exit(g_observed_draining == g_expected_draining && g_frame_locals == 0 && moved == g_expected_tally ? 0 : 1);
}

/// One core, on this thread: the exiter's coroutine calls `exit(0)` once resumed as `how` says.
[[noreturn]] void
an_actor_coroutine_exits(resumed_by const how) {
    g_tally0            = qb::io::async::abandoned_coroutine_frames_total();
    g_expected_draining = how == resumed_by::drain ? 1 : 0;
    g_expected_tally    = 2;                         // the bystander and the coroutine that exits
    std::atexit(judge_exit_from_an_actor_coroutine); // before the engine: it runs after every destructor exit() runs
    qb::Main   engine;
    auto const echo = engine.addActor<Echo>(0);
    engine.addActor<Exiter>(0, echo, how);
    engine.start(false); // core 0 runs on this thread, until the coroutine exits
    engine.join();
    std::fprintf(stderr, "setup failed: the engine returned and no coroutine exited\n");
    std::fflush(stderr);
    std::_Exit(2);
}

// ---- exit() inline, with a coroutine queued behind it and a registration on the loop ------------------------------

int g_bystander_started = 0;  ///< the bystander reached its `recv()`
int g_woke_bystander    = -1; ///< the wake the exiting coroutine sent was taken: the bystander is queued

/// Parks a bystander on a channel and posts a delayed callback -- a registration on the thread's listener, whose
/// teardown therefore runs passes of the loop. Its second coroutine, resumed INLINE by its `ask` reply, wakes the
/// bystander -- QUEUED on the thread's scheduler from then on, behind the executing coroutine -- and calls `exit(0)`.
class ExiterWithAQueuedCoroutine final : public qb::Actor {
    qb::ActorId _target;

public:
    explicit ExiterWithAQueuedCoroutine(qb::ActorId const target)
        : _target(target) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        qb::io::async::callback([] {}, std::chrono::hours(1));
        auto ch = std::make_shared<qb::io::async::channel<int>>();
        spawn([ch](qb::ScopedCoroContext) -> qb::io::async::task<void> {
            frame_local local;
            g_bystander_started = 1;
            (void) co_await ch->recv(); // only a drain resumes it: none may run once the exit below has begun
        });
        spawn([target = _target, ch](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            frame_local local;
            (void) co_await qb::ask(ctx, target, Question{}, std::chrono::hours(1));
            g_observed_draining = qb::io::async::coro_scheduler().is_draining_ready() ? 1 : 0;
            g_woke_bystander    = g_bystander_started && ch->try_send(1) ? 1 : 0;
            std::fprintf(stderr, "the coroutine, resumed with is_draining_ready() %d, queues another (%d) and calls std::exit(0)\n",
                         g_observed_draining, g_woke_bystander);
            std::fflush(stderr);
            std::exit(0);
        });
        co_return true;
    }
    void
    on(Question &q) {
        (void) resolve_ask(q);
    }
};

void
judge_exit_with_a_coroutine_queued() {
    const std::size_t moved = qb::io::async::abandoned_coroutine_frames_total() - g_tally0;
    std::fprintf(stderr,
                 "judge: resumed with is_draining_ready() %d (0 expected), another coroutine queued %d (1 expected), coroutine "
                 "locals destroyed %d (0 expected), tally moved by %zu (2 expected)\n",
                 g_observed_draining, g_woke_bystander, g_frame_locals, moved);
    std::fflush(stderr);
    std::_Exit(g_observed_draining == 0 && g_woke_bystander == 1 && g_frame_locals == 0 && moved == 2 ? 0 : 1);
}

/// One core, on this thread: the exiter's coroutine, resumed inline, queues the bystander and calls `exit(0)`.
[[noreturn]] void
an_inline_resumed_coroutine_exits_with_another_queued() {
    g_tally0 = qb::io::async::abandoned_coroutine_frames_total();
    std::atexit(judge_exit_with_a_coroutine_queued); // before the engine: it runs after every destructor exit() runs
    qb::Main   engine;
    auto const echo = engine.addActor<Echo>(0);
    engine.addActor<ExiterWithAQueuedCoroutine>(0, echo);
    engine.start(false); // core 0 runs on this thread, until the coroutine exits
    engine.join();
    std::fprintf(stderr, "setup failed: the engine returned and no coroutine exited\n");
    std::fflush(stderr);
    std::_Exit(2);
}

// ---- exit() in an actor's onInit(), added at run time ---------------------------------------------------------------

struct AddLateActor : qb::Event {};

/// Its `onInit()` calls `exit(0)` from its body, resumed by hand -- inline -- by `VirtualCore::__drive_init__`.
class ExitsInInit final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_observed_draining = qb::io::async::coro_scheduler().is_draining_ready() ? 1 : 0;
        std::fprintf(stderr, "an actor's onInit(), resumed with is_draining_ready() %d, calls std::exit(0)\n", g_observed_draining);
        std::fflush(stderr);
        std::exit(0);
        co_return true;
    }
};

/// Parks a bystander coroutine on the core, then -- from a plain handler, the bystander parked -- adds at run time
/// an actor whose `onInit()` exits.
class Host final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<AddLateActor>(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            frame_local local;
            ctx.push<AddLateActor>(); // handled once this coroutine is parked
            co_await qb::io::async::sleep(std::chrono::hours(1));
        });
        co_return true;
    }
    void
    on(AddLateActor &) {
        (void) addRefActor<ExitsInInit>(); // its onInit() runs now, inside this handler
    }
};

/// One core, on this thread: the host's handler adds the actor whose `onInit()` exits.
[[noreturn]] void
an_actor_on_init_exits() {
    g_tally0            = qb::io::async::abandoned_coroutine_frames_total();
    g_expected_draining = 0; // a handler, not a drain, is on the stack
    g_expected_tally    = 1; // the bystander: the onInit() frame is driven by hand, never the scheduler's
    std::atexit(judge_exit_from_an_actor_coroutine);
    qb::Main engine;
    engine.addActor<Host>(0);
    engine.start(false);
    engine.join();
    std::fprintf(stderr, "setup failed: the engine returned and no onInit() exited\n");
    std::fflush(stderr);
    std::_Exit(2);
}

// ---- The exactness of the inline resume's mark (in process) ---------------------------------------------------------

/// What the coroutine below saw.
struct Outcome {
    bool        resumed_inline = false; ///< no drain on the stack when its ask reply resumed it
    int         root_started   = 0;     ///< its own scheduler's root ran to its park
    int         root_destroyed = 0;     ///< that root's local destroyed, wherever and whenever
    int         root_locals    = -1;    ///< ... read right after its scheduler went
    std::size_t tally_moved    = 99;    ///< the abandon tally, across that scheduler's destruction
    bool        finished       = false;
};
inline Outcome g;

struct own_root_local {
    own_root_local()                                  = default;
    own_root_local(own_root_local const &)            = delete;
    own_root_local &operator=(own_root_local const &) = delete;
    ~own_root_local() {
        ++g.root_destroyed;
    }
};

qb::io::async::task<void>
park_on(qb::io::async::channel<int> &ch) {
    own_root_local local;
    ++g.root_started;
    (void) co_await ch.recv(); // parks: registered with no scheduler
}

/// Its coroutine, resumed inline by its `ask` reply, builds a scheduler of its own -- neither the current one nor the
/// thread's listener's -- parks a root on it and destroys it, all inside that inline resume.
class SelfScheduling final : public qb::Actor {
    qb::ActorId _target;

public:
    explicit SelfScheduling(qb::ActorId const target)
        : _target(target) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        spawn([target = _target](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await qb::ask(ctx, target, Question{}, std::chrono::hours(1));
            g.resumed_inline        = !qb::io::async::coro_scheduler().is_draining_ready();
            const std::size_t tally = qb::io::async::abandoned_coroutine_frames_total();
            {
                qb::io::async::channel<int>       ch; // outlives the scheduler below
                qb::io::async::CoroutineScheduler own;
                own.spawn(park_on(ch));
                (void) own.run_ready(); // its root runs to its park
            } // ~CoroutineScheduler, inside the inline resume
            g.root_locals = g.root_destroyed;
            g.tally_moved = qb::io::async::abandoned_coroutine_frames_total() - tally;
            g.finished    = true;
            ctx.push_to<qb::KillEvent>(target);
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
    void
    on(Question &q) {
        (void) resolve_ask(q);
    }
};

} // namespace ask_exit_teardown_test

using namespace ask_exit_teardown_test;

/**
 * @test `exit(0)` from an actor's coroutine the core's drain resumed (after a `co_await sleep`; the coroutine observes
 *       `is_draining_ready()` true): the thread's scheduler, destroyed from inside one of its own coroutines, destroys
 *       nothing -- neither the executing frame's
 *       locals nor the bystander's -- abandons its two roots and says so. Base: the listener's teardown destroyed the
 *       executing frame under it -- UBSan's "member call on an address which does not point to an object of type
 *       timer_awaiter" with g++ under the sanitizers; with clang under them, in a g++ Release build and on MSVC
 *       Release, both locals destroyed in silence and nothing counted.
 */
TEST(AskExitTeardownDeathTest, ExitFromACoroutineTheDrainResumedDestroysNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Two children of the same scenario, one regex each -- the order the report names, the resume the coroutine
    // observed: two lines of stderr, and the simple regex GoogleTest uses on Windows matches no newline with `.`.
    // The judge's exit code requires both.
    EXPECT_EXIT(an_actor_coroutine_exits(resumed_by::drain), ::testing::ExitedWithCode(0), "destroyed FROM INSIDE one of its own coroutines");
    EXPECT_EXIT(an_actor_coroutine_exits(resumed_by::drain), ::testing::ExitedWithCode(0),
                "the coroutine, resumed with is_draining_ready\\(\\) 1, calls std::exit");
}

/**
 * @test `exit(0)` from an actor's coroutine its `ask` reply resumed INLINE, from the handler that routed the reply,
 *       with no drain on the stack (the coroutine observes `is_draining_ready()` false): the inline resume marks the
 *       thread, so the thread's scheduler destroys nothing
 *       and says so. Base: the executing frame was destroyed under it -- the locals of the coroutine that called
 *       `exit()` and of the bystander destroyed, nothing counted, no line (g++ and clang, sanitized and Release; MSVC
 *       Release).
 */
TEST(AskExitTeardownDeathTest, ExitFromACoroutineItsAskReplyResumedInlineDestroysNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(an_actor_coroutine_exits(resumed_by::ask_reply), ::testing::ExitedWithCode(0),
                "destroyed FROM INSIDE one of its own coroutines");
    EXPECT_EXIT(an_actor_coroutine_exits(resumed_by::ask_reply), ::testing::ExitedWithCode(0),
                "the coroutine, resumed with is_draining_ready\\(\\) 0, calls std::exit");
}

/**
 * @test `exit(0)` from a coroutine its `ask` reply resumed inline, with ANOTHER coroutine queued behind it -- woken
 *       by the exiting one, ready, not yet resumed -- and a delayed callback registered on the thread's listener, so
 *       that its teardown runs passes of the loop: those passes resume no coroutine. The queued one stays queued, its
 *       local is not destroyed, and the thread's scheduler abandons both roots. Until the fix: the listener's
 *       teardown ran whole passes, the queued coroutine was resumed in the middle of the exit, ran to its end and
 *       destroyed its local, and one root was abandoned. Base: locals destroyed, nothing counted, no line.
 */
TEST(AskExitTeardownDeathTest, ExitFromAnInlineResumedCoroutineResumesNoneQueuedBehindIt) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(an_inline_resumed_coroutine_exits_with_another_queued(), ::testing::ExitedWithCode(0),
                "destroyed FROM INSIDE one of its own coroutines with 2 coroutine");
    EXPECT_EXIT(an_inline_resumed_coroutine_exits_with_another_queued(), ::testing::ExitedWithCode(0),
                "resumed with is_draining_ready\\(\\) 0, queues another \\(1\\) and calls std::exit");
}

/**
 * @test `exit(0)` from an actor's `onInit()`, the actor added at run time from a plain handler -- the core already
 *       runs coroutines: a bystander is parked on it. `VirtualCore::__drive_init__` resumes `onInit()` by hand, with
 *       no drain on the stack (it observes `is_draining_ready()` false), and marks the thread for that resume as an
 *       `ask` reply does: the thread's scheduler destroys nothing -- the bystander's local is not destroyed -- abandons
 *       its one root and says so. Base: the bystander's local destroyed, nothing counted, no line.
 */
TEST(AskExitTeardownDeathTest, ExitFromAnOnInitResumedByHandDestroysNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(an_actor_on_init_exits(), ::testing::ExitedWithCode(0), "destroyed FROM INSIDE one of its own coroutines");
    EXPECT_EXIT(an_actor_on_init_exits(), ::testing::ExitedWithCode(0),
                "an actor's onInit\\(\\), resumed with is_draining_ready\\(\\) 0, calls std::exit");
}

/**
 * @test The inline resume's mark is exact: a scheduler the resumed coroutine built for itself -- not the current
 *       one, not the thread's listener's -- destroyed in that coroutine's body during the inline resume, DESTROYS the
 *       root it owns (its local is destroyed, nothing is reported). A mark that made every scheduler abandon would
 *       leak that root. Base: the root was left undestroyed -- the destructor destroyed no parked frame -- on every
 *       build (g++ and clang, sanitized and Release; MSVC Release).
 */
TEST(AskExitTeardown, ASchedulerAnInlineResumedCoroutineBuiltForItselfDestroysWhatItOwns) {
    g = Outcome{};
    qb::Main   engine;
    auto const echo = engine.addActor<Echo>(0);
    engine.addActor<SelfScheduling>(0, echo);
    engine.start();
    engine.join();
    EXPECT_FALSE(engine.hasError());
    ASSERT_TRUE(g.finished) << "the coroutine never got past its ask";
    EXPECT_TRUE(g.resumed_inline) << "the ask reply did not resume the coroutine inline: the case pins nothing";
    EXPECT_EQ(g.root_started, 1) << "the scheduler's root never ran to its park";
    EXPECT_EQ(g.root_locals, 1) << "the scheduler built inside the inline resume did not destroy its root";
    EXPECT_EQ(g.tally_moved, 0u) << "a scheduler built inside the inline resume reported an abandon";
}
