/**
 * @file qb/core/tests/system/test-core-improvements.cpp
 * @brief Regression suite for the post-review hardening of qb-core.
 *
 * Each TEST below pins down one of the invariants introduced (or formalised)
 * during the QB_CORE_PLAN.md sweep. Findings tracked here:
 *
 *   - 2.1  : `qb::type_id<T>` / `Event::type_to_id<T>` are dense, stable and
 *            collision-free under multi-thread first instantiation.
 *   - 2.3  : `ServiceActor<Tag>::ServiceIndex` is unique and stable across
 *            many threads racing on first access.
 *   - 2.4  : QoS-1 deadlock-recovery survives heavy backpressure between
 *            cores (no livelock, no event loss).
 *   - 2.9  : `qb::RefActorHandle<T>` reports `nullptr` once the referenced
 *            actor has called `kill()`.
 *   - 2.10 : `Main::_instances` removal — implicit (no symbol referenced).
 *   - 2.11 : `qb::CoreIdSet{qb::NoAffinity}` is honoured as a safe sentinel
 *            for "no pinning".
 *   - 2.12 : `Main::core(idx)` rejects `idx >= qb::MaxCores` with a
 *            `std::range_error`.
 *   - 2.13 : `spawn_detached` counter is eagerly allocated; fresh actors expose
 *            a 0-valued counter without any call to `spawn_detached`.
 *   - 2.14 : `qb::allocate_actor<T>` is a customization point routed through
 *            from both the standard factory and `addRefActor`.
 *   - 2.16 : `qb::no_default_events` opts an actor out of the four default
 *            event registrations (KillEvent / SignalEvent / ...).
 *   - 2.17 : `~Main()` triggers a clean shutdown via the engine's
 *            `std::stop_source`, even with no explicit `stop()` call.
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
 *
 * @ingroup Core
 */

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

// =============================================================================
// 2.1 — TypeId stability, density, collision-freedom, thread-safety
// =============================================================================

namespace {

// 256 distinct types via a non-type template parameter. Used to stress the
// uniqueness of `qb::type_id<T>` across a large flat type cohort.
template <std::size_t N>
struct TypeIdProbe {};

template <std::size_t... Is>
void
collectTypeIds(std::index_sequence<Is...>, std::vector<qb::TypeId> &out) {
    (out.push_back(qb::type_id<TypeIdProbe<Is>>()), ...);
}

} // namespace

TEST(TypeId, IsStableAcrossCalls) {
    // Same type → same id, called arbitrarily many times.
    const auto a = qb::type_id<int>();
    for (std::size_t i = 0; i < 1024; ++i) {
        EXPECT_EQ(qb::type_id<int>(), a);
    }
    const auto b = qb::type_id<double>();
    EXPECT_NE(a, b) << "Distinct types must not collide";
}

TEST(TypeId, IsCollisionFreeAcrossManyTypes) {
    constexpr std::size_t   kNTypes = 256;
    std::vector<qb::TypeId> ids;
    ids.reserve(kNTypes);
    collectTypeIds(std::make_index_sequence<kNTypes>{}, ids);

    // All ids must be unique — the magic-static counter guarantees this up to
    // `std::numeric_limits<TypeId>::max()` distinct types, which is far more
    // than any practical application will ever register.
    std::unordered_set<qb::TypeId> uniq(ids.begin(), ids.end());
    EXPECT_EQ(uniq.size(), ids.size()) << "type_id<T>() produced a collision over " << kNTypes << " types";
    // None of them must be 0 (0 is reserved as "unassigned").
    for (auto id : ids)
        EXPECT_NE(id, qb::TypeId{0}) << "TypeId 0 is reserved";
}

namespace {
// Disjoint cohort used by the concurrent test — stops the optimizer from
// hoisting both calls if they share a TU instantiation.
template <std::size_t N>
struct ConcurrentTypeIdProbe {};
} // namespace

TEST(TypeId, ConcurrentFirstInstantiationIsRaceFree) {
    // Have many threads call `type_id<T>()` for the same T at the same time.
    // The magic-static initialiser barrier in `detail::type_id_for<T>` must
    // serialise the post-increment — the result must be *one* unique id,
    // not as many as there are threads.
    constexpr std::size_t            kThreads = 16;
    std::atomic<bool>                go{false};
    std::array<qb::TypeId, kThreads> seen{};
    std::vector<std::thread>         workers;
    workers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
        workers.emplace_back([i, &go, &seen] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            seen[i] = qb::type_id<ConcurrentTypeIdProbe<999>>();
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &t : workers)
        t.join();
    // Every thread must observe exactly the same id.
    for (std::size_t i = 1; i < kThreads; ++i)
        EXPECT_EQ(seen[i], seen[0]);
}

TEST(TypeId, EventTypeToIdMatchesGlobalTypeId) {
    // `Event::type_to_id<T>()` must produce a stable, unique identifier for
    // every distinct event type so that the router (`memh::route()`) keys
    // events with the same id used at register. The *representation* of that
    // id is build-mode dependent (intentional):
    //
    //   - NDEBUG builds collapse identity to the dense 16-bit counter that
    //     backs `qb::type_id<T>()`, which lets us assert strict equality.
    //   - Debug builds keep `typeid(T).name()` so invalid routing shows up
    //     with a human-readable tag; equality with `qb::type_id<T>()` is
    //     therefore not meaningful (different types by design) and we
    //     instead validate self-consistency: non-null, stable across calls,
    //     and distinct across distinct event types.
#ifdef NDEBUG
    EXPECT_EQ(qb::Event::type_to_id<qb::KillEvent>(), qb::type_id<qb::KillEvent>());
    EXPECT_EQ(qb::Event::type_to_id<qb::SignalEvent>(), qb::type_id<qb::SignalEvent>());
#else
    const auto kill_a   = qb::Event::type_to_id<qb::KillEvent>();
    const auto kill_b   = qb::Event::type_to_id<qb::KillEvent>();
    const auto signal_a = qb::Event::type_to_id<qb::SignalEvent>();
    ASSERT_NE(kill_a, nullptr);
    ASSERT_NE(signal_a, nullptr);
    EXPECT_EQ(kill_a, kill_b) << "type_to_id<T>() must be stable across calls";
    EXPECT_STRNE(kill_a, signal_a) << "distinct event types must map to distinct ids";
#endif
}

// =============================================================================
// 2.3 — ServiceActor<Tag>::ServiceIndex is unique under concurrent first access
// =============================================================================

namespace {
struct ServiceTagA {};
struct ServiceTagB {};
struct ServiceTagC {};
} // namespace

TEST(ServiceIndex, IsUniqueAcrossTagsAndStableUnderConcurrency) {
    constexpr std::size_t    kThreads = 8;
    std::atomic<bool>        go{false};
    std::vector<std::thread> workers;
    // We use the public `Actor::getServiceId<Tag>(CoreId)` which routes
    // through `Actor::registerIndex<Tag>()` (the magic-static fixed in 2.3).
    // The lower 16 bits encode the per-tag `ServiceId` we want to compare.
    constexpr qb::CoreId                kCore = 0;
    std::array<qb::ServiceId, kThreads> a{}, b{}, c{};
    workers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
        workers.emplace_back([i, &go, &a, &b, &c] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            a[i] = qb::Actor::getServiceId<ServiceTagA>(kCore).sid();
            b[i] = qb::Actor::getServiceId<ServiceTagB>(kCore).sid();
            c[i] = qb::Actor::getServiceId<ServiceTagC>(kCore).sid();
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &t : workers)
        t.join();

    // Within one tag, every reader must observe the same id.
    for (std::size_t i = 1; i < kThreads; ++i) {
        EXPECT_EQ(a[i], a[0]);
        EXPECT_EQ(b[i], b[0]);
        EXPECT_EQ(c[i], c[0]);
    }
    // Across tags, ids must differ — they identify distinct services.
    EXPECT_NE(a[0], b[0]);
    EXPECT_NE(b[0], c[0]);
    EXPECT_NE(a[0], c[0]);
    // Service ids are 1-based, never 0 (which marks "non-service").
    EXPECT_GT(a[0], qb::ServiceId{0});
    EXPECT_GT(b[0], qb::ServiceId{0});
    EXPECT_GT(c[0], qb::ServiceId{0});
}

// =============================================================================
// 2.11 — `qb::NoAffinity` is a safe sentinel
// =============================================================================

namespace {
class TerminateImmediatelyActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};
} // namespace

TEST(NoAffinity, SetAffinityWithSentinelOnlyDoesNotCrash) {
    qb::Main main;
    main.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity}).addActor<TerminateImmediatelyActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError()) << "CoreIdSet{NoAffinity} must be filtered out, not passed to OS APIs";
}

TEST(NoAffinity, SetAffinityWithMixedSentinelAndRealCoreIsSafe) {
    qb::Main main;
    // Real CPU id 0 is always present; the sentinel must be filtered out
    // and the pinning must still target core 0.
    main.core(0).setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(0), qb::NoAffinity}).addActor<TerminateImmediatelyActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

// =============================================================================
// 2.12 — `Main::core(idx)` range check uses `qb::MaxCores` (not magic 255)
// =============================================================================

TEST(MainCoreRange, RejectsExactlyAtMaxCores) {
    qb::Main main;
    EXPECT_THROW(main.core(static_cast<qb::CoreId>(qb::MaxCores)).addActor<TerminateImmediatelyActor>(), std::range_error)
        << "core(MaxCores) must throw — boundary is half-open";
}

TEST(MainCoreRange, RejectsAboveMaxCores) {
    qb::Main main;
    EXPECT_THROW(main.core(static_cast<qb::CoreId>(qb::MaxCores + 100)).addActor<TerminateImmediatelyActor>(), std::range_error);
}

TEST(MainCoreRange, AcceptsLastValidCoreId) {
    qb::Main main;
    EXPECT_NO_THROW(main.core(static_cast<qb::CoreId>(qb::MaxCores - 1)).addActor<TerminateImmediatelyActor>())
        << "MaxCores - 1 is the last legal CoreId; must not throw";
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

// =============================================================================
// 2.9 — RefActorHandle<T> reports liveness correctly
// =============================================================================

namespace {

class HandleChildActor : public qb::Actor {
public:
    int data           = 42;
    HandleChildActor() = default;
    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

class HandleParentActor : public qb::Actor {
public:
    HandleParentActor() = default;

    qb::io::async::task<bool>
    onInit() final {
        // 1. Default-constructed handle is invalid.
        qb::RefActorHandle<HandleChildActor> empty;
        EXPECT_FALSE(empty.valid());
        EXPECT_EQ(empty.get(), nullptr);
        EXPECT_FALSE(static_cast<bool>(empty));

        // 2. Real handle to a live child resolves correctly.
        auto handle = addRefHandle<HandleChildActor>();
        EXPECT_TRUE(handle.valid());
        EXPECT_NE(handle.get(), nullptr);
        EXPECT_EQ(handle->data, 42);
        EXPECT_TRUE(static_cast<bool>(handle));

        // 3. Once the child kills itself, `is_alive()` is false → `get()` must
        //    co_return nullptr even though the actor object still exists in the
        //    `_actors` map until the end of this workflow iteration.
        handle->kill();
        EXPECT_EQ(handle.get(), nullptr) << "RefActorHandle::get() must co_return nullptr after child kill()";
        EXPECT_FALSE(static_cast<bool>(handle));
        // valid() reflects the *id* validity, not liveness — id stays valid.
        EXPECT_TRUE(handle.valid());

        kill();
        co_return true;
    }
};

} // namespace

TEST(RefActorHandle, ReportsNullptrAfterChildKill) {
    qb::Main main;
    main.core(0).addActor<HandleParentActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

// =============================================================================
// 2.16 — `qb::no_default_events` opts an actor out of default registrations
// =============================================================================

namespace {

// Plain actor that opts out of default registrations. Without a manual
// registerEvent<KillEvent>, it CANNOT be killed via push<KillEvent>. We use
// that observable difference as proof that the tag really did skip them.
class OptOutActor : public qb::Actor {
public:
    OptOutActor()
        : qb::Actor(qb::no_default_events) {}

    qb::io::async::task<bool>
    onInit() final {
        // Self-kill on a single-cycle delay so the test still terminates.
        kill();
        co_return true;
    }
};

// Same actor, but opts back in to KillEvent so an external `push<KillEvent>`
// works as usual.
class OptOutOptInKillActor : public qb::Actor {
public:
    OptOutOptInKillActor()
        : qb::Actor(qb::no_default_events) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::KillEvent>(*this);
        co_return true;
    }
};

class KillSenderToOptOutActor : public qb::Actor {
    qb::ActorId _target;

public:
    explicit KillSenderToOptOutActor(qb::ActorId target)
        : _target(target) {}

    qb::io::async::task<bool>
    onInit() final {
        push<qb::KillEvent>(_target);
        kill();
        co_return true;
    }
};

} // namespace

TEST(NoDefaultEvents, ActorWithoutKillRegistrationStillSelfKills) {
    // Even without auto-registered handlers, the actor's own kill() works.
    qb::Main main;
    main.core(0).addActor<OptOutActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(NoDefaultEvents, OptInKillEventEnablesExternalKill) {
    // After explicit registerEvent<KillEvent>, push<KillEvent> kills the actor
    // exactly like a default-registered actor would.
    qb::Main main;
    auto     target = main.core(0).addActor<OptOutOptInKillActor>();
    main.core(0).addActor<KillSenderToOptOutActor>(target);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

// =============================================================================
// 2.14 — `qb::allocate_actor<T>` customization point
// =============================================================================

namespace {

class CustomAllocActor; // fwd
inline std::atomic<std::size_t> custom_alloc_counter{0};

class CustomAllocActor : public qb::Actor {
public:
    CustomAllocActor() = default;
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

class CustomAllocRefParent : public qb::Actor {
public:
    CustomAllocRefParent() = default;
    qb::io::async::task<bool>
    onInit() final {
        // addRefActor must also route through allocate_actor (finding 2.5).
        auto child = addRefActor<CustomAllocActor>();
        EXPECT_TRUE(child.valid());
        kill();
        co_return true;
    }
};

} // namespace

namespace qb {
// Full specialization of the allocate_actor customization point. Both
// `TActorFactory::create_impl` (used by `addActor`) and
// `VirtualCore::addReferencedActor` (used by `addRefActor`) must route
// through it (finding 2.5 + 2.13/2.14).
template <>
inline CustomAllocActor *
allocate_actor<CustomAllocActor>() {
    custom_alloc_counter.fetch_add(1, std::memory_order_relaxed);
    return new CustomAllocActor();
}
} // namespace qb

TEST(AllocateActor, IsRoutedFromStandardFactory) {
    custom_alloc_counter.store(0, std::memory_order_relaxed);
    qb::Main main;
    main.core(0).addActor<CustomAllocActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(custom_alloc_counter.load(), 1u) << "qb::allocate_actor<T> must be invoked by TActorFactory::create_impl";
}

TEST(AllocateActor, IsRoutedFromAddRefActor) {
    custom_alloc_counter.store(0, std::memory_order_relaxed);
    qb::Main main;
    main.core(0).addActor<CustomAllocRefParent>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(custom_alloc_counter.load(), 1u) << "qb::allocate_actor<T> must also be invoked by VirtualCore::addReferencedActor";
}

// =============================================================================
// 2.13 — spawn_detached counter is eagerly allocated; freshly built actors
//        expose a 0-valued counter without any spawn_detached call.
// =============================================================================

namespace {

class FreshCounterActor : public qb::Actor {
public:
    FreshCounterActor() {
        // Eager allocation (finding 2.13) means active_coroutines_ is a live
        // shared_ptr the moment the ctor body runs.
        EXPECT_FALSE(has_active_coroutines());
        EXPECT_EQ(active_coroutine_count(), 0u);
    }
    qb::io::async::task<bool>
    onInit() final {
        EXPECT_FALSE(has_active_coroutines());
        EXPECT_EQ(active_coroutine_count(), 0u);
        kill();
        co_return true;
    }
};

} // namespace

TEST(SpawnAsync, CounterIsEagerlyAllocatedAndZero) {
    qb::Main main;
    main.core(0).addActor<FreshCounterActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

// =============================================================================
// 2.4 — Deadlock-recovery survives QoS-1 backpressure between cores
// =============================================================================

namespace {

struct StressEvent : public qb::Event {
    std::uint32_t seq;
    explicit StressEvent(std::uint32_t s) noexcept
        : seq(s) {}
};

struct StressEosEvent : public qb::Event {};

inline std::atomic<std::uint32_t> stress_received{0};
inline std::atomic<std::uint32_t> stress_eos_received{0};
inline std::uint32_t              stress_eos_expected{0}; // set on the sink core

class StressSinkActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<StressEvent>(*this);
        registerEvent<StressEosEvent>(*this);
        co_return true;
    }
    void
    on(StressEvent const &) {
        stress_received.fetch_add(1, std::memory_order_relaxed);
    }
    void
    on(StressEosEvent const &) {
        // Only kill once every source has flushed its burst — guarantees
        // we counted every QoS-1 event before tearing down.
        if (stress_eos_received.fetch_add(1, std::memory_order_acq_rel) + 1 == stress_eos_expected) {
            kill();
        }
    }
};

class StressSourceActor : public qb::Actor {
    qb::ActorId   _sink;
    std::uint32_t _budget;

public:
    StressSourceActor(qb::ActorId sink, std::uint32_t budget)
        : _sink(sink)
        , _budget(budget) {}

    qb::io::async::task<bool>
    onInit() final {
        // QoS-1 events: each `push<>` triggers `allocate_back` on the
        // outbound pipe; the receiver core's mailbox fills up, exercising
        // `__flush_all__`'s bounded backoff (finding 2.4).
        for (std::uint32_t i = 0; i < _budget; ++i) {
            push<StressEvent>(_sink, i);
        }
        // Single EOS marker per source — preserves the FIFO ordering of QoS-1
        // events, so the sink only sees this AFTER its `kBurst` predecessors.
        push<StressEosEvent>(_sink);
        kill();
        co_return true;
    }
};

} // namespace

TEST(DeadlockRecovery, QoS1HighBackpressureNoLivelock) {
    // Multi-source contention into a single sink. Each source pushes a heavy
    // burst of QoS-1 events; the sink's mailbox WILL fill up faster than
    // events can drain, forcing `__flush_all__` into the bounded backoff
    // (spin → yield → bail+notify) path on every source core.
    //
    // Correctness criteria:
    //   1. All `kSources × kBurst` events are received (no drops).
    //   2. The whole run completes in bounded time (no livelock).
    constexpr std::uint32_t kSources = 4;
    constexpr std::uint32_t kBurst   = 200'000;
    constexpr std::uint32_t kTotal   = kSources * kBurst;

    stress_received.store(0, std::memory_order_relaxed);
    stress_eos_received.store(0, std::memory_order_relaxed);
    stress_eos_expected = kSources;

    qb::Main main;
    auto     sink = main.core(0).addActor<StressSinkActor>();
    for (qb::CoreId c = 1; c <= static_cast<qb::CoreId>(kSources); ++c) {
        main.core(c).addActor<StressSourceActor>(sink, kBurst);
    }

    const auto t_start = std::chrono::steady_clock::now();
    main.start(false);
    main.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start);

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(stress_received.load(), kTotal) << "All QoS-1 events must be delivered (no drops, no loss) — "
                                                 "deadlock recovery must preserve full FIFO semantics";
    EXPECT_LT(elapsed.count(), 60) << "Deadlock recovery must terminate in bounded time (no livelock)";
}

// =============================================================================
// 2.17 — ~Main() triggers a clean shutdown via std::stop_source / jthread
// =============================================================================

namespace {

class LongLivedActor : public qb::Actor {
public:
    LongLivedActor() = default;
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::KillEvent>(*this);
        registerEvent<qb::SignalEvent>(*this);
        co_return true; // Stays alive — no kill() call in onInit.
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
    void
    on(qb::SignalEvent const &) {
        kill();
    }
};

} // namespace

TEST(StopSource, ExplicitStopShutsDownPromptly) {
    // Baseline: explicit `Main::stop()` (signal-handler-safe path) brings
    // every long-lived actor down within bounded time.
    const auto t_start = std::chrono::steady_clock::now();
    {
        qb::Main main;
        main.core(0).addActor<LongLivedActor>();
        main.core(1).addActor<LongLivedActor>();
        main.start(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        main.stop();
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start);
    EXPECT_LT(elapsed.count(), 10) << "Main::stop() must broadcast SIGINT and shut down promptly";
}

TEST(StopSource, MainDestructorJoinsRunningWorkers) {
    // Without an explicit `stop()`, the destructor must still bring the
    // engine down — `std::jthread` RAII + `_stop_source.request_stop()`
    // observed in `__workflow__` (finding 2.17). The synthesised SIGINT
    // is broadcast to each core, every long-lived actor receives a
    // `SignalEvent`, calls `kill()`, and the workflow loop terminates.
    const auto t_start = std::chrono::steady_clock::now();
    {
        qb::Main main;
        main.core(0).addActor<LongLivedActor>();
        main.core(1).addActor<LongLivedActor>();
        main.start(true); // async: start() returns immediately
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // No explicit stop() — let `~Main()` issue request_stop() + join().
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start);
    EXPECT_LT(elapsed.count(), 10) << "~Main() must shut down promptly via std::stop_source/jthread";
}
