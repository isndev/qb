/**
 * @file test-actor-coroutine-scope.cpp
 * @brief Tests for actor-scoped coroutines: spawn + ScopedCoroContext +
 *        cooperative cancel-on-kill.
 *
 * Validates the contract from internal/plans/QB_ACTOR_CORO_SCOPE.md:
 *   - a scoped coroutine awaiting a cancellation-aware ScopedCoroContext op is woken
 *     and unwound when its actor is killed/destroyed (it does NOT block on a long sleep);
 *   - the normal (non-killed) path completes and ctx.push works;
 *   - spawn_detached stays detached ("orphan-and-complete") — unchanged semantics;
 *   - no frame leak (CoroutineFrameAllocator::live_frames returns to baseline);
 *   - ctx.cancellation_point() bails cooperatively.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <atomic>
#include <chrono>

using namespace qb;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Shared observation flags (reset at the start of each TEST).
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_scoped_completed_full{false}; // set if a long ctx.sleep ran to the end
std::atomic<bool> g_scoped_cancel_observed{false}; // set in the coroutine's catch on cancel
std::atomic<bool> g_raw_completed{false};          // set if a raw spawn_detached coroutine finished
std::atomic<int>  g_loop_iterations{0};            // cancellation_point loop progress

void
reset_flags() {
    g_scoped_completed_full = false;
    g_scoped_cancel_observed = false;
    g_raw_completed          = false;
    g_loop_iterations        = 0;
}
} // namespace

struct ScopeResultEvent : public qb::Event {
    int value{0};
    ScopeResultEvent() = default;
    explicit ScopeResultEvent(int v)
        : value(v) {}
};

// ---------------------------------------------------------------------------
// 1. Cancel-on-kill wakes a scoped coroutine parked on a long ctx.sleep.
// ---------------------------------------------------------------------------
class ScopedKillActor : public qb::Actor {
public:
    bool
    onInit() override {
        // Parked on a 2s cancellable sleep — must be cut short by kill, not block.
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await ctx.sleep(2s);
                g_scoped_completed_full = true; // MUST NOT run
            } catch (const qb::io::async::cancelled_error &) {
                g_scoped_cancel_observed = true; // SHOULD run
            }
        });
        // Kill shortly after, while the coroutine is parked on the 2s sleep.
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        return true;
    }
};

TEST(ActorCoroutineScope, CancelOnKillWakesScopedCoroutine) {
    reset_flags();
    qb::Main main;
    main.addActor<ScopedKillActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_scoped_completed_full.load()) << "the 2s sleep should have been cancelled";
    EXPECT_TRUE(g_scoped_cancel_observed.load()) << "cancellation must be observed on kill";
}

// ---------------------------------------------------------------------------
// 2. Happy path: a scoped coroutine completes normally and ctx.push works.
// ---------------------------------------------------------------------------
class ScopedNormalActor : public qb::Actor {
    bool got_{false};

public:
    bool
    onInit() override {
        registerEvent<ScopeResultEvent>(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(10ms);          // completes normally (not cancelled)
            ctx.push<ScopeResultEvent>(123);   // safe push back to self
        });
        return true;
    }

    void
    on(const ScopeResultEvent &ev) {
        EXPECT_EQ(ev.value, 123);
        got_ = true;
        // The coroutine has completed; no scoped coroutine should remain.
        EXPECT_EQ(active_coroutine_count(), 0u);
        kill();
    }

    ~ScopedNormalActor() override {
        EXPECT_TRUE(got_);
    }
};

TEST(ActorCoroutineScope, ScopedCoroutineNormalCompletion) {
    reset_flags();
    qb::Main main;
    main.addActor<ScopedNormalActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 3. Differentiator: on kill, a scoped coroutine is cancelled while a raw
//    spawn_detached coroutine remains detached and completes ("orphan-and-complete").
//    A keeper actor keeps the engine alive long enough to observe both.
// ---------------------------------------------------------------------------
class ScopedVsAsyncActor : public qb::Actor {
public:
    bool
    onInit() override {
        // Scoped: cancelled at kill (its 40ms sleep must NOT finish).
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(40ms);
            g_scoped_completed_full = true; // MUST NOT run (cancelled at ~10ms)
        });
        // Raw: detached — survives the actor and completes its 40ms sleep.
        spawn_detached([](auto) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(40ms);
            g_raw_completed = true; // SHOULD run (orphan-and-complete)
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            10ms);
        return true;
    }
};

class KeeperActor : public qb::Actor {
public:
    bool
    onInit() override {
        // Keep the engine alive past the 40ms coroutine deadlines, then stop.
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            150ms);
        return true;
    }
};

TEST(ActorCoroutineScope, ScopedCancelledButAsyncOrphanCompletes) {
    reset_flags();
    qb::Main main;
    main.addActor<ScopedVsAsyncActor>(0);
    main.addActor<KeeperActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_scoped_completed_full.load()) << "scoped coroutine must be cancelled on kill";
    EXPECT_TRUE(g_raw_completed.load()) << "spawn_detached must stay detached (orphan-and-complete)";
}

// ---------------------------------------------------------------------------
// 4. Stress: many scoped coroutines cancelled at once — no crash, no frame leak.
// ---------------------------------------------------------------------------
class ManyScopedActor : public qb::Actor {
public:
    static constexpr int N = 32;

    bool
    onInit() override {
        for (int i = 0; i < N; ++i) {
            // until_cancelled() parks with NO detached timer/helper, so reclamation on
            // kill is exact — every coroutine frame is reaped (validates the leak check).
            spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                co_await ctx.until_cancelled(); // parks until the actor is killed
                g_scoped_completed_full = true; // MUST NOT run
            });
        }
        EXPECT_EQ(active_coroutine_count(), static_cast<std::size_t>(N));
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        return true;
    }
};

TEST(ActorCoroutineScope, ManyScopedCoroutinesCancelledNoLeak) {
    reset_flags();
    const long baseline = qb::io::async::detail::CoroutineFrameAllocator::live_frames;
    {
        qb::Main main;
        main.addActor<ManyScopedActor>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_FALSE(g_scoped_completed_full.load());
    EXPECT_EQ(qb::io::async::detail::CoroutineFrameAllocator::live_frames, baseline)
        << "all scoped coroutine frames must be reclaimed (no leak)";
}

// ---------------------------------------------------------------------------
// 5. ctx.cancellation_point() bails cooperatively on kill.
// ---------------------------------------------------------------------------
class CancellationPointActor : public qb::Actor {
public:
    bool
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                for (;;) {
                    co_await ctx.cancellation_point(); // throws once the scope is cancelled
                    co_await ctx.sleep(2ms);
                    g_loop_iterations.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const qb::io::async::cancelled_error &) {
                g_scoped_cancel_observed = true;
            }
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            30ms);
        return true;
    }
};

TEST(ActorCoroutineScope, CancellationPointBailsOnKill) {
    reset_flags();
    qb::Main main;
    main.addActor<CancellationPointActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_scoped_cancel_observed.load()) << "cancellation_point must throw on kill";
    EXPECT_GT(g_loop_iterations.load(), 0) << "the loop should have made some progress before cancel";
}

// ---------------------------------------------------------------------------
// 6. ctx.cancellable(task) — an arbitrary task wrapped by the scope is cancelled.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_cancellable_done{false};
std::atomic<bool> g_cancellable_cancelled{false};

qb::io::async::task<int>
scope_slow_task() {
    co_await qb::io::async::sleep(2s);
    co_return 7;
}
} // namespace

class CancellableActor : public qb::Actor {
public:
    bool
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await ctx.cancellable(scope_slow_task()); // cancelled on kill
                g_cancellable_done = true;                   // MUST NOT run
            } catch (const qb::io::async::cancelled_error &) {
                g_cancellable_cancelled = true;
            }
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        return true;
    }
};

TEST(ActorCoroutineScope, CancellableWrapperCancelledOnKill) {
    g_cancellable_done      = false;
    g_cancellable_cancelled = false;
    qb::Main main;
    main.addActor<CancellableActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_cancellable_done.load());
    EXPECT_TRUE(g_cancellable_cancelled.load());
}

// ---------------------------------------------------------------------------
// 7. child_token() is cancelled when the actor scope is cancelled.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_child_cancelled{false};
} // namespace

class ChildTokenActor : public qb::Actor {
public:
    bool
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto child = ctx.child_token(); // linked to the actor scope
            try {
                co_await qb::io::async::cancellable_sleep(2s, child);
            } catch (const qb::io::async::cancelled_error &) {
                g_child_cancelled = true;
            }
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        return true;
    }
};

TEST(ActorCoroutineScope, ChildTokenCancelledWithParentScope) {
    g_child_cancelled = false;
    qb::Main main;
    main.addActor<ChildTokenActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_child_cancelled.load());
}

// ---------------------------------------------------------------------------
// 8. Lazy scope: has_coro_scope() is false until the first spawn; an
//    actor that only uses spawn_detached (never allocates a scope) kills cleanly.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_has_scope{-1}; // 1 == lazy semantics held
} // namespace

class ScopeFlagActor : public qb::Actor {
public:
    bool
    onInit() override {
        const bool before = has_coro_scope(); // false
        spawn_detached([](auto) -> qb::io::async::task<void> { co_await qb::io::async::sleep(5ms); });
        const bool after_async = has_coro_scope(); // still false (spawn_detached never allocates a scope)
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await ctx.sleep(5ms); });
        const bool after_scoped = has_coro_scope(); // true
        g_has_scope             = (!before && !after_async && after_scoped) ? 1 : 0;
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        return true;
    }
};

TEST(ActorCoroutineScope, HasCoroScopeReflectsLazyAllocation) {
    g_has_scope = -1;
    qb::Main main;
    main.addActor<ScopeFlagActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_has_scope.load(), 1);
}

// ---------------------------------------------------------------------------
// 9. A scoped coroutine with several sequential awaits is cancelled mid-sequence.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int>  g_steps{0};
std::atomic<bool> g_seq_cancelled{false};
} // namespace

class MultiStepActor : public qb::Actor {
public:
    bool
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await ctx.sleep(10ms);
                g_steps.fetch_add(1); // step 1 — completes before kill
                co_await ctx.sleep(200ms);
                g_steps.fetch_add(1); // killed during this await — never reached
                co_await ctx.sleep(10ms);
                g_steps.fetch_add(1);
            } catch (const qb::io::async::cancelled_error &) {
                g_seq_cancelled = true;
            }
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            50ms); // fires during the 200ms await
        return true;
    }
};

TEST(ActorCoroutineScope, MultiStepCancelledMidSequence) {
    g_steps        = 0;
    g_seq_cancelled = false;
    qb::Main main;
    main.addActor<MultiStepActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_steps.load(), 1) << "only the first await completed before kill";
    EXPECT_TRUE(g_seq_cancelled.load());
}

// ---------------------------------------------------------------------------
// 10. active_coroutine_count() counts scoped coroutines.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_count_at_spawn{-1};
} // namespace

class CountActor : public qb::Actor {
public:
    bool
    onInit() override {
        for (int i = 0; i < 4; ++i)
            spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await ctx.until_cancelled(); });
        g_count_at_spawn = static_cast<int>(active_coroutine_count());
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        return true;
    }
};

TEST(ActorCoroutineScope, ActiveCountCountsScopedCoroutines) {
    g_count_at_spawn = -1;
    qb::Main main;
    main.addActor<CountActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_count_at_spawn.load(), 4);
}
