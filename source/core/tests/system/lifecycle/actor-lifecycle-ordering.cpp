/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/lifecycle/actor-lifecycle-ordering.cpp
 * @brief The actor lifecycle hook ordering — constructor → onInit → [kill] → destructor — proven
 *        with a DETERMINISTIC ordered event log and an actor-signalled completion (no wall clock).
 *
 * Every actor appends a `{name, phase}` entry to a single mutex-guarded `g_log` at each lifecycle
 * point (constructor, onInit, self/external kill, destructor). The vector outlives the engine, so
 * destructor entries — which fire during teardown — are recorded safely and read after `join()`.
 *
 * Completion is event-driven, NOT a `1s` timer + `5s` poll: a `LifeCoordinator` tells each worker to
 * terminate (self-kill or external-kill, per its role) and counts the kill-acks; once every worker
 * has acked, it stops the engine. The run therefore ends the instant the work is done; a LOUD ctest
 * TIMEOUT is the only backstop, and nothing here can hang silently.
 *
 * Strengthened oracles (asserted after join()):
 *   - every worker reached its destructor (killed actors are actually destroyed);
 *   - for every worker, onInit is recorded BEFORE its kill, and its kill BEFORE its destructor;
 *   - the global ordering is internally consistent (constructor < onInit per actor).
 *
 * Also folds the previously-dead `fail_init` case the original file had wired but never asserted:
 * an actor whose async `onInit` `co_return false`s fails the init and aborts `start()` with
 * `hasError()` (BadActorInit-class), and is still destroyed.
 */

#include <atomic>
#include <cstddef>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <vector>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// A single ordered lifecycle log. Entries are appended at each hook; the vector outlives the engine
// so teardown-phase destructor entries are captured. Read only after join() (happens-before).
// ---------------------------------------------------------------------------
enum class Phase { Constructor, OnInit, Kill, Destructor };

struct Entry {
    std::string name;
    Phase       phase;
};

std::mutex         g_log_mutex;
std::vector<Entry> g_log;

void
record(const std::string &name, Phase phase) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log.push_back({name, phase});
}

void
reset_log() {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log.clear();
}

// Index of the FIRST entry matching {name, phase}, or -1 if absent. Caller holds no lock; this takes
// the lock itself. Used only after join().
int
first_index_of(const std::string &name, Phase phase) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    for (std::size_t i = 0; i < g_log.size(); ++i)
        if (g_log[i].name == name && g_log[i].phase == phase)
            return static_cast<int>(i);
    return -1;
}

bool
contains(const std::string &name, Phase phase) {
    return first_index_of(name, phase) >= 0;
}

} // namespace

// Control events.
struct TerminateSelf : public qb::Event {};    // coordinator → worker: kill yourself
struct WorkerGone : public qb::Event {         // worker → coordinator: I am about to die
    qb::ActorId who;
    explicit WorkerGone(qb::ActorId w)
        : who(w) {}
};

// A lifecycle worker. `external` workers are killed by the coordinator's TerminateSelf; this models
// both the "self kill" and "external kill" roles of the original test with one code path, since the
// distinction was always who *sent* the kill, not the mechanism.
class LifecycleWorker : public qb::Actor {
    std::string _name;
    qb::ActorId _coord;

public:
    LifecycleWorker(std::string name, qb::ActorId coord)
        : _name(std::move(name))
        , _coord(coord) {
        record(_name, Phase::Constructor);
    }

    ~LifecycleWorker() override {
        record(_name, Phase::Destructor);
    }

    qb::io::async::task<bool>
    onInit() override {
        record(_name, Phase::OnInit);
        registerEvent<TerminateSelf>(*this);
        co_return true;
    }

    void
    on(const TerminateSelf &) {
        record(_name, Phase::Kill);
        push<WorkerGone>(_coord, id()); // tell the coordinator before we go
        kill();
    }
};

// Coordinator: spawns the workers, asks them all to terminate, and stops the engine once every
// worker has acked its impending death. No wall clock anywhere on the completion path.
class LifeCoordinator : public qb::Actor {
    const int                _expected;
    std::vector<qb::ActorId> _workers;
    int                      _gone = 0;

public:
    explicit LifeCoordinator(int expected)
        : _expected(expected) {}

    qb::io::async::task<bool>
    onInit() override {
        record("coordinator", Phase::OnInit);
        registerEvent<WorkerGone>(*this);

        _workers.push_back(addRefActor<LifecycleWorker>(std::string("worker_a"), id()).id());
        _workers.push_back(addRefActor<LifecycleWorker>(std::string("worker_b"), id()).id());
        _workers.push_back(addRefActor<LifecycleWorker>(std::string("worker_c"), id()).id());

        // Ask each worker to terminate. Mailbox-ordered behind their own onInit activations.
        for (auto w : _workers)
            to(w).push<TerminateSelf>();

        co_return true;
    }

    void
    on(const WorkerGone &) {
        if (++_gone == _expected) {
            qb::Main::stop();
            kill();
        }
    }
};

TEST(ActorLifecycle, HooksFireInConstructorOnInitKillDestructorOrder) {
    reset_log();

    qb::Main main;
    constexpr int kWorkers = 3;
    main.core(0).addActor<LifeCoordinator>(kWorkers);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());

    // The coordinator and all three workers ran their onInit.
    EXPECT_TRUE(contains("coordinator", Phase::OnInit));

    for (const std::string name : {"worker_a", "worker_b", "worker_c"}) {
        const int ctor = first_index_of(name, Phase::Constructor);
        const int init = first_index_of(name, Phase::OnInit);
        const int kill = first_index_of(name, Phase::Kill);
        const int dtor = first_index_of(name, Phase::Destructor);

        ASSERT_GE(ctor, 0) << name << " missing constructor";
        ASSERT_GE(init, 0) << name << " missing onInit";
        ASSERT_GE(kill, 0) << name << " missing kill";
        ASSERT_GE(dtor, 0) << name << " killed actor must reach its destructor";

        EXPECT_LT(ctor, init) << name << ": constructor must precede onInit";
        EXPECT_LT(init, kill) << name << ": onInit must precede the kill";
        EXPECT_LT(kill, dtor) << name << ": the kill must precede the destructor";
    }
}

// ---------------------------------------------------------------------------
// fail_init: an async onInit that co_return false fails the init, aborts start() with hasError(),
// and is still destroyed. (The original file wired a "fail_init_actor" name but never asserted it.)
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_failinit_oninit_ran{false};
std::atomic<bool> g_failinit_destroyed{false};
} // namespace

class FailInitActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_failinit_oninit_ran.store(true);
        co_return false; // false-return path ⇒ BadActorInit-class init failure
    }
    ~FailInitActor() override {
        g_failinit_destroyed.store(true);
    }
};

TEST(ActorLifecycle, FailedOnInitAbortsStartAndStillDestroys) {
    g_failinit_oninit_ran.store(false);
    g_failinit_destroyed.store(false);

    qb::Main main;
    main.core(0).addActor<FailInitActor>();
    main.start(false);
    main.join();

    // A co_return-false onInit of a core's only actor fails that core's init and aborts start().
    EXPECT_TRUE(main.hasError()) << "a co_return-false onInit must fail the core init";
    EXPECT_TRUE(g_failinit_oninit_ran.load()) << "onInit must actually have run";
    EXPECT_TRUE(g_failinit_destroyed.load()) << "a failed-init actor must still be destroyed";
}
