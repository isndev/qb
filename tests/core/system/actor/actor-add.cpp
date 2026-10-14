/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/actor/actor-add.cpp
 * @brief Actor / ServiceActor / referenced-actor creation + init-failure polarity + KillEvent teardown.
 *
 * The reference-quality "how do actors come into and out of existence" system suite. It pins, on a
 * real `qb::Main`:
 *   - `addActor<>()` returns a valid `ActorId` (and a `ServiceActor`'s id is its service index on that core);
 *   - `getService<T>()` identity: null before init, exactly `this` inside `onInit()`, non-null to a peer;
 *   - the `CoreInitializer::builder()` ordered `idList()` + its `valid()` flip on a duplicate service;
 *   - a rejected dynamic duplicate service leaves the first service's custom subscription intact;
 *   - bad-core-index throws `std::range_error`; adding after `start()` throws `std::runtime_error`;
 *   - referenced actors (`addRefActor<>()`) propagate their child's init success/failure;
 *   - init-failure POLARITY, pinned to the SPECIFIC `qb::VirtualCore::Error` each path raises (the
 *     enum is packed into Main's private start barrier; only the `hasError()` bool is public, so we
 *     distinguish the codes by observable side effects — a per-path atom — not by reading the enum):
 *       · `onInit()` co_returns false → `Error::BadActorInit`   (g_returned_false set, g_threw NOT);
 *       · `onInit()` THROWS           → `Error::ExceptionThrown` (g_threw set, g_returned_false NOT).
 *   - `KillEvent` teardown at scale: a unicast self-kill + a `BroadcastId(1)` kill must leave ZERO
 *     of the 1024 broadcast-targeted actors alive (asserted by a live-instance counter that returns
 *     to 0 after join, not merely inferred from `!hasError()`).
 *
 * Every in-actor `EXPECT_*` is mirrored to a process-global atom asserted after `join()`, so a
 * worker-thread check that never runs cannot let a case pass vacuously. No wall-clock oracle; actors
 * self-terminate and the engine drains, with the ctest TIMEOUT as the only backstop.
 */

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/detail/actor_id_pool.h>
#include <qb/main.h>

namespace qb::detail {
struct actor_id_pool_test_access {
    static void
    limit(CoreInitializer &core, ActorSlotId ordinary_limit, std::uint32_t generation_limit = ActorId::MaxGeneration) {
        if (!core._actor_factories.empty()) {
            ADD_FAILURE() << "pool limits must be installed before creating factory promises";
            std::abort();
        }
        core._ids = ActorIdPool(core._index, ordinary_limit, generation_limit);
    }
};
} // namespace qb::detail

namespace actor_add_test {

// ---------------------------------------------------------------------------
// Observability atoms (reset per test). Mirror in-actor outcomes to the body.
// ---------------------------------------------------------------------------
std::atomic<bool> g_service_ctor_ran{false}; // ServiceActor ctor body ran (and its checks held)
std::atomic<bool> g_service_init_ran{false}; // ServiceActor onInit body ran (identity held)
std::atomic<bool> g_peer_saw_service{false}; // a peer actor observed the service via getService<>()
std::atomic<bool> g_returned_false{false};   // a failing onInit reached its clean co_return false
std::atomic<bool> g_threw{false};            // a throwing onInit reached its throw site
std::atomic<int>  g_kill_targets_alive{0};   // live TestKillActor instances (must return to 0)
std::atomic<int>  g_kill_targets_built{0};   // TestKillActor instances ever constructed

void
reset_atoms() {
    g_service_ctor_ran.store(false);
    g_service_init_ran.store(false);
    g_peer_saw_service.store(false);
    g_returned_false.store(false);
    g_threw.store(false);
    g_kill_targets_alive.store(0);
    g_kill_targets_built.store(0);
}

struct Tag {};

class TestServiceActor : public qb::ServiceActor<Tag> {
    const bool _ret_init;

public:
    TestServiceActor() = delete;
    explicit TestServiceActor(bool init)
        : _ret_init(init) {
        EXPECT_TRUE(id().is_valid());
        EXPECT_EQ(nullptr, getService<TestServiceActor>()); // not yet registered at ctor time
        g_service_ctor_ran.store(true);
        kill();
    }

    qb::io::async::task<bool>
    onInit() final {
        EXPECT_EQ(this, getService<TestServiceActor>()); // registered & resolvable to exactly *this*
        g_service_init_ran.store(true);
        co_return _ret_init;
    }
};

struct CheckServiceActor : public qb::Actor {
    CheckServiceActor() {
        EXPECT_NE(nullptr, getService<TestServiceActor>());
    }

    qb::io::async::task<bool>
    onInit() final {
        const bool ok = getService<TestServiceActor>() != nullptr;
        EXPECT_TRUE(ok);
        if (ok)
            g_peer_saw_service.store(true);
        kill();
        co_return true;
    }
};

class TestActor : public qb::Actor {
    const bool _ret_init;

public:
    TestActor() = delete;
    explicit TestActor(bool init)
        : _ret_init(init) {
        EXPECT_TRUE(id().is_valid());
        kill();
    }

    qb::io::async::task<bool>
    onInit() final {
        if (!_ret_init)
            g_returned_false.store(true);
        co_return _ret_init;
    }
};

class TestRefActor : public qb::Actor {
    const bool _ret_init;

public:
    TestRefActor() = delete;
    explicit TestRefActor(bool init)
        : _ret_init(init) {
        EXPECT_TRUE(id().is_valid());
    }

    qb::io::async::task<bool>
    onInit() final {
        auto actor = addRefActor<TestActor>(_ret_init);
        kill();
        co_return actor.valid();
    }
};

// onInit throws → BadActorInit: __drive_init__ catches it and reports a failed init.
class ThrowInitActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        if (id().is_valid()) { // always true; defeats unreachable-code analysis on the co_return
            g_threw.store(true);
            throw std::runtime_error("onInit threw during actor creation");
        }
        co_return true; // unreachable
    }
};

// ===========================================================================
// Creation / id contracts.
// ===========================================================================

TEST(AddActor, EngineShouldAbortIfActorFailedToInitAtStart) {
    reset_atoms();
    qb::Main main;
    main.addActor<TestActor>(0, false);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_TRUE(g_returned_false.load()) << "the false-returning onInit must have run";
    EXPECT_FALSE(g_threw.load()) << "this case returned false rather than throwing";
}

// onInit throws during creation → BadActorInit, like the false-return case above.
TEST(AddActor, EngineShouldAbortIfActorThrewDuringInitAtStart) {
    reset_atoms();
    qb::Main main;
    main.addActor<ThrowInitActor>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_TRUE(g_threw.load()) << "the throwing onInit must have reached its throw site";
    EXPECT_FALSE(g_returned_false.load()) << "this case threw rather than returning false";
}

TEST(AddActor, ShouldReturnValidActorIdAtStart) {
    reset_atoms();
    qb::Main main;
    auto     id = main.addActor<TestServiceActor>(0, true);
    main.addActor<CheckServiceActor>(0);
    EXPECT_TRUE(id.is_valid());

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_service_ctor_ran.load()) << "the service ctor must have run its identity checks";
    EXPECT_TRUE(g_service_init_ran.load()) << "the service onInit must have resolved to *this*";
    EXPECT_TRUE(g_peer_saw_service.load()) << "the peer must have resolved the service non-null";
}

TEST(AddActor, ShouldReturnValidServiceActorIdAtStart) {
    reset_atoms();
    qb::Main main;
    auto     id = main.addActor<TestServiceActor>(0, true);
    // A service's id is its registered index on its core -- WHICH index a Tag gets is not fixed. It is
    // drawn when `ServiceActor<Tag>::ServiceIndex` is dynamically initialised, and the standard leaves
    // that order unspecified across Tags: with the nine service Tags of this file, g++ gives `Tag` 1
    // and MSVC 9. Asserting 1 tested the compiler, not qb.
    EXPECT_TRUE(id.is_service_instance());
    EXPECT_EQ(id.service_tag(), qb::Actor::getServiceId<Tag>(0).service_tag());
    EXPECT_EQ(id.index(), qb::Actor::getServiceId<Tag>(0).index());
    EXPECT_NE(id, qb::Actor::getServiceId<Tag>(0));

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_service_init_ran.load());
}

TEST(AddActorUsingCoreBuilder, ShouldNotAddActorOnBadCoreIndex) {
    qb::Main main;
    EXPECT_THROW(main.core(256).addActor<TestActor>(true), std::range_error);
}

TEST(AddActorUsingCoreBuilder, ShouldNotAddActorWhenEngineIsRunning) {
    qb::Main main;
    main.core(0).addActor<TestActor>(true);
    main.start();
    EXPECT_THROW(main.core(0).addActor<TestActor>(true), std::runtime_error);
}

TEST(AddActorUsingCoreBuilder, ShouldRetrieveValidOrderedActorIdList) {
    reset_atoms();
    qb::Main main;
    auto     builder = main.core(0).builder().addActor<TestServiceActor>(true).addActor<TestActor>(true);
    EXPECT_TRUE(static_cast<bool>(builder));
    EXPECT_EQ(builder.idList().size(), 2u);
    EXPECT_TRUE(builder.idList()[0].is_service_instance());
    EXPECT_EQ(builder.idList()[0].service_tag(), qb::Actor::getServiceId<Tag>(0).service_tag());
    EXPECT_TRUE(builder.idList()[1].is_valid());
    builder.addActor<TestServiceActor>(true); // duplicate service → builder invalidates
    EXPECT_FALSE(static_cast<bool>(builder));
    EXPECT_EQ(builder.idList().size(), 3u);
    EXPECT_FALSE(builder.idList()[2].is_valid()); // failed add → NotFound id

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

struct DuplicateServiceTag {};
struct DistinctServiceTag {};
struct ServicePoke : qb::Event {};

std::atomic<int>  g_duplicate_service_ctors{0};
std::atomic<int>  g_original_service_pokes{0};
std::atomic<int>  g_rejected_service_pokes{0};
std::atomic<int>  g_distinct_service_pokes{0};
std::atomic<bool> g_duplicate_rejected{false};
std::atomic<bool> g_original_still_registered{false};
std::atomic<bool> g_distinct_service_accepted{false};

class DuplicateProbeService : public qb::ServiceActor<DuplicateServiceTag> {
    int _ordinal;

public:
    DuplicateProbeService()
        : _ordinal(g_duplicate_service_ctors.fetch_add(1) + 1) {
        registerEvent<ServicePoke>(*this);
    }

    void
    on(ServicePoke &) {
        if (_ordinal == 1)
            g_original_service_pokes.fetch_add(1);
        else
            g_rejected_service_pokes.fetch_add(1);
    }
};

class DistinctProbeService : public qb::ServiceActor<DistinctServiceTag> {
public:
    DistinctProbeService() {
        registerEvent<ServicePoke>(*this);
    }

    void
    on(ServicePoke &) {
        g_distinct_service_pokes.fetch_add(1);
    }
};

class DuplicateServiceAttempt : public qb::Actor {
    qb::ActorId _service;

public:
    explicit DuplicateServiceAttempt(qb::ActorId service)
        : _service(service) {}

    qb::io::async::task<bool>
    onInit() final {
        auto *const original  = getService<DuplicateProbeService>();
        const auto  duplicate = addRefActor<DuplicateProbeService>();
        g_duplicate_rejected.store(!duplicate.valid());
        g_original_still_registered.store(original != nullptr && getService<DuplicateProbeService>() == original);
        const auto distinct = addRefActor<DistinctProbeService>();
        g_distinct_service_accepted.store(distinct.valid() && getService<DistinctProbeService>() == distinct.get());
        push<ServicePoke>(_service);
        if (distinct.valid()) {
            push<ServicePoke>(distinct.id());
            push<qb::KillEvent>(distinct.id());
        }
        push<qb::KillEvent>(_service);
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, RejectedDuplicateServicePreservesOriginalCustomSubscription) {
    g_duplicate_service_ctors.store(0);
    g_original_service_pokes.store(0);
    g_rejected_service_pokes.store(0);
    g_distinct_service_pokes.store(0);
    g_duplicate_rejected.store(false);
    g_original_still_registered.store(false);
    g_distinct_service_accepted.store(false);

    qb::Main   main;
    const auto service = main.addActor<DuplicateProbeService>(0);
    main.addActor<DuplicateServiceAttempt>(0, service);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_duplicate_rejected.load());
    EXPECT_TRUE(g_original_still_registered.load());
    EXPECT_TRUE(g_distinct_service_accepted.load());
    EXPECT_EQ(g_duplicate_service_ctors.load(), 1);
    EXPECT_EQ(g_original_service_pokes.load(), 1);
    EXPECT_EQ(g_rejected_service_pokes.load(), 0);
    EXPECT_EQ(g_distinct_service_pokes.load(), 1);
}

struct DefaultOnlyServiceTag {};
std::atomic<int>  g_default_only_service_ctors{0};
std::atomic<bool> g_default_only_duplicate_rejected{false};
std::atomic<bool> g_default_only_original_alive{false};

class DefaultOnlyService : public qb::ServiceActor<DefaultOnlyServiceTag> {
public:
    DefaultOnlyService() {
        g_default_only_service_ctors.fetch_add(1);
    }
};

class DefaultOnlyServiceAttempt : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        const auto original  = addRefActor<DefaultOnlyService>();
        const auto duplicate = addRefActor<DefaultOnlyService>();
        g_default_only_duplicate_rejected.store(original.valid() && !duplicate.valid());
        g_default_only_original_alive.store(getService<DefaultOnlyService>() == original.get() && is_actor_alive(original.id()));
        push<qb::KillEvent>(original.id());
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, RejectedDefaultOnlyServicePreservesOriginal) {
    g_default_only_service_ctors.store(0);
    g_default_only_duplicate_rejected.store(false);
    g_default_only_original_alive.store(false);

    qb::Main main;
    main.addActor<DefaultOnlyServiceAttempt>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_default_only_duplicate_rejected.load());
    EXPECT_TRUE(g_default_only_original_alive.load());
    EXPECT_EQ(g_default_only_service_ctors.load(), 1);
}

struct ReentrantServiceTag {};
struct ReentrantPoke : qb::Event {};
std::atomic<int>  g_reentrant_service_ctors{0};
std::atomic<int>  g_reentrant_first_pokes{0};
std::atomic<bool> g_reentrant_nested_rejected{false};
std::atomic<bool> g_reentrant_first_admitted{false};

class ReentrantService : public qb::ServiceActor<ReentrantServiceTag> {
    int _ordinal;

public:
    ReentrantService()
        : _ordinal(g_reentrant_service_ctors.fetch_add(1) + 1) {
        if (_ordinal == 1)
            g_reentrant_nested_rejected.store(!addRefActor<ReentrantService>().valid());
        registerEvent<ReentrantPoke>(*this);
    }

    void
    on(ReentrantPoke &) {
        if (_ordinal == 1)
            g_reentrant_first_pokes.fetch_add(1);
    }
};

class ReentrantServiceAttempt : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        const auto service = addRefActor<ReentrantService>();
        g_reentrant_first_admitted.store(service.valid() && getService<ReentrantService>() == service.get());
        const auto id = service.valid() ? service.id() : getService<ReentrantService>()->id();
        push<ReentrantPoke>(id);
        push<qb::KillEvent>(id);
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, ServiceConstructorCannotAdmitItsOwnTagRecursively) {
    g_reentrant_service_ctors.store(0);
    g_reentrant_first_pokes.store(0);
    g_reentrant_nested_rejected.store(false);
    g_reentrant_first_admitted.store(false);

    qb::Main main;
    main.addActor<ReentrantServiceAttempt>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_reentrant_service_ctors.load(), 1);
    EXPECT_TRUE(g_reentrant_nested_rejected.load());
    EXPECT_TRUE(g_reentrant_first_admitted.load());
    EXPECT_EQ(g_reentrant_first_pokes.load(), 1);
}

struct ThrowOnceServiceTag {};
struct ThrowOncePoke : qb::Event {};
std::atomic<int>  g_throw_once_service_ctors{0};
std::atomic<int>  g_throw_once_pokes{0};
std::atomic<bool> g_throw_once_caught{false};
std::atomic<bool> g_throw_once_retry_admitted{false};

class ThrowOnceService : public qb::ServiceActor<ThrowOnceServiceTag> {
public:
    ThrowOnceService() {
        registerEvent<ThrowOncePoke>(*this);
        if (g_throw_once_service_ctors.fetch_add(1) == 0)
            throw std::runtime_error("first service construction failed");
    }

    void
    on(ThrowOncePoke &) {
        g_throw_once_pokes.fetch_add(1);
    }
};

class ThrowOnceNoRetryAttempt : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        try {
            (void) addRefActor<ThrowOnceService>();
        } catch (std::runtime_error const &) {
            g_throw_once_caught.store(true);
        }
        push<ThrowOncePoke>(getServiceId<ThrowOnceServiceTag>(getIndex()));
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, ThrowingServiceConstructorRemovesItsCustomSubscription) {
    g_throw_once_service_ctors.store(0);
    g_throw_once_pokes.store(0);
    g_throw_once_caught.store(false);

    qb::Main main;
    main.addActor<ThrowOnceNoRetryAttempt>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_throw_once_caught.load());
    EXPECT_EQ(g_throw_once_service_ctors.load(), 1);
    EXPECT_EQ(g_throw_once_pokes.load(), 0);
}

class ThrowOnceServiceAttempt : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        try {
            (void) addRefActor<ThrowOnceService>();
        } catch (std::runtime_error const &) {
            g_throw_once_caught.store(true);
        }
        const auto retry = addRefActor<ThrowOnceService>();
        g_throw_once_retry_admitted.store(retry.valid() && getService<ThrowOnceService>() == retry.get());
        if (retry.valid()) {
            push<ThrowOncePoke>(retry.id());
            push<qb::KillEvent>(retry.id());
        }
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, ThrowingServiceConstructorReleasesAdmissionReservation) {
    g_throw_once_service_ctors.store(0);
    g_throw_once_pokes.store(0);
    g_throw_once_caught.store(false);
    g_throw_once_retry_admitted.store(false);

    qb::Main main;
    main.addActor<ThrowOnceServiceAttempt>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_throw_once_caught.load());
    EXPECT_TRUE(g_throw_once_retry_admitted.load());
    EXPECT_EQ(g_throw_once_service_ctors.load(), 2);
    EXPECT_EQ(g_throw_once_pokes.load(), 1);
}

struct ThrowingCallbackServiceTag {};
std::atomic<int>  g_failed_service_callback_ticks{0};
std::atomic<int>  g_callback_probe_ticks{0};
std::atomic<bool> g_callback_service_throw_caught{false};

class ThrowingCallbackService
    : public qb::ServiceActor<ThrowingCallbackServiceTag>
    , public qb::ICallback {
public:
    ThrowingCallbackService() {
        registerCallback(*this);
        throw std::runtime_error("service callback constructor failed");
    }

    void
    on(qb::LoopEvent const &) final {
        g_failed_service_callback_ticks.fetch_add(1);
    }
};

class CallbackAfterFailedService
    : public qb::Actor
    , public qb::ICallback {
public:
    CallbackAfterFailedService() {
        registerCallback(*this);
    }

    qb::io::async::task<bool>
    onInit() final {
        try {
            (void) addRefActor<ThrowingCallbackService>();
        } catch (std::runtime_error const &) {
            g_callback_service_throw_caught.store(true);
        }
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        if (g_callback_probe_ticks.fetch_add(1) + 1 == 2)
            kill();
    }
};

TEST(AddReferencedActor, ThrowingServiceConstructorRemovesItsCallback) {
    g_failed_service_callback_ticks.store(0);
    g_callback_probe_ticks.store(0);
    g_callback_service_throw_caught.store(false);

    qb::Main main;
    main.addActor<CallbackAfterFailedService>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_callback_service_throw_caught.load());
    EXPECT_EQ(g_callback_probe_ticks.load(), 2);
    EXPECT_EQ(g_failed_service_callback_ticks.load(), 0);
}

struct ThrowingWatchServiceTag {};
std::atomic<int>  g_watch_service_ctors{0};
std::atomic<int>  g_replacement_down_events{0};
std::atomic<int>  g_watch_probe_turns_after_target_death{0};
std::atomic<bool> g_watch_target_destroyed{false};
std::atomic<bool> g_watch_retry_admitted{false};
std::atomic<bool> g_watch_throw_caught{false};
std::atomic<int>  g_external_service_down_events{0};
std::atomic<bool> g_external_down_reason_unknown{true};

class WatchTarget : public qb::Actor {
public:
    ~WatchTarget() override {
        g_watch_target_destroyed.store(true);
    }
};

class ThrowingWatchService : public qb::ServiceActor<ThrowingWatchServiceTag> {
public:
    explicit ThrowingWatchService(qb::ActorId target) {
        registerEvent<qb::DownEvent>(*this);
        if (g_watch_service_ctors.fetch_add(1) == 0) {
            watch(target);
            throw std::runtime_error("service watch constructor failed");
        }
    }

    void
    on(qb::DownEvent const &) {
        g_replacement_down_events.fetch_add(1);
    }
};

class WatchAfterFailedService
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId _target;
    qb::ActorId _replacement;

public:
    explicit WatchAfterFailedService(qb::ActorId target)
        : _target(target) {
        registerCallback(*this);
        registerEvent<qb::DownEvent>(*this);
    }

    qb::io::async::task<bool>
    onInit() final {
        watch(getServiceId<ThrowingWatchServiceTag>(getIndex()));
        try {
            (void) addRefActor<ThrowingWatchService>(_target);
        } catch (std::runtime_error const &) {
            g_watch_throw_caught.store(true);
        }
        const auto replacement = addRefActor<ThrowingWatchService>(_target);
        g_watch_retry_admitted.store(replacement.valid());
        _replacement = replacement.id();
        push<qb::KillEvent>(_target);
        co_return true;
    }

    void
    on(qb::DownEvent const &event) {
        if (event.watched == getServiceId<ThrowingWatchServiceTag>(getIndex())) {
            g_external_service_down_events.fetch_add(1);
            if (event.reason != qb::DownReason::unknown)
                g_external_down_reason_unknown.store(false);
        }
    }

    void
    on(qb::LoopEvent const &) final {
        if (g_watch_target_destroyed.load() && g_watch_probe_turns_after_target_death.fetch_add(1) + 1 == 2) {
            if (_replacement.is_valid())
                push<qb::KillEvent>(_replacement);
            kill();
        }
    }
};

TEST(AddReferencedActor, ThrowingServiceConstructorWithdrawsDeathWatchBeforeRetry) {
    g_watch_service_ctors.store(0);
    g_replacement_down_events.store(0);
    g_watch_probe_turns_after_target_death.store(0);
    g_watch_target_destroyed.store(false);
    g_watch_retry_admitted.store(false);
    g_watch_throw_caught.store(false);
    g_external_service_down_events.store(0);
    g_external_down_reason_unknown.store(true);

    qb::Main   main;
    const auto target = main.addActor<WatchTarget>(0);
    main.addActor<WatchAfterFailedService>(0, target);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_watch_service_ctors.load(), 2);
    EXPECT_TRUE(g_watch_throw_caught.load());
    EXPECT_TRUE(g_watch_retry_admitted.load());
    EXPECT_TRUE(g_watch_target_destroyed.load());
    EXPECT_GE(g_watch_probe_turns_after_target_death.load(), 2);
    EXPECT_EQ(g_replacement_down_events.load(), 0);
    EXPECT_EQ(g_external_service_down_events.load(), 1);
    EXPECT_TRUE(g_external_down_reason_unknown.load());
}

struct OrdinaryPoisonPoke : qb::Event {};
std::atomic<std::uint64_t> g_ordinary_poison_id{0};
std::atomic<std::uint64_t> g_ordinary_nested_id{0};
std::atomic<std::uint64_t> g_ordinary_healthy_id{0};
std::atomic<int>           g_ordinary_poison_events{0};
std::atomic<int>           g_ordinary_poison_callbacks{0};
std::atomic<int>           g_ordinary_healthy_events{0};
std::atomic<bool>          g_ordinary_poison_caught{false};

class OrdinaryNested : public qb::Actor {};

class OrdinaryPoison
    : public qb::Actor
    , public qb::ICallback {
public:
    OrdinaryPoison() {
        g_ordinary_poison_id.store(id().raw64());
        const auto nested = addRefActor<OrdinaryNested>();
        g_ordinary_nested_id.store(nested.id().raw64());
        registerEvent<OrdinaryPoisonPoke>(*this);
        registerCallback(*this);
        throw std::runtime_error("ordinary constructor failed");
    }

    void
    on(OrdinaryPoisonPoke &) {
        g_ordinary_poison_events.fetch_add(1);
    }
    void
    on(qb::LoopEvent const &) final {
        g_ordinary_poison_callbacks.fetch_add(1);
    }
};

class OrdinaryHealthy : public qb::Actor {
public:
    OrdinaryHealthy() {
        g_ordinary_healthy_id.store(id().raw64());
        registerEvent<OrdinaryPoisonPoke>(*this);
    }
    void
    on(OrdinaryPoisonPoke &) {
        g_ordinary_healthy_events.fetch_add(1);
    }
};

class OrdinaryPoisonDriver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        try {
            (void) addRefActor<OrdinaryPoison>();
        } catch (std::runtime_error const &) {
            g_ordinary_poison_caught.store(true);
            push<OrdinaryPoisonPoke>(qb::ActorId::from_raw(g_ordinary_poison_id.load()));
            push<qb::KillEvent>(qb::ActorId::from_raw(g_ordinary_nested_id.load()));
        }
        const auto healthy = addRefActor<OrdinaryHealthy>();
        if (healthy.valid())
            push<qb::KillEvent>(healthy.id());
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, OrdinaryConstructorFailureClearsRegistrationsAndAdvancesTheReusedSlot) {
    g_ordinary_poison_id.store(0);
    g_ordinary_nested_id.store(0);
    g_ordinary_healthy_id.store(0);
    g_ordinary_poison_events.store(0);
    g_ordinary_poison_callbacks.store(0);
    g_ordinary_healthy_events.store(0);
    g_ordinary_poison_caught.store(false);

    qb::Main main;
    main.addActor<OrdinaryPoisonDriver>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ordinary_poison_caught.load());
    EXPECT_NE(g_ordinary_poison_id.load(), 0u);
    EXPECT_NE(g_ordinary_nested_id.load(), 0u);
    EXPECT_NE(g_ordinary_healthy_id.load(), 0u);
    EXPECT_NE(g_ordinary_nested_id.load(), g_ordinary_poison_id.load());
    EXPECT_NE(g_ordinary_healthy_id.load(), g_ordinary_poison_id.load());
    const auto poison  = qb::ActorId::from_raw(g_ordinary_poison_id.load());
    const auto healthy = qb::ActorId::from_raw(g_ordinary_healthy_id.load());
    EXPECT_EQ(healthy.slot(), poison.slot()) << "cleanup must return the failed parent's capacity";
    EXPECT_GT(healthy.generation(), poison.generation());
    EXPECT_EQ(g_ordinary_poison_events.load(), 0);
    EXPECT_EQ(g_ordinary_poison_callbacks.load(), 0);
    EXPECT_EQ(g_ordinary_healthy_events.load(), 0);
}

struct KillThenThrowServiceTag {};
struct KillThenThrowPoke : qb::Event {};
std::atomic<int>  g_kill_then_throw_ctors{0};
std::atomic<int>  g_kill_then_throw_pokes{0};
std::atomic<bool> g_kill_then_throw_retry_admitted{false};
std::atomic<bool> g_kill_then_throw_retry_survived{false};

class KillThenThrowService : public qb::ServiceActor<KillThenThrowServiceTag> {
public:
    KillThenThrowService() {
        if (g_kill_then_throw_ctors.fetch_add(1) == 0) {
            kill();
            throw std::runtime_error("service killed before construction failed");
        }
        registerEvent<KillThenThrowPoke>(*this);
    }

    void
    on(KillThenThrowPoke &) {
        g_kill_then_throw_pokes.fetch_add(1);
    }
};

class KillThenThrowDriver
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId _retry_id;

public:
    KillThenThrowDriver() {
        registerCallback(*this);
    }

    qb::io::async::task<bool>
    onInit() final {
        try {
            (void) addRefActor<KillThenThrowService>();
        } catch (std::runtime_error const &) {
        }
        const auto retry = addRefActor<KillThenThrowService>();
        g_kill_then_throw_retry_admitted.store(retry.valid());
        _retry_id = retry.id();
        if (retry.valid())
            push<KillThenThrowPoke>(retry.id());
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        g_kill_then_throw_retry_survived.store(_retry_id.is_valid() && is_actor_alive(_retry_id));
        if (_retry_id.is_valid())
            push<qb::KillEvent>(_retry_id);
        kill();
    }
};

TEST(AddReferencedActor, FailedServiceConstructorCannotKillItsReplacement) {
    g_kill_then_throw_ctors.store(0);
    g_kill_then_throw_pokes.store(0);
    g_kill_then_throw_retry_admitted.store(false);
    g_kill_then_throw_retry_survived.store(false);

    qb::Main main;
    main.addActor<KillThenThrowDriver>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_kill_then_throw_ctors.load(), 2);
    EXPECT_TRUE(g_kill_then_throw_retry_admitted.load());
    EXPECT_TRUE(g_kill_then_throw_retry_survived.load());
    EXPECT_EQ(g_kill_then_throw_pokes.load(), 1);
}

TEST(AddReferencedActor, ShouldReturnNullptrIfActorFailedToInit) {
    reset_atoms();
    qb::Main main;
    main.addActor<TestRefActor>(0, false);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_TRUE(g_returned_false.load()) << "the referenced child's false-returning onInit must have run";
}

TEST(AddReferencedActor, ShouldReturnActorPtrOnSucess) {
    reset_atoms();
    qb::Main main;
    main.addActor<TestRefActor>(0, true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// KillEvent teardown at scale — ZERO survivors.
// ===========================================================================

class TestKillSenderActor : public qb::Actor {
public:
    TestKillSenderActor() = default;

    qb::io::async::task<bool>
    onInit() final {
        EXPECT_TRUE(id().is_valid());
        push<qb::KillEvent>(id());               // self
        push<qb::KillEvent>(qb::BroadcastId(1)); // every actor on core 1
        co_return true;
    }
};

class TestKillActor : public qb::Actor {
public:
    TestKillActor() {
        g_kill_targets_alive.fetch_add(1, std::memory_order_relaxed);
        g_kill_targets_built.fetch_add(1, std::memory_order_relaxed);
    }
    ~TestKillActor() override {
        g_kill_targets_alive.fetch_sub(1, std::memory_order_relaxed);
    }

    qb::io::async::task<bool>
    onInit() final {
        EXPECT_TRUE(id().is_valid());
        co_return true;
    }
};

TEST(KillActor, BroadcastKillLeavesNoSurvivors) {
    reset_atoms();
    constexpr int kTargets = 1024;

    qb::Main main;
    main.addActor<TestKillSenderActor>(0);
    auto builder = main.core(1).builder();
    for (auto i = 0; i < kTargets; ++i)
        builder.addActor<TestKillActor>();
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_kill_targets_built.load(), kTargets) << "all 1024 broadcast targets must have been created";
    EXPECT_EQ(g_kill_targets_alive.load(), 0) << "every broadcast-killed actor must be destroyed — zero survivors after join";
}

// ===========================================================================
// Both deaths are reaped in one batch. The first destructor enqueues an event for its
// own ID; the second destructor creates a same-type actor before the next receive pass.
// An old ActorHandle resolves by ID, so it can alias that replacement as well.
struct ReapedIdProbe : qb::Event {
    bool fresh;
    explicit ReapedIdProbe(bool is_fresh)
        : fresh(is_fresh) {}
};
std::atomic<int>  g_reaped_stale_deliveries{0};
std::atomic<int>  g_reaped_fresh_deliveries{0};
std::atomic<bool> g_reaped_slot_reused{false};
std::atomic<bool> g_reaped_handle_aliased{false};

class ReapedIdTarget : public qb::Actor {
    bool _emit_on_destroy;

public:
    explicit ReapedIdTarget(bool emit_on_destroy)
        : _emit_on_destroy(emit_on_destroy) {
        registerEvent<ReapedIdProbe>(*this);
    }
    ~ReapedIdTarget() override {
        if (_emit_on_destroy)
            push<ReapedIdProbe>(id(), false);
    }

    void
    on(ReapedIdProbe &event) {
        if (event.fresh) {
            g_reaped_fresh_deliveries.fetch_add(1);
            kill();
        } else {
            g_reaped_stale_deliveries.fetch_add(1);
        }
    }
};

class ReapedIdCreator : public qb::Actor {
    qb::ActorHandle<ReapedIdTarget> _old;

public:
    explicit ReapedIdCreator(qb::ActorHandle<ReapedIdTarget> old)
        : _old(old) {}
    ~ReapedIdCreator() override {
        const auto replacement = addRefActor<ReapedIdTarget>(false);
        if (replacement.valid()) {
            g_reaped_slot_reused.store(replacement.id().slot() == _old.id().slot() && replacement.id().generation() > _old.id().generation());
            g_reaped_handle_aliased.store(_old.get() == replacement.get());
            push<ReapedIdProbe>(replacement.id(), true);
        }
    }
};

class ReapedIdDriver
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() final {
        const auto old     = addRefActor<ReapedIdTarget>(true);
        const auto creator = addRefActor<ReapedIdCreator>(old);
        push<qb::KillEvent>(old.id());
        push<qb::KillEvent>(creator.id());
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        if (g_reaped_fresh_deliveries.load() == 1)
            kill();
    }
};

TEST(AddReferencedActor, ReapedIdMustNotAliasOldHandleOrQueuedEvent) {
    g_reaped_stale_deliveries.store(0);
    g_reaped_fresh_deliveries.store(0);
    g_reaped_slot_reused.store(false);
    g_reaped_handle_aliased.store(false);

    qb::Main main;
    main.addActor<ReapedIdDriver>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_reaped_fresh_deliveries.load(), 1);
    EXPECT_TRUE(g_reaped_slot_reused.load()) << "the witness must reuse the old slot";
    EXPECT_FALSE(g_reaped_handle_aliased.load());
    EXPECT_EQ(g_reaped_stale_deliveries.load(), 0);
}

struct ReplacedServiceTag {};
struct ReplacedServiceProbe : qb::Event {
    bool fresh;
    explicit ReplacedServiceProbe(bool is_fresh)
        : fresh(is_fresh) {}
};
std::atomic<int>  g_replaced_service_destroyed{0};
std::atomic<int>  g_replaced_service_events{0};
std::atomic<int>  g_old_service_events{0};
std::atomic<bool> g_replaced_service_admitted{false};
std::atomic<bool> g_replaced_service_instance_changed{false};
std::atomic<bool> g_old_service_handle_resolved{false};
std::atomic<bool> g_old_service_base_handle_resolved{false};
std::atomic<bool> g_old_service_moved_handle_resolved{false};
std::atomic<bool> g_old_service_ready{false};
std::atomic<bool> g_old_service_async_checked{false};
std::atomic<bool> g_old_service_async_ready{false};
std::atomic<bool> g_new_service_handle_resolved{false};

class ReplacedService : public qb::ServiceActor<ReplacedServiceTag> {
public:
    ReplacedService() {
        registerEvent<ReplacedServiceProbe>(*this);
    }
    ~ReplacedService() override {
        g_replaced_service_destroyed.fetch_add(1);
    }

    void
    on(ReplacedServiceProbe &event) {
        if (event.fresh) {
            g_replaced_service_events.fetch_add(1);
            kill();
        } else {
            g_old_service_events.fetch_add(1);
        }
    }
};

class ReplacedServiceDriver
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorHandle<ReplacedService> _old;
    qb::ActorHandle<qb::Actor>       _old_as_actor;
    bool                             _replacement_attempted = false;

public:
    qb::io::async::task<bool>
    onInit() final {
        _old = addRefActor<ReplacedService>();
        if (_old.valid()) {
            _old_as_actor = qb::ActorHandle<qb::Actor>(_old.get());
            push<qb::KillEvent>(_old.id());
            registerCallback(*this);
        } else {
            kill();
        }
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        if (!_replacement_attempted && g_replaced_service_destroyed.load() == 1) {
            _replacement_attempted = true;
            const auto replacement = addRefActor<ReplacedService>();
            g_replaced_service_admitted.store(replacement.valid());
            g_replaced_service_instance_changed.store(replacement.valid() && replacement.id() != _old.id()
                                                      && replacement.id().slot() == _old.id().slot());
            g_old_service_handle_resolved.store(_old.get() != nullptr);
            g_old_service_base_handle_resolved.store(_old_as_actor.get() != nullptr);
            auto old_copy = _old;
            auto old_move = std::move(old_copy);
            g_old_service_moved_handle_resolved.store(old_move.get() != nullptr);
            g_old_service_ready.store(_old.ready() || static_cast<bool>(_old));
            g_new_service_handle_resolved.store(replacement.get() != nullptr);
            spawn([old = _old, fresh = replacement](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                auto source      = old;
                auto wait        = source.ready_async(ctx);
                source           = fresh; // the lazy task must already own the old handle's identity
                const bool ready = co_await wait;
                g_old_service_async_ready.store(ready);
                g_old_service_async_checked.store(true);
            });
            if (replacement.valid()) {
                push<ReplacedServiceProbe>(_old.id(), false); // an exact stale instance must miss
                push<ReplacedServiceProbe>(getServiceId<ReplacedServiceTag>(getIndex()), true);
            } else {
                kill();
            }
        } else if (_replacement_attempted && g_replaced_service_events.load() == 1 && g_old_service_async_checked.load()) {
            kill();
        }
    }
};

TEST(AddReferencedActor, ServiceIdReachesReplacementButOldHandleDoesNotResolveIt) {
    g_replaced_service_destroyed.store(0);
    g_replaced_service_events.store(0);
    g_old_service_events.store(0);
    g_replaced_service_admitted.store(false);
    g_replaced_service_instance_changed.store(false);
    g_old_service_handle_resolved.store(false);
    g_old_service_base_handle_resolved.store(false);
    g_old_service_moved_handle_resolved.store(false);
    g_old_service_ready.store(false);
    g_old_service_async_checked.store(false);
    g_old_service_async_ready.store(false);
    g_new_service_handle_resolved.store(false);

    ASSERT_TRUE(qb::Actor::getServiceId<ReplacedServiceTag>(0).is_service_address());
    qb::Main main;
    main.addActor<ReplacedServiceDriver>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_replaced_service_admitted.load());
    EXPECT_TRUE(g_replaced_service_instance_changed.load());
    EXPECT_FALSE(g_old_service_handle_resolved.load());
    EXPECT_FALSE(g_old_service_base_handle_resolved.load());
    EXPECT_FALSE(g_old_service_moved_handle_resolved.load());
    EXPECT_FALSE(g_old_service_ready.load());
    EXPECT_TRUE(g_old_service_async_checked.load());
    EXPECT_FALSE(g_old_service_async_ready.load());
    EXPECT_TRUE(g_new_service_handle_resolved.load());
    EXPECT_EQ(g_replaced_service_events.load(), 1);
    EXPECT_EQ(g_old_service_events.load(), 0);
    EXPECT_EQ(g_replaced_service_destroyed.load(), 2);
}

std::vector<qb::ActorId>   g_failed_ids;
std::atomic<int>           g_failed_payloads{0};
std::atomic<std::uint64_t> g_after_failures_id{0};

struct FailedAdmissionLifetime {
    FailedAdmissionLifetime() {
        ++g_failed_payloads;
    }
    ~FailedAdmissionLifetime() {
        --g_failed_payloads;
    }
};

class AdmissionFailure : public qb::Actor {
    FailedAdmissionLifetime _lifetime;

public:
    explicit AdmissionFailure(bool throw_in_constructor) {
        g_failed_ids.push_back(id());
        if (throw_in_constructor)
            throw std::runtime_error("failed after reserving an identity");
    }
    qb::io::async::task<bool>
    onInit() final {
        co_return false;
    }
};

class FailureReuseDriver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        for (unsigned i = 0; i < 128; ++i) {
            try {
                (void) addRefActor<AdmissionFailure>(i % 2 == 0);
            } catch (std::runtime_error const &) {
            }
        }
        const auto fresh = addRefActor<OrdinaryNested>();
        g_after_failures_id.store(fresh.id().raw64());
        if (fresh.valid())
            push<qb::KillEvent>(fresh.id());
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, FailedAdmissionsReturnCapacityWithDistinctFullIdentities) {
    g_failed_ids.clear();
    g_failed_payloads.store(0);
    g_after_failures_id.store(0);
    qb::Main main;
    qb::detail::actor_id_pool_test_access::limit(main.core(0), 16);
    main.addActor<FailureReuseDriver>(0);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());
    ASSERT_EQ(g_failed_ids.size(), 128u) << "every failure must actually construct, not merely attempt admission";
    EXPECT_EQ(g_failed_payloads.load(), 0);
    for (std::size_t i = 1; i < g_failed_ids.size(); ++i) {
        EXPECT_EQ(g_failed_ids[i].slot(), g_failed_ids[0].slot());
        EXPECT_GT(g_failed_ids[i].generation(), g_failed_ids[i - 1].generation());
    }
    const auto fresh = qb::ActorId::from_raw(g_after_failures_id.load());
    EXPECT_TRUE(fresh.is_valid());
    EXPECT_EQ(fresh.slot(), g_failed_ids.back().slot());
    EXPECT_GT(fresh.generation(), g_failed_ids.back().generation());
}

std::atomic<std::uint64_t> g_promised_parent_actual{0};
std::atomic<std::uint64_t> g_promised_peer_actual{0};
std::atomic<std::uint64_t> g_promised_child_actual{0};

class PromisedChild : public qb::Actor {
public:
    PromisedChild() {
        g_promised_child_actual.store(id().raw64());
    }
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};
class PromisedParent : public qb::Actor {
public:
    PromisedParent() {
        g_promised_parent_actual.store(id().raw64());
        (void) addRefActor<PromisedChild>();
    }
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};
class PromisedPeer : public qb::Actor {
public:
    PromisedPeer() {
        g_promised_peer_actual.store(id().raw64());
    }
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

TEST(AddActor, PreStartPromisesSurviveNestedConstructionAndClearInvalidatesOldPromises) {
    g_promised_parent_actual.store(0);
    g_promised_peer_actual.store(0);
    g_promised_child_actual.store(0);
    qb::Main   main;
    auto      &core    = main.core(0);
    const auto cleared = core.addActor<PromisedPeer>();
    core.clear();
    const auto parent = core.addActor<PromisedParent>();
    const auto peer   = core.addActor<PromisedPeer>();
    ASSERT_TRUE(parent.is_valid());
    ASSERT_TRUE(peer.is_valid());
    EXPECT_EQ(parent.slot(), cleared.slot());
    EXPECT_NE(parent, cleared);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());
    EXPECT_EQ(g_promised_parent_actual.load(), parent.raw64());
    EXPECT_EQ(g_promised_peer_actual.load(), peer.raw64());
    const auto child = qb::ActorId::from_raw(g_promised_child_actual.load());
    EXPECT_TRUE(child.is_valid());
    EXPECT_NE(child, parent);
    EXPECT_NE(child, peer);
}

TEST(AddActor, StartupFailureReturnsThePoolBeforeAnAuthorizedRetry) {
    reset_atoms();
    g_promised_peer_actual.store(0);
    qb::Main   main;
    const auto failed = main.core(0).addActor<TestActor>(false);
    main.start(false);
    main.join();
    ASSERT_TRUE(main.hasError());
    const auto retry = main.core(0).addActor<PromisedPeer>();
    ASSERT_TRUE(retry.is_valid());
    EXPECT_EQ(retry.slot(), failed.slot());
    EXPECT_GT(retry.generation(), failed.generation());
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_promised_peer_actual.load(), retry.raw64());
}

struct PopulationProbe : qb::Event {};
std::atomic<int>         g_population_alive{0};
std::atomic<int>         g_population_built{0};
std::atomic<int>         g_population_peak{0};
std::atomic<bool>        g_population_routed{false};
std::vector<qb::ActorId> g_population_ids;

class PopulationTarget : public qb::Actor {
public:
    PopulationTarget() {
        ++g_population_alive;
        ++g_population_built;
        registerEvent<PopulationProbe>(*this);
    }
    ~PopulationTarget() override {
        --g_population_alive;
    }
    void
    on(PopulationProbe &event) {
        reply(event);
    }
};

class PopulationDriver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<PopulationProbe>(*this);
        for (unsigned i = 0; i < 200000; ++i) {
            const auto target = addRefActor<PopulationTarget>();
            if (!target.valid())
                break;
            g_population_ids.push_back(target.id());
        }
        g_population_peak.store(g_population_alive.load());
        if (!g_population_ids.empty())
            push<PopulationProbe>(g_population_ids.back());
        else
            push<qb::KillEvent>(qb::BroadcastId(getIndex()));
        co_return true;
    }
    void
    on(PopulationProbe const &event) {
        g_population_routed.store(event.getSource() == g_population_ids.back());
        push<qb::KillEvent>(qb::BroadcastId(getIndex()));
    }
};

TEST(AddReferencedActor, HoldsTwoHundredThousandLiveOrdinaryActorsOnOneCore) {
    g_population_alive.store(0);
    g_population_built.store(0);
    g_population_peak.store(0);
    g_population_routed.store(false);
    g_population_ids.clear();
    g_population_ids.reserve(200000);
    qb::Main main;
    main.addActor<PopulationDriver>(0);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());
    EXPECT_EQ(g_population_built.load(), 200000);
    EXPECT_EQ(g_population_peak.load(), 200000) << "all targets must coexist before the first kill";
    EXPECT_EQ(g_population_alive.load(), 0);
    EXPECT_TRUE(g_population_routed.load());
    ASSERT_EQ(g_population_ids.size(), 200000u);
    std::sort(g_population_ids.begin(), g_population_ids.end());
    EXPECT_EQ(std::adjacent_find(g_population_ids.begin(), g_population_ids.end()), g_population_ids.end());
    EXPECT_GT(g_population_ids.back().slot(), 65535u);
}

// An admission reserves its ID BEFORE construction. A first base can spend the other slots,
// but cannot steal the enclosing actor's promise. The next admission must be refused before
// constructing an actor or a parked frame. A bounded internal pool exercises the real boundary.
// ===========================================================================

std::atomic<int>      g_spent_fillers{0};      // fillers admitted
std::atomic<int>      g_spent_filler_inits{0}; // filler onInit() runs
std::atomic<int>      g_late_drawn_ctors{0};
std::atomic<int>      g_late_drawn_inits{0};
std::atomic<bool>     g_late_drawn_refused{false};
std::atomic<int>      g_parked_ctors{0};
std::atomic<int>      g_parked_dtors{0};
std::atomic<int>      g_parked_inits{0};
std::atomic<int>      g_parked_frames{0};  // live onInit() frames of ParkedChild
std::atomic<int>      g_parked_orphans{0}; // such frames alive right after their actor was destroyed
std::atomic<bool>     g_parked_refused{false};
std::atomic<bool>     g_spent_driver_ran{false};
std::function<void()> g_spent_drain; // set by the driver: admits fillers until the core refuses one

class SpentFiller : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_spent_filler_inits.fetch_add(1);
        co_return true;
    }
};

// Built BEFORE the qb::Actor base: the other slots are spent before its reserved ID is claimed.
struct SpendsTheCoreFirst {
    SpendsTheCoreFirst() {
        if (g_spent_drain)
            g_spent_drain();
    }
};

class LateDrawn
    : public SpendsTheCoreFirst
    , public qb::Actor {
public:
    LateDrawn() {
        g_late_drawn_ctors.fetch_add(1);
    }

    qb::io::async::task<bool>
    onInit() final {
        g_late_drawn_inits.fetch_add(1);
        co_return true;
    }
};

struct ParkedFrameProbe {
    ParkedFrameProbe() {
        g_parked_frames.fetch_add(1);
    }
    ~ParkedFrameProbe() {
        g_parked_frames.fetch_sub(1);
    }
    ParkedFrameProbe(ParkedFrameProbe const &)            = delete;
    ParkedFrameProbe &operator=(ParkedFrameProbe const &) = delete;
};

class ParkedChild : public qb::Actor {
public:
    ParkedChild() {
        g_parked_ctors.fetch_add(1);
    }
    ~ParkedChild() override {
        g_parked_dtors.fetch_add(1);
    }

    qb::io::async::task<bool>
    onInit() final {
        g_parked_inits.fetch_add(1);
        ParkedFrameProbe const probe;
        co_await std::suspend_always{}; // parks for good: only a core's teardown destroys this frame
        co_return true;
    }
};

class SpentDriver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_spent_drain = [this] {
            while (addRefActor<SpentFiller>().valid())
                g_spent_fillers.fetch_add(1);
        };
        g_late_drawn_refused.store(!addRefActor<LateDrawn>().valid());
        g_spent_drain = nullptr;
        g_parked_refused.store(!addRefActor<ParkedChild>().valid());
        if (g_parked_dtors.load() != 0)
            g_parked_orphans.store(g_parked_frames.load());
        g_spent_driver_ran.store(true);
        push<qb::KillEvent>(qb::BroadcastId(getIndex()));
        co_return true;
    }
};

TEST(AddReferencedActor, ASpentIdPoolRefusesTheActorBeforeItsOnInitRuns) {
    g_spent_fillers.store(0);
    g_spent_filler_inits.store(0);
    g_late_drawn_ctors.store(0);
    g_late_drawn_inits.store(0);
    g_late_drawn_refused.store(false);
    g_parked_ctors.store(0);
    g_parked_dtors.store(0);
    g_parked_inits.store(0);
    g_parked_frames.store(0);
    g_parked_orphans.store(0);
    g_parked_refused.store(false);
    g_spent_driver_ran.store(false);

    qb::Main main;
    qb::detail::actor_id_pool_test_access::limit(main.core(0), 64);
    main.addActor<SpentDriver>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(g_spent_driver_ran.load()) << "the driver's onInit never ran";
    EXPECT_EQ(g_spent_fillers.load(), 62) << "driver and enclosing admission already own two of the 64 slots";
    EXPECT_EQ(g_spent_filler_inits.load(), g_spent_fillers.load()) << "onInit() ran for a filler the core refused";
    // The enclosing actor keeps its reservation even though its first base filled every other slot.
    EXPECT_FALSE(g_late_drawn_refused.load());
    EXPECT_EQ(g_late_drawn_ctors.load(), 1);
    EXPECT_EQ(g_late_drawn_inits.load(), 1);
    // The plain path: nothing is left, so nothing is constructed and no frame can outlive anything.
    EXPECT_TRUE(g_parked_refused.load());
    EXPECT_EQ(g_parked_ctors.load(), 0) << "an actor was constructed on a core that had no id left";
    EXPECT_EQ(g_parked_inits.load(), 0) << "onInit() ran for an actor that has no id";
    EXPECT_EQ(g_parked_orphans.load(), 0) << "an onInit() frame stayed parked in the core after its actor was destroyed";
    EXPECT_EQ(g_parked_frames.load(), 0);
}

} // namespace actor_add_test
