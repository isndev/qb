/**
 * @file unit/async/listener-teardown-drain.cpp
 * @brief `~listener` ends with nothing registered and no scheduler: what its own teardown registers or spawns --
 *        a coroutine local's destructor that posts an `async::callback`, a callback's closure whose destructor
 *        spawns a coroutine -- is torn down again, sixteen passes at most (Huly QB-1246).
 *
 * The thread's listener is destroyed at its thread's exit: `clear()` detaches what is registered and destroys the
 * loop-owned handlers (an `async::callback`'s timer and its closure), then the scheduler destroys the coroutines it
 * owns. Until 3.4 that was one pass. A coroutine local's destructor that posted a callback registered it AFTER
 * `clear()`, on a loop about to be destroyed: the handler and the state its closure captured were never destroyed
 * (LeakSanitizer reports the handler; the cases count the closure's destruction, so they are red on the base with
 * no sanitizer too). The destructor now repeats `clear()` and the scheduler's teardown while either left something,
 * as a scheduler repeats its own passes; past the sixteenth, what is still registered is detached and left to its
 * owner, and the scheduler abandons what it owns and says so.
 *
 * `clear()` itself is bounded too, twice. The loop-owned owners it destroys are BUDGETED: sixteen times what was
 * registered when its drain began; past the budget a handler is detached like any other and its owner left alive.
 * And it drops sixteen turns of deferred callbacks at most; what the sixteenth leaves is given up, never run and
 * never destroyed. Without them a closure that posts another callback, or defers another closure, at every
 * destruction kept `clear()` -- an explicit one, or the one `~listener` runs at a thread's exit -- looping for ever
 * (Huly QB-1246): the base did, for a closure there before the teardown; the teardown passes above made the same
 * loop of one posted DURING the teardown, which the base leaked instead. A control pins that the budget does not
 * cut a teardown that ends by itself.
 *
 * Each case runs on a WORKER thread: its `listener::current` is destroyed at the thread's exit, its loop built by
 * the coroutine that sleeps there or by the callback it posts. (A temporary `listener` does not apply:
 * `async::callback` and `async::defer` post on the THREAD's listener, never on a listener of one's own.) What a
 * bound leaves is leaked by design, so the unbounded shapes run in child processes that end with `std::_Exit`, past
 * LeakSanitizer -- each with a run-away guard that ends the child, with a message, past a hundred thousand
 * generations: an unbounded drain FAILS instead of hanging.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace listener_teardown_drain_test {

int g_locals_gone   = 0; ///< coroutine locals destroyed
int g_frames_gone   = 0; ///< generations of coroutines destroyed (their by-value parameter)
int g_closures_gone = 0; ///< generations of callback closures destroyed (their captured object)
int g_deferred_gone = 0; ///< generations of deferred closures destroyed (their captured state)

/// A generation count no bounded teardown reaches: past it the child process stops with a message rather than spin.
constexpr int kRunAway = 100000;

[[noreturn]] void
run_away(int const generation) {
    std::fprintf(stderr, "listener-teardown-drain: generation %d and counting -- an unbounded teardown\n", generation);
    std::fflush(stderr);
    std::_Exit(3);
}

/// Counts its own destruction, once (a move leaves the source with nothing to count).
class counted {
public:
    explicit counted(int &count) noexcept
        : _count(&count) {}
    counted(counted &&other) noexcept
        : _count(std::exchange(other._count, nullptr)) {}
    counted(counted const &)            = delete;
    counted &operator=(counted const &) = delete;
    counted &operator=(counted &&)      = delete;
    ~counted() {
        if (_count)
            ++*_count;
    }

private:
    int *_count;
};

/// (a) A coroutine local whose destructor posts a callback ten seconds away, its closure holding a `counted`.
struct posts_when_destroyed {
    posts_when_destroyed()                                        = default;
    posts_when_destroyed(posts_when_destroyed const &)            = delete;
    posts_when_destroyed &operator=(posts_when_destroyed const &) = delete;
    ~posts_when_destroyed() {
        callback([held = counted{g_closures_gone}] { (void) held; }, 10s);
    }
};

task<void>
parked_posting() {
    counted              local{g_locals_gone};
    posts_when_destroyed post;
    co_await sleep(1h);
}

// (b), (c): generations. A coroutine's by-value parameter posts a callback when the frame is destroyed; that
// callback's closure, when IT is destroyed, spawns a coroutine carrying the next parameter on the thread's
// scheduler -- `limit` generations of each, or for ever with a limit of 0. One generation per teardown pass of
// `~listener`: `clear()` destroys the closure (which spawns), the scheduler's teardown destroys the coroutine (which
// posts).
class frame_generation;

class closure_generation {
public:
    closure_generation(int const n, int const limit) noexcept
        : _n(n)
        , _limit(limit) {}
    closure_generation(closure_generation &&other) noexcept
        : _n(other._n)
        , _limit(other._limit)
        , _armed(std::exchange(other._armed, false)) {}
    closure_generation(closure_generation const &)            = delete;
    closure_generation &operator=(closure_generation const &) = delete;
    closure_generation &operator=(closure_generation &&)      = delete;
    ~closure_generation();

private:
    int  _n;
    int  _limit;
    bool _armed = true;
};

class frame_generation {
public:
    frame_generation(int const n, int const limit) noexcept
        : _n(n)
        , _limit(limit) {}
    frame_generation(frame_generation &&other) noexcept
        : _n(other._n)
        , _limit(other._limit)
        , _armed(std::exchange(other._armed, false)) {}
    frame_generation(frame_generation const &)            = delete;
    frame_generation &operator=(frame_generation const &) = delete;
    frame_generation &operator=(frame_generation &&)      = delete;
    ~frame_generation() {
        if (!_armed)
            return;
        ++g_frames_gone;
        callback([next = closure_generation{_n + 1, _limit}] { (void) next; }, 10s);
    }

private:
    int  _n;
    int  _limit;
    bool _armed = true;
};

task<void>
carrier(frame_generation generation) {
    co_await sleep(1h);
    (void) generation;
}

closure_generation::~closure_generation() {
    if (!_armed)
        return;
    ++g_closures_gone;
    if (_n >= kRunAway)
        run_away(_n);
    if (_limit == 0 || _n < _limit)
        coro_scheduler().spawn(carrier(frame_generation{_n, _limit}));
}

/// A worker thread that spawns `root` on its listener's scheduler, runs it to its park, and returns: its listener
/// is destroyed at its exit.
template <typename Root>
void
on_a_thread_that_returns(Root root) {
    std::thread([&root] {
        coro_scheduler().spawn(root());
        coro_scheduler().run_ready(); // the root runs to its sleep: the thread's loop is built
    }).join();
}

void
reset_counts() {
    g_locals_gone = g_frames_gone = g_closures_gone = g_deferred_gone = 0;
}

// ---- the budgets of clear() ------------------------------------------------------------------------------------

/// (d) A callback's closure that posts the next callback when it is destroyed, at EVERY generation.
class reposts_when_destroyed {
public:
    explicit reposts_when_destroyed(int const n) noexcept
        : _n(n) {}
    reposts_when_destroyed(reposts_when_destroyed &&other) noexcept
        : _n(other._n)
        , _armed(std::exchange(other._armed, false)) {}
    reposts_when_destroyed(reposts_when_destroyed const &)            = delete;
    reposts_when_destroyed &operator=(reposts_when_destroyed const &) = delete;
    reposts_when_destroyed &operator=(reposts_when_destroyed &&)      = delete;
    ~reposts_when_destroyed() {
        if (!_armed)
            return;
        ++g_closures_gone;
        if (_n >= kRunAway)
            run_away(_n);
        callback([next = reposts_when_destroyed{_n + 1}] { (void) next; }, 10s);
    }

private:
    int  _n;
    bool _armed = true;
};

void
post_a_reposting_callback() {
    callback([first = reposts_when_destroyed{1}] { (void) first; }, 10s);
}

/// A coroutine local that posts the first of them when it is destroyed: during `~listener`, after its `clear()`.
struct posts_a_reposting_callback {
    posts_a_reposting_callback()                                              = default;
    posts_a_reposting_callback(posts_a_reposting_callback const &)            = delete;
    posts_a_reposting_callback &operator=(posts_a_reposting_callback const &) = delete;
    ~posts_a_reposting_callback() {
        post_a_reposting_callback();
    }
};

task<void>
parked_posting_a_reposting_callback() {
    posts_a_reposting_callback post;
    co_await sleep(1h);
}

/// (e) A deferred closure's captured state that defers the next closure when it is destroyed, at EVERY generation.
/// Held through a `shared_ptr`: a `std::function` that copies its closure changes nothing, the generation ends when
/// its last copy goes.
struct defers_when_destroyed {
    explicit defers_when_destroyed(int const generation) noexcept
        : n(generation) {}
    defers_when_destroyed(defers_when_destroyed const &)            = delete;
    defers_when_destroyed &operator=(defers_when_destroyed const &) = delete;
    ~defers_when_destroyed();
    int n;
};

void
defer_generation(int const n) {
    qb::io::async::defer([held = std::make_shared<defers_when_destroyed>(n)] { (void) held; });
}

defers_when_destroyed::~defers_when_destroyed() {
    ++g_deferred_gone;
    if (n >= kRunAway)
        run_away(n);
    defer_generation(n + 1);
}

/// (f) The control: a callback's closure that posts ONE more callback when it is destroyed, whose closure posts none.
class posts_one_more {
public:
    posts_one_more() noexcept = default;
    posts_one_more(posts_one_more &&other) noexcept
        : _armed(std::exchange(other._armed, false)) {}
    posts_one_more(posts_one_more const &)            = delete;
    posts_one_more &operator=(posts_one_more const &) = delete;
    posts_one_more &operator=(posts_one_more &&)      = delete;
    ~posts_one_more() {
        if (!_armed)
            return;
        ++g_closures_gone;
        callback([last = counted{g_closures_gone}] { (void) last; }, 10s);
    }

private:
    bool _armed = true;
};

constexpr int kPosters = 100;

void
post_the_posters() {
    for (int i = 0; i < kPosters; ++i)
        callback([poster = posts_one_more{}] { (void) poster; }, 10s);
}

/// What a child reads after one of the shapes above returned, then its exit: 0 when `destroyed` is `expected`.
[[noreturn]] void
generations_verdict(char const *const shape, int const destroyed, int const expected) {
    std::fprintf(stderr, "listener-teardown-drain: %s: %d generations destroyed\n", shape, destroyed);
    std::fflush(stderr);
    std::_Exit(destroyed == expected ? 0 : 1);
}

// One handler is registered when the drain that meets it begins: sixteen owners destroyed, and the closure the
// sixteenth posts is the one detached and left. One closure is deferred when the drop begins: sixteen turns destroy
// one each, and the closure the sixteenth defers is the one given up.
constexpr int kOneHandlersBudget = 16;
constexpr int kDeferredTurns     = 16;

[[noreturn]] void
reposting_callback_posted_during_the_teardown() {
    reset_counts();
    on_a_thread_that_returns([] { return parked_posting_a_reposting_callback(); });
    generations_verdict("a re-posting callback posted during ~listener", g_closures_gone, kOneHandlersBudget);
}

[[noreturn]] void
reposting_callback_posted_before_the_thread_exits() {
    reset_counts();
    std::thread([] { post_a_reposting_callback(); }).join();
    generations_verdict("a re-posting callback posted before the thread's exit", g_closures_gone, kOneHandlersBudget);
}

[[noreturn]] void
repost_then_clear() {
    reset_counts();
    int after_clear = -1;
    std::thread([&after_clear] {
        post_a_reposting_callback();
        listener::current.clear();
        after_clear = g_closures_gone;
    }).join(); // the thread's exit destroys none more: what the budget left is detached
    int const destroyed = after_clear == g_closures_gone ? after_clear : -1; // -1: the thread's exit destroyed more
    generations_verdict("a re-posting callback, then clear()", destroyed, kOneHandlersBudget);
}

[[noreturn]] void
redeferring_closure_then_clear() {
    reset_counts();
    int after_clear = -1;
    std::thread([&after_clear] {
        defer_generation(1);
        listener::current.clear();
        after_clear = g_deferred_gone;
    }).join(); // the thread's exit destroys none more: what the sixteenth turn left is given up
    int const destroyed = after_clear == g_deferred_gone ? after_clear : -1; // -1: the thread's exit destroyed more
    generations_verdict("a re-deferring closure, then clear()", destroyed, kDeferredTurns);
}

[[noreturn]] void
redeferring_closure_then_the_thread_exits() {
    reset_counts();
    std::thread([] { defer_generation(1); }).join();
    generations_verdict("a re-deferring closure at the thread's exit", g_deferred_gone, kDeferredTurns);
}

/// The child of the unbounded case: generations for ever, then the verdict on stderr and `std::_Exit`, so that what
/// the sixteenth pass leaves -- leaked by design -- is not LeakSanitizer's to report.
[[noreturn]] void
endless_generations_then_verdict() {
    reset_counts();
    on_a_thread_that_returns([] { return carrier(frame_generation{0, 0}); });
    // One pass destroys the root, each of the fifteen others one closure and the coroutine it spawned; the closure
    // the last coroutine posted is still registered when the passes end, detached and left.
    bool const bounded = g_frames_gone == 16 && g_closures_gone == 15;
    std::fprintf(stderr, "listener-teardown-drain: the thread returned with %d coroutine and %d callback generations destroyed\n",
                 g_frames_gone, g_closures_gone);
    std::fflush(stderr);
    std::_Exit(bounded ? 0 : 1);
}

} // namespace listener_teardown_drain_test

using namespace listener_teardown_drain_test;

/**
 * @test (a) A coroutine parked on its worker thread's scheduler, whose local's destructor posts an `async::callback`
 *       ten seconds away: when the thread returns, its listener's destructor destroys the coroutine (the local posts
 *       the callback, registered after `clear()`), then destroys that callback -- its closure is destroyed, nothing
 *       leaks. On the base the closure was never destroyed and LeakSanitizer reported the handler.
 */
TEST(ListenerTeardownDrain, ACallbackPostedByACoroutineLocalsDestructorIsDestroyed) {
    reset_counts();
    on_a_thread_that_returns([] { return parked_posting(); });
    EXPECT_EQ(g_locals_gone, 1) << "the parked coroutine must be destroyed at its thread's exit";
    EXPECT_EQ(g_closures_gone, 1) << "the callback its local posted while the listener was torn down was never destroyed";
}

/**
 * @test (b) Five generations: a coroutine's parameter posts a callback when the coroutine is destroyed, the callback's
 *       closure spawns the next coroutine when it is destroyed, five times. When the thread returns, every coroutine
 *       and every closure has been destroyed. On the base the first closure was never destroyed (and with it no
 *       later generation existed).
 */
TEST(ListenerTeardownDrain, GenerationsOfCallbacksAndCoroutinesAreAllDestroyed) {
    constexpr int kGenerations = 5;
    reset_counts();
    on_a_thread_that_returns([] { return carrier(frame_generation{0, kGenerations}); });
    EXPECT_EQ(g_frames_gone, kGenerations) << "a coroutine of a later generation was never destroyed";
    EXPECT_EQ(g_closures_gone, kGenerations) << "a callback of a later generation was never destroyed";
}

/**
 * @test (c) The bound: generations that never end. The thread still returns -- after sixteen teardown passes of its
 *       listener, with sixteen coroutine generations and fifteen callback generations destroyed -- and what the last
 *       pass leaves stays registered, detached, leaked by design: hence a child process, its verdict on stderr and
 *       `std::_Exit`. An unbounded teardown would spin; the child stops itself past a hundred thousand generations
 *       and says so. On the base the thread returned after ONE pass: one coroutine, no callback destroyed.
 */
TEST(ListenerTeardownDrainDeathTest, AnEndlessTeardownReturnsAfterSixteenPasses) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(endless_generations_then_verdict(), ::testing::ExitedWithCode(0),
                "the thread returned with 16 coroutine and 15 callback generations destroyed");
}

/**
 * @test (d i) The literal shape of an unbounded drain: a callback's closure that posts another `async::callback`
 *       ten seconds away at every destruction, the first one posted by a coroutine local's destructor while the
 *       worker's listener is torn down. The pass after the one that posted it destroys sixteen generations -- the
 *       budget of one handler -- and detaches the seventeenth; the thread returns. On the base the thread returned
 *       after one pass with NO generation destroyed (the first closure leaked); with the teardown passes and no
 *       budget, it looped for ever.
 */
TEST(ListenerTeardownDrainDeathTest, ARepostingCallbackPostedDuringTheTeardownStopsAtTheBudget) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(reposting_callback_posted_during_the_teardown(), ::testing::ExitedWithCode(0),
                "a re-posting callback posted during ~listener: 16 generations destroyed");
}

/**
 * @test (d ii) The same closure, posted in the worker's body before it returns: the first `clear()` of its
 *       listener's destructor destroys sixteen generations and detaches the seventeenth. On the base that `clear()`
 *       looped for ever: the run-away guard ends the child, exit 3.
 */
TEST(ListenerTeardownDrainDeathTest, ARepostingCallbackPostedBeforeTheThreadExitsStopsAtTheBudget) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(reposting_callback_posted_before_the_thread_exits(), ::testing::ExitedWithCode(0),
                "a re-posting callback posted before the thread's exit: 16 generations destroyed");
}

/**
 * @test (d iii) The same closure, then an explicit `listener::current.clear()` on the worker: `clear()` returns
 *       after sixteen generations, and the thread's exit destroys none more. On the base `clear()` looped for ever
 *       (the run-away guard, exit 3).
 */
TEST(ListenerTeardownDrainDeathTest, ARepostingCallbackThenClearStopsAtTheBudget) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(repost_then_clear(), ::testing::ExitedWithCode(0), "re-posting callback, then clear..: 16 generations destroyed");
}

/**
 * @test (e) A deferred closure whose captured state defers another closure at every destruction, then an explicit
 *       `listener::current.clear()` on the worker: the drop returns after sixteen turns, one generation each, and
 *       gives the seventeenth up; the thread's exit destroys none more. On the base the drop looped for ever (the
 *       run-away guard, exit 3).
 */
TEST(ListenerTeardownDrainDeathTest, AReDeferringClosureThenClearStopsAfterSixteenTurns) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(redeferring_closure_then_clear(), ::testing::ExitedWithCode(0),
                "a re-deferring closure, then clear..: 16 generations destroyed");
}

/**
 * @test (e) The same closure, left deferred when the worker returns: its listener's destructor -- the loop never
 *       built -- drops sixteen turns and gives the seventeenth up. On the base it looped for ever (the run-away
 *       guard, exit 3).
 */
TEST(ListenerTeardownDrainDeathTest, AReDeferringClosureAtTheThreadsExitStopsAfterSixteenTurns) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(redeferring_closure_then_the_thread_exits(), ::testing::ExitedWithCode(0),
                "a re-deferring closure at the thread's exit: 16 generations destroyed");
}

/**
 * @test (f) The control: the budget does not cut a teardown that ends by itself. A hundred callbacks, each closure
 *       posting ONE more callback when it is destroyed (whose closure posts none), then an explicit `clear()` on the
 *       worker: all two hundred closures are destroyed and nothing is left registered -- in this process, under
 *       LeakSanitizer in the sanitize lane. The base destroyed them all too.
 */
TEST(ListenerTeardownDrain, TheBudgetDoesNotCutAClearThatEndsByItself) {
    reset_counts();
    std::size_t left = 1;
    std::thread([&left] {
        post_the_posters();
        listener::current.clear();
        left = listener::current.size();
    }).join();
    EXPECT_EQ(g_closures_gone, 2 * kPosters) << "clear() left a closure undestroyed";
    EXPECT_EQ(left, 0u) << "clear() left a handler registered";
}

/**
 * @test (f) The same control at the thread's exit: the worker returns with its hundred callbacks posted, and its
 *       listener's destructor destroys all two hundred closures. The base destroyed them all too.
 */
TEST(ListenerTeardownDrain, TheBudgetDoesNotCutAThreadsTeardownThatEndsByItself) {
    reset_counts();
    std::thread([] { post_the_posters(); }).join();
    EXPECT_EQ(g_closures_gone, 2 * kPosters) << "the thread's exit left a closure undestroyed";
}
