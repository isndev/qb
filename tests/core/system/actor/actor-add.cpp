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

#include <array>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
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

// ===========================================================================
// A core whose id pool is spent (Huly QB-1014).
//
// An actor draws its id in `Actor::Actor()`; once every id of its core is taken it gets NotFound. Until 3.3
// `appendActor` drove that actor's `onInit()` before noticing the id, so the coroutine of an actor the core then
// destroyed ran -- and when it suspended, its frame was parked in the core's activations under NotFound, alive
// until the core's teardown, while its actor was gone. Now `addRefActor` refuses a spent core before constructing
// anything, and `appendActor` refuses an id-less actor before its `onInit()`. Both paths are driven: the plain one
// (the pool is spent before the add), and the one only `appendActor` can catch -- a first base, built before
// `qb::Actor`, that takes the last ids itself. The fillers that spend the pool are ~65 000 actors on one core.
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

// Built BEFORE the qb::Actor base of the class below: the pool is spent when Actor::Actor() draws its id.
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
    main.addActor<SpentDriver>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(g_spent_driver_ran.load()) << "the driver's onInit never ran";
    EXPECT_GT(g_spent_fillers.load(), 60000) << "the fillers did not spend the core's id pool";
    EXPECT_EQ(g_spent_filler_inits.load(), g_spent_fillers.load()) << "onInit() ran for a filler the core refused";
    // The path only appendActor sees: constructed (its first base spent the pool), refused before its onInit().
    EXPECT_TRUE(g_late_drawn_refused.load());
    EXPECT_EQ(g_late_drawn_ctors.load(), 1);
    EXPECT_EQ(g_late_drawn_inits.load(), 0) << "onInit() ran for an actor that has no id";
    // The plain path: nothing is left, so nothing is constructed and no frame can outlive anything.
    EXPECT_TRUE(g_parked_refused.load());
    EXPECT_EQ(g_parked_ctors.load(), 0) << "an actor was constructed on a core that had no id left";
    EXPECT_EQ(g_parked_inits.load(), 0) << "onInit() ran for an actor that has no id";
    EXPECT_EQ(g_parked_orphans.load(), 0) << "an onInit() frame stayed parked in the core after its actor was destroyed";
    EXPECT_EQ(g_parked_frames.load(), 0);
}

} // namespace

namespace actor_add_red_witness_test {

// QB-264: observations outlive the failed actor; suspended work captures shared state only.
class ScopedAdmissionCallbackChild;

struct ScopedAdmissionProbe {
    qb::io::async::cancellation_token             token{qb::io::async::null_token};
    qb::ActorId                                   failed_id;
    qb::ActorId                                   healthy_id;
    qb::ActorId                                   callback_child_actual;
    qb::ActorHandle<ScopedAdmissionCallbackChild> callback_child;
    int                                           callback_child_ctors              = 0;
    int                                           callback_child_inits              = 0;
    int                                           callback_child_dtors              = 0;
    int                                           callback_child_deliveries         = 0;
    int                                           callback_child_init_sequence      = 0;
    bool                                          callback_after_destruction        = false;
    bool                                          callback_child_ready_in_callback  = false;
    bool                                          callback_child_coherent_at_return = false;
    bool                                          callback_child_survived_reap      = false;
    int                                           sequence                          = 0;
    int                                           cancel_sequence                   = 0;
    int                                           destructor_sequence               = 0;
    int                                           failed_ctors                      = 0;
    int                                           failed_inits                      = 0;
    int                                           failed_dtors                      = 0;
    int                                           cancel_calls                      = 0;
    int                                           scoped_entries                    = 0;
    int                                           healthy_ctors                     = 0;
    int                                           healthy_inits                     = 0;
    int                                           healthy_dtors                     = 0;
    bool                                          failure_path_reached              = false;
    bool                                          scoped_spawned                    = false;
    bool                                          scoped_saw_cancelled              = false;
    bool                                          cancelled_in_destructor           = false;
    bool                                          cancelled_at_admission_return     = false;
    bool                                          failed_handle_valid               = true;
    bool                                          healthy_admitted                  = false;
    bool                                          healthy_survived_reap             = false;
};

struct ScopedAdmissionCallbackEvent : qb::Event {};

class ScopedAdmissionCallbackChild : public qb::Actor {
    std::shared_ptr<ScopedAdmissionProbe> _probe;

public:
    explicit ScopedAdmissionCallbackChild(std::shared_ptr<ScopedAdmissionProbe> probe)
        : _probe(std::move(probe)) {
        ++_probe->callback_child_ctors;
        _probe->callback_child_actual = id();
    }
    ~ScopedAdmissionCallbackChild() override {
        ++_probe->callback_child_dtors;
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->callback_child_inits;
        _probe->callback_child_init_sequence = ++_probe->sequence;
        registerEvent<ScopedAdmissionCallbackEvent>(*this);
        co_return true;
    }
    void
    on(ScopedAdmissionCallbackEvent const &) {
        ++_probe->callback_child_deliveries;
    }
};

class ScopedAdmissionFailure : public qb::Actor {
    std::shared_ptr<ScopedAdmissionProbe> _probe;
    bool                                  _throw_in_init;
    bool                                  _self_kill;

public:
    ScopedAdmissionFailure(std::shared_ptr<ScopedAdmissionProbe> probe, bool throw_in_init, bool self_kill)
        : _probe(std::move(probe))
        , _throw_in_init(throw_in_init)
        , _self_kill(self_kill) {
        ++_probe->failed_ctors;
        _probe->failed_id = id();
    }

    ~ScopedAdmissionFailure() override {
        ++_probe->failed_dtors;
        _probe->destructor_sequence = ++_probe->sequence;
        // Observed in the derived destructor body, before member/base destruction completes.
        _probe->cancelled_in_destructor = _probe->token.is_cancelled();
    }

    qb::io::async::task<bool>
    onInit() final {
        ++_probe->failed_inits;
        ++_probe->sequence;
        const auto ctx                                 = context();
        _probe->token                                  = ctx.token();
        const std::weak_ptr<ScopedAdmissionProbe> weak = _probe;
        (void) _probe->token.on_cancel([weak, owner = this] {
            if (const auto probe = weak.lock()) {
                ++probe->cancel_calls;
                probe->cancel_sequence = ++probe->sequence;
                // The first-drive rollback must cancel while this temporary still exists.
                // Check shared liveness before touching owner, even on a broken control.
                if (probe->failed_dtors != 0) {
                    probe->callback_after_destruction = true;
                    return;
                }
                probe->callback_child                   = owner->addRefActor<ScopedAdmissionCallbackChild>(probe);
                const auto child                        = probe->callback_child;
                probe->callback_child_ready_in_callback = child.valid() && child.get() != nullptr;
                if (child.valid())
                    owner->push<ScopedAdmissionCallbackEvent>(child.id());
            }
        });
        const auto probe = _probe;
        spawn([probe](qb::ScopedCoroContext work_ctx) -> qb::io::async::task<void> {
            ++probe->scoped_entries;
            probe->scoped_saw_cancelled = work_ctx.token().is_cancelled();
            co_return;
        });
        _probe->scoped_spawned = true;
        // This control is separate from the no-kill failure: kill() must not mask that defect.
        if (_self_kill)
            kill();
        _probe->failure_path_reached = true;
        if (_throw_in_init)
            throw std::runtime_error("first dynamic onInit drive failed before suspension");
        co_return false;
    }
};

class ScopedAdmissionHealthy : public qb::Actor {
    std::shared_ptr<ScopedAdmissionProbe> _probe;

public:
    explicit ScopedAdmissionHealthy(std::shared_ptr<ScopedAdmissionProbe> probe)
        : _probe(std::move(probe)) {
        ++_probe->healthy_ctors;
        _probe->healthy_id = id();
    }
    ~ScopedAdmissionHealthy() override {
        ++_probe->healthy_dtors;
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->healthy_inits;
        co_return true;
    }
};

class ScopedAdmissionDriver
    : public qb::Actor
    , public qb::ICallback {
    std::shared_ptr<ScopedAdmissionProbe>   _probe;
    bool                                    _throw_in_init;
    bool                                    _self_kill;
    unsigned                                _ticks = 0;
    qb::ActorHandle<ScopedAdmissionHealthy> _healthy;

public:
    ScopedAdmissionDriver(std::shared_ptr<ScopedAdmissionProbe> probe, bool throw_in_init, bool self_kill)
        : _probe(std::move(probe))
        , _throw_in_init(throw_in_init)
        , _self_kill(self_kill) {}

    qb::io::async::task<bool>
    onInit() final {
        registerCallback(*this);
        const auto failed                     = addRefActor<ScopedAdmissionFailure>(_probe, _throw_in_init, _self_kill);
        _probe->failed_handle_valid           = failed.valid();
        _probe->cancelled_at_admission_return = _probe->token.is_cancelled();
        auto callback_child                   = _probe->callback_child;
        if (const auto *live = callback_child.get())
            _probe->callback_child_coherent_at_return = live->id() == _probe->callback_child_actual;
        _healthy                 = addRefActor<ScopedAdmissionHealthy>(_probe);
        _probe->healthy_admitted = _healthy.valid() && _healthy.get() != nullptr;
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        // Any queued self-kill is reaped before the first tick that can get here.
        if (_ticks == 0) {
            _probe->healthy_survived_reap        = _healthy.get() != nullptr;
            auto child                           = _probe->callback_child;
            _probe->callback_child_survived_reap = child.get() != nullptr;
        }
        if (++_ticks >= 2) {
            if (auto *live = _healthy.get())
                live->kill();
            auto child = _probe->callback_child;
            if (auto *live = child.get())
                live->kill();
            kill();
        }
    }
};

void
check_scoped_admission(bool throw_in_init) {
    for (bool self_kill : {false, true}) {
        SCOPED_TRACE(self_kill ? "self-kill rollback control" : "scope failure without self-kill");
        const auto probe = std::make_shared<ScopedAdmissionProbe>();
        qb::Main   main;
        main.addActor<ScopedAdmissionDriver>(0, probe, throw_in_init, self_kill);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
        EXPECT_EQ(probe->failed_ctors, 1);
        EXPECT_EQ(probe->failed_inits, 1);
        EXPECT_EQ(probe->failed_dtors, 1);
        EXPECT_TRUE(probe->failure_path_reached);
        EXPECT_TRUE(probe->scoped_spawned);
        EXPECT_FALSE(probe->failed_handle_valid);
        EXPECT_TRUE(probe->cancelled_in_destructor);
        EXPECT_TRUE(probe->cancelled_at_admission_return);
        EXPECT_EQ(probe->cancel_calls, 1);
        EXPECT_GT(probe->cancel_sequence, 0);
        EXPECT_LT(probe->cancel_sequence, probe->callback_child_init_sequence);
        EXPECT_LT(probe->callback_child_init_sequence, probe->destructor_sequence);
        EXPECT_FALSE(probe->callback_after_destruction);
        EXPECT_EQ(probe->callback_child_ctors, 1);
        EXPECT_EQ(probe->callback_child_inits, 1);
        EXPECT_EQ(probe->callback_child_dtors, 1);
        EXPECT_EQ(probe->callback_child_deliveries, 1);
        EXPECT_TRUE(probe->callback_child_ready_in_callback);
        EXPECT_TRUE(probe->callback_child_coherent_at_return);
        EXPECT_TRUE(probe->callback_child_survived_reap);
        EXPECT_EQ(probe->callback_child.id(), probe->callback_child_actual);
        EXPECT_TRUE(probe->callback_child_actual.is_valid());
        EXPECT_NE(probe->callback_child_actual, probe->failed_id);
        EXPECT_NE(probe->callback_child_actual, probe->healthy_id);
        EXPECT_LE(probe->scoped_entries, 1);
        if (probe->scoped_entries != 0) {
            EXPECT_TRUE(probe->scoped_saw_cancelled);
        }
        EXPECT_TRUE(probe->healthy_admitted);
        EXPECT_TRUE(probe->healthy_survived_reap);
        EXPECT_EQ(probe->healthy_ctors, 1);
        EXPECT_EQ(probe->healthy_inits, 1);
        EXPECT_EQ(probe->healthy_dtors, 1);
        EXPECT_TRUE(probe->failed_id.is_valid());
        EXPECT_TRUE(probe->healthy_id.is_valid());
        EXPECT_NE(probe->failed_id, probe->healthy_id);
    }
}

TEST(AddReferencedActor, DynamicFalseInitCancelsScopeBeforeDestruction) {
    check_scoped_admission(false);
}

TEST(AddReferencedActor, DynamicThrowingInitCancelsScopeBeforeDestruction) {
    check_scoped_admission(true);
}

} // namespace actor_add_red_witness_test

namespace actor_add_constructor_gate_test {

// All writes occur on core 0; the test body reads only after start(false)/join().
// The manual-reset gate owns no actor pointer and outlives every parked init frame.
struct ConstructorGateProbe {
    qb::io::async::async_event gate;
    qb::ActorId                parent_actual;
    qb::ActorId                peer_actual;
    qb::ActorId                child_actual;
    qb::ActorId                child_handle_id;
    bool                       init_succeeds;
    bool                       child_valid_in_constructor   = false;
    bool                       child_hidden_in_constructor  = false;
    bool                       child_hidden_in_parent_init  = false;
    bool                       startup_snapshot_observed    = false;
    bool                       gate_had_one_waiter          = false;
    bool                       stash_was_pending_at_release = false;
    bool                       ready_wait_started           = false;
    bool                       ready_wait_returned          = false;
    bool                       ready_result                 = false;
    bool                       ready_lookup_coherent        = false;
    bool                       drain_correct_at_ready       = false;
    bool                       phase_limit_reached          = false;
    int                        sequence                     = 0;
    int                        release_sequence             = 0;
    int                        init_complete_sequence       = 0;
    int                        ready_sequence               = 0;
    int                        child_destructor_sequence    = 0;
    int                        parent_ctors                 = 0;
    int                        parent_inits                 = 0;
    int                        parent_dtors                 = 0;
    int                        peer_ctors                   = 0;
    int                        peer_inits                   = 0;
    int                        peer_dtors                   = 0;
    int                        child_ctors                  = 0;
    int                        child_inits                  = 0;
    int                        child_completions            = 0;
    int                        child_dtors                  = 0;
    int                        payload_ctors                = 0;
    int                        payload_dtors                = 0;
    int                        deliveries                   = 0;
    std::array<unsigned, 2>    delivered_order{};

    explicit ConstructorGateProbe(bool succeeds)
        : init_succeeds(succeeds) {}
};

struct ConstructorGatePayload {
    std::weak_ptr<ConstructorGateProbe> probe;

    explicit ConstructorGatePayload(const std::shared_ptr<ConstructorGateProbe> &observations)
        : probe(observations) {
        ++observations->payload_ctors;
    }
    ~ConstructorGatePayload() {
        if (const auto observations = probe.lock())
            ++observations->payload_dtors;
    }
};

struct ConstructorGateWork : qb::Event {
    std::shared_ptr<ConstructorGatePayload> payload;
    unsigned                                serial;

    ConstructorGateWork(std::shared_ptr<ConstructorGatePayload> value, unsigned number)
        : payload(std::move(value))
        , serial(number) {}
};

class ConstructorGateChild : public qb::Actor {
    std::shared_ptr<ConstructorGateProbe> _probe;

public:
    explicit ConstructorGateChild(std::shared_ptr<ConstructorGateProbe> probe)
        : _probe(std::move(probe)) {
        ++_probe->child_ctors;
        _probe->child_actual = id();
    }
    ~ConstructorGateChild() override {
        ++_probe->child_dtors;
        _probe->child_destructor_sequence = ++_probe->sequence;
    }
    qb::io::async::task<bool>
    onInit() final {
        const auto probe = _probe;
        ++probe->child_inits;
        registerEvent<ConstructorGateWork>(*this);
        co_await probe->gate.wait();
        ++probe->child_completions;
        probe->init_complete_sequence = ++probe->sequence;
        co_return probe->init_succeeds;
    }
    void
    on(ConstructorGateWork const &event) {
        if (_probe->deliveries < static_cast<int>(_probe->delivered_order.size()))
            _probe->delivered_order[static_cast<std::size_t>(_probe->deliveries)] = event.serial;
        ++_probe->deliveries;
    }
};

class ConstructorGateParent
    : public qb::Actor
    , public qb::ICallback {
    std::shared_ptr<ConstructorGateProbe> _probe;
    qb::ActorHandle<ConstructorGateChild> _child;
    unsigned                              _ticks    = 0;
    bool                                  _released = false;

    void
    stop_all() {
        // Wake any surviving init before the kill: cleanup never relies on its deadline.
        _probe->gate.set();
        push<qb::KillEvent>(qb::BroadcastId(getIndex()));
        kill();
    }

public:
    explicit ConstructorGateParent(std::shared_ptr<ConstructorGateProbe> probe)
        : _probe(std::move(probe)) {
        ++_probe->parent_ctors;
        _probe->parent_actual               = id();
        _child                              = addRefActor<ConstructorGateChild>(_probe);
        auto child                          = _child;
        _probe->child_handle_id             = child.id();
        _probe->child_valid_in_constructor  = child.valid();
        _probe->child_hidden_in_constructor = child.get() == nullptr && !child.ready();
    }
    ~ConstructorGateParent() override {
        ++_probe->parent_dtors;
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->parent_inits;
        auto child                          = _child;
        _probe->child_hidden_in_parent_init = child.get() == nullptr && !child.ready();
        registerCallback(*this);
        for (unsigned serial : {1u, 2u})
            push<ConstructorGateWork>(child.id(), std::make_shared<ConstructorGatePayload>(_probe), serial);
        const auto probe = _probe;
        spawn([probe, child](qb::ScopedCoroContext ctx) mutable -> qb::io::async::task<void> {
            probe->ready_wait_started    = true;
            const bool ready             = co_await child.ready_async(ctx, qb::duration::zero());
            probe->ready_result          = ready;
            probe->ready_sequence        = ++probe->sequence;
            const auto *live             = child.get();
            probe->ready_lookup_coherent = ready ? live != nullptr && live->id() == probe->child_actual : live == nullptr;
            if (probe->init_succeeds)
                probe->drain_correct_at_ready = probe->deliveries == 2 && probe->payload_dtors == 2;
            else
                probe->drain_correct_at_ready = probe->deliveries == 0 && probe->payload_dtors == 2 && probe->child_dtors == 1;
            probe->ready_wait_returned = true;
        });
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        if (++_ticks > 64) {
            _probe->phase_limit_reached = true;
            stop_all();
            return;
        }
        if (!_released) {
            // The tick follows startup's complete snapshot walk and the receive phase.
            // The gate is still closed, so these observations prove a suspended first drive.
            if (!_probe->ready_wait_started)
                return;
            _probe->startup_snapshot_observed    = _probe->parent_inits == 1 && _probe->peer_inits == 1;
            _probe->gate_had_one_waiter          = _probe->gate.waiters_count() == 1;
            _probe->stash_was_pending_at_release = _probe->deliveries == 0 && _probe->payload_dtors == 0;
            _probe->release_sequence             = ++_probe->sequence;
            _released                            = true;
            _probe->gate.set();
            return;
        }
        if (_probe->ready_wait_returned)
            stop_all();
    }
};

class ConstructorGatePeer : public qb::Actor {
    std::shared_ptr<ConstructorGateProbe> _probe;

public:
    explicit ConstructorGatePeer(std::shared_ptr<ConstructorGateProbe> probe)
        : _probe(std::move(probe)) {
        ++_probe->peer_ctors;
        _probe->peer_actual = id();
    }
    ~ConstructorGatePeer() override {
        ++_probe->peer_dtors;
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->peer_inits;
        co_return true;
    }
};

TEST(AddActor, ConstructorCreatedSuspendedChildHasOneInitAndCoherentReadiness) {
    for (bool succeeds : {true, false}) {
        SCOPED_TRACE(succeeds ? "constructor child activates" : "constructor child fails after release");
        const auto probe = std::make_shared<ConstructorGateProbe>(succeeds);
        qb::Main   main;
        // Both startup factories already own their promises when nested construction runs.
        const auto parent = main.addActor<ConstructorGateParent>(0, probe);
        const auto peer   = main.addActor<ConstructorGatePeer>(0, probe);
        ASSERT_TRUE(parent.is_valid());
        ASSERT_TRUE(peer.is_valid());
        main.start(false);
        main.join();

        EXPECT_FALSE(main.hasError());
        EXPECT_FALSE(probe->phase_limit_reached);
        EXPECT_EQ(probe->parent_actual, parent);
        EXPECT_EQ(probe->peer_actual, peer);
        EXPECT_NE(probe->child_actual, parent);
        EXPECT_NE(probe->child_actual, peer);
        EXPECT_EQ(probe->child_handle_id, probe->child_actual);
        EXPECT_TRUE(probe->child_valid_in_constructor);
        EXPECT_TRUE(probe->child_hidden_in_constructor);
        EXPECT_TRUE(probe->child_hidden_in_parent_init);
        EXPECT_TRUE(probe->startup_snapshot_observed);
        EXPECT_TRUE(probe->gate_had_one_waiter);
        EXPECT_TRUE(probe->stash_was_pending_at_release);
        EXPECT_TRUE(probe->ready_wait_started);
        EXPECT_TRUE(probe->ready_wait_returned);
        EXPECT_EQ(probe->ready_result, succeeds);
        EXPECT_TRUE(probe->ready_lookup_coherent);
        EXPECT_TRUE(probe->drain_correct_at_ready);
        EXPECT_EQ(probe->parent_ctors, 1);
        EXPECT_EQ(probe->parent_inits, 1);
        EXPECT_EQ(probe->parent_dtors, 1);
        EXPECT_EQ(probe->peer_ctors, 1);
        EXPECT_EQ(probe->peer_inits, 1);
        EXPECT_EQ(probe->peer_dtors, 1);
        EXPECT_EQ(probe->child_ctors, 1);
        EXPECT_EQ(probe->child_inits, 1);
        EXPECT_EQ(probe->child_completions, 1);
        EXPECT_EQ(probe->child_dtors, 1);
        EXPECT_EQ(probe->payload_ctors, 2);
        EXPECT_EQ(probe->payload_dtors, 2);
        EXPECT_EQ(probe->deliveries, succeeds ? 2 : 0);
        EXPECT_LT(probe->release_sequence, probe->init_complete_sequence);
        EXPECT_LT(probe->init_complete_sequence, probe->ready_sequence);
        EXPECT_EQ(probe->gate.waiters_count(), 0u);
        if (succeeds) {
            EXPECT_EQ(probe->delivered_order[0], 1u);
            EXPECT_EQ(probe->delivered_order[1], 2u);
            EXPECT_LT(probe->ready_sequence, probe->child_destructor_sequence);
        } else {
            EXPECT_LT(probe->child_destructor_sequence, probe->ready_sequence);
        }
    }
}

} // namespace actor_add_constructor_gate_test

namespace actor_add_const_handle_test {
struct Observation {
    bool parent_ran          = false;
    bool valid_resolved      = false;
    bool id_preserved        = false;
    bool empty_resolved_null = false;
    int  child_inits         = 0;
    int  child_dtors         = 0;
};
class ConstChild : public qb::Actor {
    std::shared_ptr<Observation> _observation;

public:
    explicit ConstChild(std::shared_ptr<Observation> observation)
        : _observation(std::move(observation)) {}
    qb::io::async::task<bool>
    onInit() final {
        ++_observation->child_inits;
        co_return true;
    }
    ~ConstChild() override {
        ++_observation->child_dtors;
    }
};
class ConstParent : public qb::Actor {
    std::shared_ptr<Observation> _observation;

public:
    explicit ConstParent(std::shared_ptr<Observation> observation)
        : _observation(std::move(observation)) {}
    qb::io::async::task<bool>
    onInit() final {
        // QB-266 positive API control only. The invalid const store is established by
        // source review; this test does not manufacture read-only pages or rely on UB.
        const auto                        child = addRefActor<ConstChild>(_observation);
        const qb::ActorHandle<ConstChild> empty;
        _observation->parent_ran          = true;
        _observation->valid_resolved      = child.valid() && child.get() != nullptr && child.ready();
        _observation->id_preserved        = child.get() != nullptr && child.get()->id() == child.id();
        _observation->empty_resolved_null = empty.get() == nullptr && !empty.ready();
        if (auto *live = child.get())
            live->kill();
        kill();
        co_return true;
    }
};
TEST(AddReferencedActor, QB266ConstHandlesResolveActiveAndEmptyActors) {
    const auto observation = std::make_shared<Observation>();
    qb::Main   main;
    main.addActor<ConstParent>(0, observation);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(observation->parent_ran);
    EXPECT_TRUE(observation->valid_resolved);
    EXPECT_TRUE(observation->id_preserved);
    EXPECT_TRUE(observation->empty_resolved_null);
    EXPECT_EQ(observation->child_inits, 1);
    EXPECT_EQ(observation->child_dtors, 1);
}
} // namespace actor_add_const_handle_test

namespace actor_add_allocator_factory_test {
struct Probe;
class AllocatorParent;
} // namespace actor_add_allocator_factory_test

namespace qb {
// TActorFactory stores a decayed shared_ptr and passes its tuple element as an lvalue.
// Declare this exact specialization before any use can instantiate the default allocator.
template <>
actor_add_allocator_factory_test::AllocatorParent *
allocate_actor<actor_add_allocator_factory_test::AllocatorParent, std::shared_ptr<actor_add_allocator_factory_test::Probe> &>(
    std::shared_ptr<actor_add_allocator_factory_test::Probe> &probe);
} // namespace qb

namespace actor_add_allocator_factory_test {

enum class ChildRole : unsigned { service_constructor, refused_before_new, allocator_before_new };
struct Probe {
    std::array<qb::ActorId, 4> promised{};
    std::array<qb::ActorId, 4> actual{};
    std::array<int, 4>         ctors{};
    std::array<int, 4>         inits{};
    std::array<int, 4>         dtors{};
    std::array<int, 4>         deliveries{};
    std::array<int, 4>         acknowledgements{};
    std::array<qb::ActorId, 3> child_actual{};
    std::array<int, 3>         child_ctors{};
    std::array<int, 3>         child_inits{};
    std::array<int, 3>         child_dtors{};
    std::function<void()>      before_allocate;
    bool                       attempt_failure                     = false;
    bool                       false_admission_refused             = false;
    bool                       healthy_child_resolved              = false;
    bool                       service_child_resolved              = false;
    bool                       allocator_saw_live_host             = false;
    bool                       allocator_before_target_constructor = false;
    bool                       factory_bindings_observed           = false;
    bool                       service_lookup_bound                = false;
    bool                       guard_exhausted                     = false;
    int                        allocation_calls                    = 0;
    int                        wrong_delivery                      = 0;
    int                        sequence                            = 0;
    int                        service_constructor_sequence        = 0;
    int                        allocator_begin_sequence            = 0;
    int                        allocator_end_sequence              = 0;
    int                        target_constructor_sequence         = 0;
};
struct Poke : qb::Event {
    unsigned expected;
    explicit Poke(unsigned role)
        : expected(role) {}
};
struct Ack : qb::Event {
    unsigned role;
    explicit Ack(unsigned value)
        : role(value) {}
};

class NestedChild : public qb::Actor {
    std::shared_ptr<Probe> _probe;
    unsigned               _role;

public:
    NestedChild(std::shared_ptr<Probe> probe, ChildRole role)
        : _probe(std::move(probe))
        , _role(static_cast<unsigned>(role)) {
        ++_probe->child_ctors[_role];
        _probe->child_actual[_role] = id();
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->child_inits[_role];
        co_return _role != static_cast<unsigned>(ChildRole::refused_before_new);
    }
    ~NestedChild() override {
        ++_probe->child_dtors[_role];
    }
};

struct StartupServiceTag {};
class StartupService : public qb::ServiceActor<StartupServiceTag> {
    std::shared_ptr<Probe> _probe;

public:
    explicit StartupService(std::shared_ptr<Probe> probe)
        : _probe(std::move(probe)) {
        ++_probe->ctors[1];
        _probe->actual[1]                    = id();
        _probe->service_constructor_sequence = ++_probe->sequence;
        // A service factory is interleaved with ordinary promises. This ordinary
        // child must not consume any later startup factory's promised identity.
        auto child                     = addRefActor<NestedChild>(_probe, ChildRole::service_constructor);
        _probe->service_child_resolved = child.valid() && child.get() != nullptr;
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->inits[1];
        registerEvent<Poke>(*this);
        co_return true;
    }
    void
    on(Poke const &event) {
        ++_probe->deliveries[1];
        _probe->wrong_delivery += event.expected != 1u;
        push<Ack>(_probe->actual[0], 1u);
    }
    ~StartupService() override {
        ++_probe->dtors[1];
    }
};

class AllocatorParent : public qb::Actor {
    std::shared_ptr<Probe> _probe;

public:
    explicit AllocatorParent(std::shared_ptr<Probe> probe)
        : _probe(std::move(probe)) {
        ++_probe->ctors[2];
        _probe->actual[2]                   = id();
        _probe->target_constructor_sequence = ++_probe->sequence;
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->inits[2];
        registerEvent<Poke>(*this);
        co_return true;
    }
    void
    on(Poke const &event) {
        ++_probe->deliveries[2];
        _probe->wrong_delivery += event.expected != 2u;
        push<Ack>(_probe->actual[0], 2u);
    }
    ~AllocatorParent() override {
        ++_probe->dtors[2];
    }
};

class StartupPeer : public qb::Actor {
    std::shared_ptr<Probe> _probe;

public:
    explicit StartupPeer(std::shared_ptr<Probe> probe)
        : _probe(std::move(probe)) {
        ++_probe->ctors[3];
        _probe->actual[3] = id();
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->inits[3];
        registerEvent<Poke>(*this);
        co_return true;
    }
    void
    on(Poke const &event) {
        ++_probe->deliveries[3];
        _probe->wrong_delivery += event.expected != 3u;
        push<Ack>(_probe->actual[0], 3u);
    }
    ~StartupPeer() override {
        ++_probe->dtors[3];
    }
};

class StartupHost
    : public qb::Actor
    , public qb::ICallback {
    std::shared_ptr<Probe> _probe;
    unsigned               _ticks = 0;

public:
    explicit StartupHost(std::shared_ptr<Probe> probe)
        : _probe(std::move(probe)) {
        ++_probe->ctors[0];
        _probe->actual[0]               = id();
        const std::weak_ptr<Probe> weak = _probe;
        // The host is already constructed and owned by the core when the later
        // factory invokes this closure. It contains no suspended coroutine.
        _probe->before_allocate = [this, weak] {
            const auto observations = weak.lock();
            if (!observations || observations->dtors[0] != 0)
                return;
            observations->allocator_saw_live_host = true;
            if (observations->attempt_failure) {
                auto refused                          = addRefActor<NestedChild>(observations, ChildRole::refused_before_new);
                observations->false_admission_refused = !refused.valid();
            }
            auto child                           = addRefActor<NestedChild>(observations, ChildRole::allocator_before_new);
            observations->healthy_child_resolved = child.valid() && child.get() != nullptr;
        };
    }
    qb::io::async::task<bool>
    onInit() final {
        ++_probe->inits[0];
        registerEvent<Poke>(*this);
        registerEvent<Ack>(*this);
        registerCallback(*this);
        for (unsigned role = 0; role != _probe->promised.size(); ++role)
            push<Poke>(_probe->promised[role], role);
        co_return true;
    }
    void
    on(Poke const &event) {
        ++_probe->deliveries[0];
        _probe->wrong_delivery += event.expected != 0u;
        push<Ack>(_probe->actual[0], 0u);
    }
    void
    on(Ack const &event) {
        if (event.role < _probe->acknowledgements.size())
            ++_probe->acknowledgements[event.role];
        else
            ++_probe->wrong_delivery;
    }
    void
    on(qb::LoopEvent const &) final {
        bool all_received = true;
        for (unsigned role = 0; role != _probe->promised.size(); ++role)
            all_received &= _probe->acknowledgements[role] == 1 && _probe->inits[role] == 1;
        if (all_received) {
            _probe->factory_bindings_observed = true;
            auto *service                     = getService<StartupService>();
            _probe->service_lookup_bound      = service != nullptr && service->id() == _probe->promised[1];
        } else if (++_ticks < 8) {
            return;
        } else {
            _probe->guard_exhausted = true;
        }
        broadcast<qb::KillEvent>();
        kill();
    }
    ~StartupHost() override {
        _probe->before_allocate = {}; // no closure retains a stale host after teardown
        ++_probe->dtors[0];
    }
};
} // namespace actor_add_allocator_factory_test

namespace qb {
template <>
actor_add_allocator_factory_test::AllocatorParent *
allocate_actor<actor_add_allocator_factory_test::AllocatorParent, std::shared_ptr<actor_add_allocator_factory_test::Probe> &>(
    std::shared_ptr<actor_add_allocator_factory_test::Probe> &probe) {
    ++probe->allocation_calls;
    probe->allocator_begin_sequence            = ++probe->sequence;
    probe->allocator_before_target_constructor = probe->ctors[2] == 0;
    if (probe->before_allocate)
        probe->before_allocate(); // nested admission precedes the target Actor base constructor
    probe->allocator_end_sequence = ++probe->sequence;
    // Ordinary new/delete obey the customization point's default-deleter contract.
    return new actor_add_allocator_factory_test::AllocatorParent(probe);
}
} // namespace qb

namespace actor_add_allocator_factory_test {
void
check_allocator_factory_binding(bool attempt_failure) {
    auto probe             = std::make_shared<Probe>();
    probe->attempt_failure = attempt_failure;
    qb::Main main;
    main.core(0).setLatency(qb::duration::zero());
    // Keep this order: ordinary host, service, specialized allocator parent, ordinary peer.
    // Registration returns the promises before any user constructor/allocation hook runs.
    probe->promised[0] = main.addActor<StartupHost>(0, probe);
    probe->promised[1] = main.addActor<StartupService>(0, probe);
    probe->promised[2] = main.addActor<AllocatorParent>(0, probe);
    probe->promised[3] = main.addActor<StartupPeer>(0, probe);
    for (auto promised : probe->promised) {
        ASSERT_TRUE(promised.is_valid());
    }
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(probe->allocation_calls, 1);
    EXPECT_TRUE(probe->allocator_saw_live_host);
    EXPECT_TRUE(probe->allocator_before_target_constructor);
    EXPECT_LT(probe->service_constructor_sequence, probe->allocator_begin_sequence);
    EXPECT_LT(probe->allocator_begin_sequence, probe->allocator_end_sequence);
    EXPECT_LT(probe->allocator_end_sequence, probe->target_constructor_sequence);
    EXPECT_EQ(probe->false_admission_refused, attempt_failure);
    EXPECT_TRUE(probe->service_child_resolved);
    EXPECT_TRUE(probe->healthy_child_resolved);
    EXPECT_TRUE(probe->factory_bindings_observed);
    EXPECT_TRUE(probe->service_lookup_bound);
    EXPECT_FALSE(probe->guard_exhausted);
    EXPECT_EQ(probe->wrong_delivery, 0);
    EXPECT_FALSE(static_cast<bool>(probe->before_allocate));
    for (unsigned role = 0; role != probe->promised.size(); ++role) {
        SCOPED_TRACE(::testing::Message() << "startup role=" << role);
        EXPECT_EQ(probe->actual[role], probe->promised[role]);
        EXPECT_EQ(probe->ctors[role], 1);
        EXPECT_EQ(probe->inits[role], 1);
        EXPECT_EQ(probe->dtors[role], 1);
        EXPECT_EQ(probe->deliveries[role], 1);
        EXPECT_EQ(probe->acknowledgements[role], 1);
    }
    for (unsigned role = 0; role != probe->child_actual.size(); ++role) {
        const int expected = role == static_cast<unsigned>(ChildRole::refused_before_new) && !attempt_failure ? 0 : 1;
        SCOPED_TRACE(::testing::Message() << "nested role=" << role);
        EXPECT_EQ(probe->child_ctors[role], expected);
        EXPECT_EQ(probe->child_inits[role], expected);
        EXPECT_EQ(probe->child_dtors[role], expected);
        if (expected != 0) {
            EXPECT_TRUE(probe->child_actual[role].is_valid());
            for (auto promised : probe->promised) {
                EXPECT_NE(probe->child_actual[role], promised);
            }
            for (unsigned previous = 0; previous < role; ++previous) {
                if (probe->child_ctors[previous] != 0) {
                    EXPECT_NE(probe->child_actual[role], probe->child_actual[previous]);
                }
            }
        }
    }
}
TEST(AddActor, QB362AllocatorBeforeConstructorPreservesInterleavedServiceAndOrdinaryPromises) {
    check_allocator_factory_binding(false);
}
TEST(AddActor, QB362RefusedAdmissionInAllocatorPreservesLaterStartupBindings) {
    check_allocator_factory_binding(true);
}
} // namespace actor_add_allocator_factory_test
