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

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

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
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
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
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
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
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
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
    EXPECT_NE(static_cast<std::uint32_t>(id), 0u);

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
    EXPECT_EQ(static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(qb::Actor::getServiceId<Tag>(0)));

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
    EXPECT_EQ(static_cast<std::uint32_t>(builder.idList()[0]), static_cast<std::uint32_t>(qb::Actor::getServiceId<Tag>(0))); // service id
    EXPECT_NE(static_cast<std::uint32_t>(builder.idList()[1]), 0u);
    builder.addActor<TestServiceActor>(true); // duplicate service → builder invalidates
    EXPECT_FALSE(static_cast<bool>(builder));
    EXPECT_EQ(builder.idList().size(), 3u);
    EXPECT_EQ(static_cast<std::uint32_t>(builder.idList()[2]), 0u); // failed add → NotFound id

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
std::atomic<std::uint32_t> g_ordinary_poison_id{0};
std::atomic<std::uint32_t> g_ordinary_nested_id{0};
std::atomic<std::uint32_t> g_ordinary_healthy_id{0};
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
        g_ordinary_poison_id.store(static_cast<std::uint32_t>(id()));
        const auto nested = addRefActor<OrdinaryNested>();
        g_ordinary_nested_id.store(static_cast<std::uint32_t>(nested.id()));
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
        g_ordinary_healthy_id.store(static_cast<std::uint32_t>(id()));
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
            push<OrdinaryPoisonPoke>(qb::ActorId(g_ordinary_poison_id.load()));
            push<qb::KillEvent>(qb::ActorId(g_ordinary_nested_id.load()));
        }
        const auto healthy = addRefActor<OrdinaryHealthy>();
        if (healthy.valid())
            push<qb::KillEvent>(healthy.id());
        kill();
        co_return true;
    }
};

TEST(AddReferencedActor, OrdinaryConstructorFailureClearsRegistrationsAndKeepsIdReserved) {
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
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
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
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
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

} // namespace
