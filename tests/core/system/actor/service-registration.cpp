/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/actor/service-registration.cpp
 * @brief When a service tag registers, which id its readers see, and when a registration is refused (Huly QB-1007).
 *
 * A tag has a logical service address; each service instance has its own nonzero generation. The fixed service
 * band is disjoint from ordinary slots, including tags registered after a factory promised an ordinary ID. Pinned here:
 *   - `ServiceActor<Tag>::ServiceIndex` is 0 until its dynamic initialiser has run, and that initialiser is UNORDERED
 *     (an implicitly instantiated template static). No reader of a service id may load it: an `addActor<Service>`
 *     run from another global's initialiser, and the three runtime readers -- the service's constructor,
 *     `addRefActor<Service>()` and `getService<Service>()` -- all use the registered tag and core, with an instance ID.
 *   - Naming a tag registers it at static initialisation, before any core exists: a tag only ever named through
 *     `getServiceId<Tag>()`, from inside a running actor, does not draw the id of an ordinary actor of that core.
 *   - A tag whose FIRST registration comes while a core is alive is refused -- NotFound, a CRIT log and, in a debug
 *     build, an assert -- and a first registration is accepted again once every core is gone.
 *   - A tag registered between the first `core(index)` and `start()` leaves ordinary promises unchanged and can
 *     coexist with those actors. This does not permit first registration while a VirtualCore is alive.
 *
 * Two tools make the order deterministic on every compiler. An EXPLICIT specialization of `ServiceIndex` has ordered
 * initialisation ([basic.start.dynamic]): defined after a global, it is initialised after that global, whereas the
 * shipped, implicitly instantiated definition is unordered and its place is the compiler's choice. And an explicit
 * specialization defined as `0` holds a tag in the state "its static initialiser has not run" for the whole process:
 * the window a reader meets from another global's initialiser, or the state of a tag an image loaded mid-run brings.
 *
 * Every in-actor observation is mirrored to a process-global atom asserted after `join()`; the engines run on the
 * calling thread (`start(false)`, one core) and end by themselves.
 */

#include <atomic>
#include <cstdint>
#include <utility>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace service_registration_test {

std::uint64_t
raw(qb::ActorId const id) {
    return id.raw64();
}

bool
instance_of(qb::ActorId const instance, qb::ActorId const address) {
    return instance.is_service_instance() && address.is_service_address() && instance.service_tag() == address.service_tag()
           && instance.index() == address.index();
}

struct Poke : qb::Event {};

// ---------------------------------------------------------------------------------------------------------------
// 1. A service added from a global initialiser, before its tag's ServiceIndex is initialised.
// ---------------------------------------------------------------------------------------------------------------
struct EarlyTag {};
struct EarlyIds {
    qb::ActorId core0;
    qb::ActorId core1;
};
// Defined below `ServiceActor<EarlyTag>::ServiceIndex`, so that nothing names that member before it is declared.
EarlyIds add_early_service_from_a_global_initialiser();
// Ordered initialisation, first in this file: it runs BEFORE the initialiser of `ServiceIndex<EarlyTag>` below.
EarlyIds const g_early = add_early_service_from_a_global_initialiser();

} // namespace service_registration_test

namespace qb {
// An explicit specialization, so ORDERED and defined after `g_early`: initialised after it. Its initialiser is the
// shipped one, `registerIndex<Tag>()`, reached through the public `getServiceId<Tag>()`.
template <>
const ServiceId ServiceActor<service_registration_test::EarlyTag>::ServiceIndex =
    Actor::getServiceId<service_registration_test::EarlyTag>(0).service_tag();
} // namespace qb

namespace service_registration_test {

class EarlyService : public qb::ServiceActor<EarlyTag> {
public:
    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

EarlyIds
add_early_service_from_a_global_initialiser() {
    // A whole `qb::Main`, configured and dropped inside a static initialiser. Nothing is started, so the only reader
    // of a service id that runs is the reservation, `CoreInitializer::addActor<Service>`.
    qb::Main main;
    EarlyIds ids;
    ids.core0 = main.addActor<EarlyService>(0);
    ids.core1 = main.addActor<EarlyService>(1);
    return ids;
}

// ---------------------------------------------------------------------------------------------------------------
// 2. The runtime readers, with the tag registered but its ServiceIndex never initialised.
// ---------------------------------------------------------------------------------------------------------------
struct PinnedTag {};

} // namespace service_registration_test

namespace qb {
// Defined as 0: this tag's ServiceIndex stays in the state "its dynamic initialiser has not run" for good.
template <>
const ServiceId ServiceActor<service_registration_test::PinnedTag>::ServiceIndex = 0;
} // namespace qb

namespace service_registration_test {

// The tag registers here, at static initialisation like every tag, only not through its ServiceIndex.
qb::ActorId const g_pinned_registered = qb::Actor::getServiceId<PinnedTag>(0);

std::atomic<int>           g_pinned_constructed{0};
std::atomic<int>           g_pinned_poked{0};
std::atomic<std::uint64_t> g_pinned_instance_id{0};

class PinnedService : public qb::ServiceActor<PinnedTag> {
public:
    PinnedService() {
        g_pinned_constructed.fetch_add(1);
        g_pinned_instance_id.store(raw(id()));
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Poke>(*this);
        co_return true;
    }

    void
    on(Poke const &) {
        g_pinned_poked.fetch_add(1);
    }
};

std::atomic<bool> g_rt_ran{false};
std::atomic<bool> g_rt_handle_valid{false};
std::atomic<bool> g_rt_id_matches{false};
std::atomic<bool> g_rt_found{false};
std::atomic<bool> g_rt_duplicate_refused{false};
std::atomic<int>  g_rt_constructed{-1};

// addRefActor<Service>() reads the id twice -- to refuse a duplicate before building it, and in the service's own
// constructor -- and getService<Service>() once.
class PinnedHost : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        qb::ActorId const expected = qb::Actor::getServiceId<PinnedTag>(getIndex());
        auto              handle   = addRefActor<PinnedService>();
        g_rt_handle_valid.store(handle.valid());
        g_rt_id_matches.store(handle.valid() && instance_of(handle.id(), expected));
        auto *const found = getService<PinnedService>();
        g_rt_found.store(found != nullptr && found == handle.get());
        auto duplicate = addRefActor<PinnedService>();
        g_rt_duplicate_refused.store(!duplicate.valid());
        g_rt_constructed.store(g_pinned_constructed.load());
        push<Poke>(expected);                             // the registered id reaches the service...
        push<qb::KillEvent>(qb::BroadcastId(getIndex())); // ...and the run ends whatever happened above
        g_rt_ran.store(true);
        co_return true;
    }
};

std::atomic<bool> g_init_found{false};

// Runs on the core where `CoreInitializer::addActor<PinnedService>` reserved the service before the start.
class PinnedPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        qb::ActorId const expected = qb::Actor::getServiceId<PinnedTag>(getIndex());
        auto *const       found    = getService<PinnedService>();
        g_init_found.store(found != nullptr && instance_of(found->id(), expected));
        push<Poke>(expected);
        push<qb::KillEvent>(qb::BroadcastId(getIndex()));
        co_return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// 3. A tag only ever named through getServiceId<Tag>(), first from inside a running actor.
// ---------------------------------------------------------------------------------------------------------------
struct PhantomTag {}; // no ServiceActor<PhantomTag> is ever built anywhere

std::atomic<std::uint64_t> g_phantom_id{0};
std::atomic<std::uint64_t> g_phantom_probe_id{0};

class PhantomProbe : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_phantom_probe_id.store(raw(id())); // the first ordinary actor of the core: the pool's lowest id
        g_phantom_id.store(raw(qb::Actor::getServiceId<PhantomTag>(getIndex())));
        kill();
        co_return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// 4. A tag whose first registration comes while a core is alive, and the control: once the cores are gone.
// ---------------------------------------------------------------------------------------------------------------
struct LateTag {};
struct FreshTag {};

} // namespace service_registration_test

namespace qb {
// Neither tag registers at static initialisation: the state of a tag that an image loaded mid-run brings.
template <>
const ServiceId ServiceActor<service_registration_test::LateTag>::ServiceIndex = 0;
template <>
const ServiceId ServiceActor<service_registration_test::FreshTag>::ServiceIndex = 0;
} // namespace qb

namespace service_registration_test {

std::atomic<int> g_late_constructed{0};

class LateService : public qb::ServiceActor<LateTag> {
public:
    LateService() {
        g_late_constructed.fetch_add(1);
    }

    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

std::atomic<std::uint64_t> g_late_id{0};
std::atomic<std::uint64_t> g_late_probe_id{0};
std::atomic<bool>          g_late_ref_valid{true};

class LateProbe : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_late_probe_id.store(raw(id()));                                   // the first ordinary actor of the core: the pool's lowest id
        g_late_id.store(raw(qb::Actor::getServiceId<LateTag>(getIndex()))); // LateTag's FIRST registration
        g_late_ref_valid.store(addRefActor<LateService>().valid());
        kill();
        co_return true;
    }
};

void
run_a_registration_on_a_live_core() {
    qb::Main main;
    main.addActor<LateProbe>(0);
    main.start(false);
    main.join();
}

class IdleProbe : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// 5. A tag registered between the first core(index) and start().
// ---------------------------------------------------------------------------------------------------------------
struct GapTag {};
struct QuietTag {};

} // namespace service_registration_test

namespace qb {
// Neither registers at static initialisation: each test registers its tag after `core(0)` was built.
template <>
const ServiceId ServiceActor<service_registration_test::GapTag>::ServiceIndex = 0;
template <>
const ServiceId ServiceActor<service_registration_test::QuietTag>::ServiceIndex = 0;
} // namespace qb

namespace service_registration_test {

std::atomic<std::uint64_t> g_gap_ran_as{0};
std::atomic<std::uint64_t> g_gap_service_ran_as{0};
std::atomic<int>           g_gap_service_poked{0};

class GapService : public qb::ServiceActor<GapTag> {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_gap_service_ran_as.store(raw(id()));
        registerEvent<Poke>(*this);
        co_return true;
    }
    void
    on(Poke const &) {
        ++g_gap_service_poked;
        kill();
    }
};

class GapProbe : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_gap_ran_as.store(raw(id()));
        push<Poke>(qb::Actor::getServiceId<GapTag>(getIndex()));
        kill();
        co_return true;
    }
};

struct QuietServiceTag {}; // an ordinary tag: registered at static initialisation

std::atomic<bool>          g_quiet_ran{false};
std::atomic<std::uint64_t> g_quiet_child_id{0};

class QuietChild : public qb::Actor {};

class QuietService : public qb::ServiceActor<QuietServiceTag> {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_quiet_child_id.store(raw(addRefActor<QuietChild>().id()));
        g_quiet_ran.store(true);
        push<qb::KillEvent>(qb::BroadcastId(getIndex()));
        co_return true;
    }
};

} // namespace service_registration_test

using namespace service_registration_test;

TEST(ServiceRegistration, AServiceAddedFromAGlobalInitialiserGetsItsRegisteredId) {
    // On a reader of ServiceIndex, both reservations read 0: NotFound on core 0, and `(0, 1)` on core 1 -- an id
    // no actor holds, which `is_valid()` accepted until 3.3.
    EXPECT_TRUE(g_early.core0.is_valid()) << "the reservation made from a global initialiser came back NotFound";
    EXPECT_TRUE(instance_of(g_early.core0, qb::Actor::getServiceId<EarlyTag>(0)));
    EXPECT_TRUE(instance_of(g_early.core1, qb::Actor::getServiceId<EarlyTag>(1)));
    EXPECT_NE(g_early.core1.service_tag(), qb::ServiceId{0});
    EXPECT_NE(raw(g_early.core0), raw(qb::Actor::getServiceId<EarlyTag>(0)));
}

TEST(ServiceRegistration, TheRuntimeReadersUseTheRegisteredIdNotServiceIndex) {
    ASSERT_TRUE(g_pinned_registered.is_service_address());
    g_pinned_constructed.store(0);
    g_pinned_poked.store(0);
    g_pinned_instance_id.store(0);
    g_rt_ran.store(false);
    qb::Main main;
    main.addActor<PinnedHost>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(g_rt_ran.load()) << "the host's onInit never ran";
    EXPECT_TRUE(g_rt_handle_valid.load()) << "addRefActor<Service>() refused a service whose tag is registered";
    EXPECT_TRUE(g_rt_id_matches.load()) << "the service instance has the wrong registered tag or core";
    EXPECT_TRUE(g_rt_found.load()) << "getService<Service>() did not find the service";
    EXPECT_TRUE(g_rt_duplicate_refused.load()) << "a second addRefActor<Service>() was accepted";
    EXPECT_EQ(g_rt_constructed.load(), 1) << "the duplicate was constructed before it was refused";
    EXPECT_EQ(g_pinned_poked.load(), 1) << "an event sent to getServiceId<Tag>() did not reach the service";
}

TEST(ServiceRegistration, AServiceReservedBeforeTheStartRunsUnderItsRegisteredId) {
    ASSERT_TRUE(g_pinned_registered.is_service_address());
    g_pinned_constructed.store(0);
    g_pinned_poked.store(0);
    g_pinned_instance_id.store(0);
    g_init_found.store(false);
    qb::Main          main;
    qb::ActorId const reserved = main.addActor<PinnedService>(0);
    EXPECT_TRUE(instance_of(reserved, qb::Actor::getServiceId<PinnedTag>(0))) << "the reservation read ServiceIndex";
    main.addActor<PinnedPeer>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError()) << "the service's constructor gave it an id the core refused";
    EXPECT_EQ(g_pinned_constructed.load(), 1);
    EXPECT_TRUE(g_init_found.load()) << "getService<Service>() did not find the registered tag and core";
    EXPECT_EQ(g_pinned_instance_id.load(), raw(reserved)) << "the actual instance must retain its pre-start promise";
    EXPECT_EQ(g_pinned_poked.load(), 1) << "an event sent to getServiceId<Tag>() did not reach the service";
}

TEST(ServiceRegistration, ATagNamedOnlyFromARunningActorWasRegisteredBeforeTheCores) {
    g_phantom_id.store(0);
    g_phantom_probe_id.store(0);
    qb::Main main;
    main.addActor<PhantomProbe>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    qb::ActorId const phantom = qb::ActorId::from_raw(g_phantom_id.load());
    qb::ActorId const probe   = qb::ActorId::from_raw(g_phantom_probe_id.load());
    ASSERT_TRUE(probe.is_valid()) << "the probe never ran";
    EXPECT_TRUE(phantom.is_service_address());
    EXPECT_TRUE(probe.is_ordinary());
    EXPECT_NE(raw(phantom), raw(probe)) << "the tag drew the id of the core's first ordinary actor";
    EXPECT_GT(phantom.slot(), 0xff0000u);
    EXPECT_LE(probe.slot(), 0xfeffffu) << "the ordinary actor and logical service occupy disjoint bands";
}

TEST(ServiceRegistrationDeathTest, ATagFirstRegisteredWhileACoreIsAliveIsRefused) {
    // The statement runs a whole engine, so the child re-executes rather than forking a process with threads.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    g_late_constructed.store(0);
    g_late_id.store(0);
    g_late_probe_id.store(0);
    g_late_ref_valid.store(true);
    // Debug: the refusal asserts, naming why. Release: the statement runs here, and the refusal is its result.
    EXPECT_DEBUG_DEATH(run_a_registration_on_a_live_core(), "registered while a VirtualCore is alive");
#ifdef NDEBUG
    qb::ActorId const late  = qb::ActorId::from_raw(g_late_id.load());
    qb::ActorId const probe = qb::ActorId::from_raw(g_late_probe_id.load());
    ASSERT_TRUE(probe.is_valid()) << "the probe never ran";
    EXPECT_FALSE(late.is_valid()) << "a first registration on a live core was accepted";
    EXPECT_NE(raw(late), raw(probe)) << "the late tag drew the id of the core's first ordinary actor";
    EXPECT_FALSE(g_late_ref_valid.load()) << "addRefActor<Service>() of a refused tag returned a valid handle";
    EXPECT_EQ(g_late_constructed.load(), 0) << "the service of a refused tag was constructed";
#endif
}

TEST(ServiceRegistration, AFirstRegistrationIsAcceptedOnceEveryCoreIsGone) {
    // The control of the refusal above: it lasts as long as the cores, not as long as the process.
    {
        qb::Main main;
        main.addActor<IdleProbe>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    qb::ActorId const fresh = qb::Actor::getServiceId<FreshTag>(0);
    EXPECT_TRUE(fresh.is_service_address()) << "a first registration with no core alive was refused";
    EXPECT_EQ(raw(qb::Actor::getServiceId<FreshTag>(0)), raw(fresh)) << "a registered id must not move";
}

TEST(ServiceRegistration, ATagRegisteredBeforeStartCoexistsWithoutChangingAnOrdinaryPromise) {
    // The scenario is GapTag's FIRST registration, and a tag registers once per process: a repetition in the same
    // process (--gtest_repeat) cannot rebuild it, and says so instead of failing its setup. No other test names GapTag,
    // so the order of a shuffled run does not matter -- only a second pass over this one does.
    static bool tag_spent = false;
    if (std::exchange(tag_spent, true)) {
        GTEST_SKIP() << "GapTag registered in an earlier repetition of this process: its first registration happens once";
    }
    g_gap_ran_as.store(0);
    g_gap_service_ran_as.store(0);
    g_gap_service_poked.store(0);
    qb::Main          main;
    qb::ActorId const promised = main.addActor<GapProbe>(0);         // core(0) is built here, and promises the probe an id
    qb::ActorId const gap      = qb::Actor::getServiceId<GapTag>(0); // GapTag registers now: no core is alive yet
    const auto        service  = main.addActor<GapService>(0);
    ASSERT_TRUE(promised.is_ordinary());
    ASSERT_TRUE(gap.is_service_address());
    ASSERT_TRUE(instance_of(service, gap));
    EXPECT_LE(promised.slot(), 0xfeffffu);
    EXPECT_GT(gap.slot(), 0xff0000u);
    EXPECT_NE(raw(gap), raw(promised));
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_gap_ran_as.load(), raw(promised)) << "registration must preserve the exact ordinary promise";
    EXPECT_EQ(g_gap_service_ran_as.load(), raw(service)) << "the service must also retain its exact instance promise";
    EXPECT_EQ(g_gap_service_poked.load(), 1) << "the newly registered logical address must reach its coexisting service";
}

TEST(ServiceRegistration, ACoreThatPromisedNoOrdinaryIdStartsAfterALateRegistration) {
    // The control: a service-only factory list also remains valid when another logical tag registers before start.
    // Like the case above, it needs QuietTag's first registration: once per process.
    static bool tag_spent = false;
    if (std::exchange(tag_spent, true)) {
        GTEST_SKIP() << "QuietTag registered in an earlier repetition of this process: its first registration happens once";
    }
    g_quiet_ran.store(false);
    g_quiet_child_id.store(0);
    qb::Main main;
    main.addActor<QuietService>(0);
    qb::ActorId const quiet = qb::Actor::getServiceId<QuietTag>(0);
    ASSERT_TRUE(quiet.is_valid());
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_quiet_ran.load());
    qb::ActorId const child = qb::ActorId::from_raw(g_quiet_child_id.load());
    EXPECT_TRUE(child.is_ordinary()) << "the service's child was refused";
    EXPECT_TRUE(quiet.is_service_address());
    EXPECT_LE(child.slot(), 0xfeffffu);
    EXPECT_GT(quiet.slot(), 0xff0000u);
    EXPECT_NE(raw(child), raw(quiet)) << "the new tag must not alias the service's ordinary child";
}
