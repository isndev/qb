#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/icallback.h>
#include <qb/main.h>
#include <atomic>
#include <thread>

// Finding #4 repro: an event type that NO actor anywhere subscribes to.
// Sending it must NOT kill the destination core — it should be dropped+warned.
struct NeverSubscribedEvent : qb::Event {};

struct ReproReceiver : qb::Actor {
    bool
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        return true;
    }
};

struct ReproSender : qb::Actor {
    qb::ActorId target;
    explicit ReproSender(qb::ActorId t)
        : target(t) {}
    bool
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        push<NeverSubscribedEvent>(target); // nobody handles this type
        return true;
    }
};

TEST(AuditRepro, UnregisteredEventTypeMustNotKillCore) {
    qb::Main engine;
    auto     rid = engine.addActor<ReproReceiver>(0);
    engine.addActor<ReproSender>(0, rid);
    std::thread stopper([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        qb::Main::stop();
    });
    engine.start(false); // this thread becomes the worker; returns on shutdown
    stopper.join();
    // BUG: the unregistered event throws std::out_of_range in router::memh::route's
    // onError disposer path; start_thread catches it and flags ExceptionThrown, so
    // hasError() becomes true (the core died). It must be false (event dropped).
    EXPECT_FALSE(engine.hasError()) << "destination core died after receiving an unregistered event type";
}

// Finding #C: an actor killed during the callback dispatch pass (by another
// actor's onCallback) must NOT receive another onCallback in that same pass.
static std::atomic<int> g_victim_ticks{0};

struct CbVictim
    : qb::Actor
    , qb::ICallback {
    bool
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);
        return true;
    }
    void
    onCallback() override {
        g_victim_ticks.fetch_add(1, std::memory_order_relaxed);
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

struct CbKiller
    : qb::Actor
    , qb::ICallback {
    qb::RefActorHandle<CbVictim> victim;
    bool
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);           // killer registers first => earlier in list
        victim = addRefHandle<CbVictim>(); // victim registers its callback after
        return true;
    }
    void
    onCallback() override {
        // First tick: synchronously kill the victim (which is later in the
        // callback snapshot). get() returns nullptr once it is dead, so this
        // fires exactly once.
        if (auto *v = victim.get())
            v->kill();
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

TEST(AuditRepro, KilledActorGetsNoFurtherCallbackInSamePass) {
    g_victim_ticks = 0;
    qb::Main engine;
    engine.addActor<CbKiller>(0);
    std::thread stopper([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        qb::Main::stop();
    });
    engine.start(false);
    stopper.join();
    // BUG: victim's onCallback ran once after it was killed (it was still in the
    // already-copied snapshot). Fixed: a killed actor is skipped in the loop.
    EXPECT_EQ(g_victim_ticks.load(), 0) << "victim ticked after being killed in the same callback pass";
}
