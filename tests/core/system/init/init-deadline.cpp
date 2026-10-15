/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-deadline.cpp
 * @brief The activation deadline — the deadlock-proofing that bounds a suspended `onInit()`.
 *
 * `VirtualCore::activation_deadline_ns` bounds the *Activating* window: an `onInit()` that does not
 * complete within it is failed and removed (its coroutine scope is cancelled so the frame unwinds).
 * This makes mutual-init deadlocks impossible and clamps no-timeout in-init asks. This file proves
 * the deadline force-fails three flavours of stuck init:
 *
 *   - a raw `co_await context().until_cancelled()` that never completes on its own;
 *   - an in-init `qb::ask` to a peer that is ITSELF permanently Activating (the request is stashed
 *     forever) — the asker's own deadline cancels the ask;
 *   - an in-init `qb::ask_retry` loop whose total retry budget exceeds the deadline — the deadline
 *     cancels the WHOLE loop, not just one attempt.
 *
 * ORDERING, NOT ABSOLUTE TIME: each stuck actor exposes a "completed naturally" flag that is set
 * ONLY if its long/forever wait finished on its own, and a "destroyed" flag set in its destructor.
 * The deadline (here 200-250 ms) is configured far below the natural completion (5 s / never), so
 * the assertions check the *ordering* invariant — the deadline cancelled BEFORE natural completion
 * (`destroyed == true && completed_naturally == false`) — rather than a fragile wall-clock bound.
 * A generous ctest TIMEOUT remains the only backstop; there is no tight `EXPECT_LT(elapsed, ...)`.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;
using qb::test::Cfg;
using qb::test::ScopedDeadline;

namespace init_deadline_test {

// ===========================================================================
// 1. A raw stuck init (parks forever) — only the deadline frees it.
// ===========================================================================
std::atomic<bool> g_stuck_started{false};
std::atomic<bool> g_stuck_completed_naturally{false};
std::atomic<bool> g_stuck_destroyed{false};

class StuckInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_stuck_started.store(true);
        co_await context().until_cancelled();    // parks forever — only the deadline cancels it
        g_stuck_completed_naturally.store(true); // must NOT happen (no natural completion)
        co_return true;
    }
    ~StuckInit() override {
        g_stuck_destroyed.store(true);
    }
};

TEST(InitDeadline, StuckInitForceFailedByDeadlineBeforeNaturalCompletion) {
    g_stuck_started.store(false);
    g_stuck_completed_naturally.store(false);
    g_stuck_destroyed.store(false);
    ScopedDeadline dl(200'000'000); // 200 ms — far below the (never) natural completion
    qb::Main       main;
    main.addActor<StuckInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_stuck_started.load());              // the init body ran up to its park
    EXPECT_FALSE(g_stuck_completed_naturally.load()); // ordering: it never completed on its own
    EXPECT_TRUE(g_stuck_destroyed.load());            // ...the deadline cancelled + removed it first
}

// ===========================================================================
// 2. An in-init ask to a peer that is permanently Activating — the request is
//    stashed forever; the asker's OWN deadline cancels the stuck ask.
// ===========================================================================
class StuckPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(5s); // far longer than the test deadline → stays Activating
        co_return true;
    }
};

std::atomic<bool> g_asker_completed_naturally{false};
std::atomic<bool> g_asker_destroyed{false};

class AsksStuckPeer : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksStuckPeer(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        // The peer is Activating → this request is stashed and never answered. The asker's OWN
        // activation deadline cancels the scope; the ask throws cancelled_error, which we
        // deliberately let propagate so the init FAILS (catching + co_return true would wrongly
        // succeed). This is the deadlock-proofing the deadline guarantees.
        (void) co_await qb::ask(context(), _peer, Cfg{1}, 5s);
        g_asker_completed_naturally.store(true); // unreachable — the deadline cancels first
        co_return true;
    }
    ~AsksStuckPeer() override {
        g_asker_destroyed.store(true);
    }
};

TEST(InitDeadline, AskToStuckActivatingPeerBrokenByDeadline) {
    g_asker_completed_naturally.store(false);
    g_asker_destroyed.store(false);
    ScopedDeadline dl(200'000'000); // 200 ms
    qb::Main       main;
    const auto     peer = main.addActor<StuckPeer>(0);
    main.addActor<AsksStuckPeer>(0, peer);
    main.start(false);
    main.join();
    EXPECT_FALSE(g_asker_completed_naturally.load()); // ordering: the ask never resolved
    EXPECT_TRUE(g_asker_destroyed.load());            // the deadline cancelled the stuck ask → removed
}

// ===========================================================================
// 3. An in-init ask_retry loop whose total budget exceeds the deadline — the
//    deadline cancels the WHOLE loop, not just one attempt.
// ===========================================================================
std::atomic<bool> g_retry_completed_naturally{false};
std::atomic<bool> g_retry_destroyed{false};

class AskRetryDeadlineInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AskRetryDeadlineInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        // The peer (StuckPeer) never answers; each attempt times out and retries — but the per-actor
        // activation deadline cancels the WHOLE loop, not just one attempt.
        (void) co_await qb::ask_retry(context(), _peer, Cfg{1}, 80ms, qb::retry_policy{.max_attempts = 5});
        g_retry_completed_naturally.store(true); // unreachable — the deadline cancels first
        co_return true;
    }
    ~AskRetryDeadlineInInit() override {
        g_retry_destroyed.store(true);
    }
};

TEST(InitDeadline, AskRetryLoopBrokenByActivationDeadline) {
    g_retry_completed_naturally.store(false);
    g_retry_destroyed.store(false);
    ScopedDeadline dl(250'000'000); // 250 ms < 5 x 80 ms of retries → the loop cannot finish in time
    qb::Main       main;
    const auto     peer = main.addActor<StuckPeer>(0);
    main.addActor<AskRetryDeadlineInInit>(0, peer);
    main.start(false);
    main.join();
    EXPECT_FALSE(g_retry_completed_naturally.load()); // ordering: the retry loop never resolved
    EXPECT_TRUE(g_retry_destroyed.load());            // the deadline cancelled the retry loop → removed
}

} // namespace init_deadline_test

// QB-363: deadline cancellation cannot be converted into a successful admission.
namespace init_deadline_admission_witness_test {

using namespace std::chrono_literals;

enum class Verdict { catch_true, catch_false, healthy };

struct State {
    std::atomic<int>        cancelled{0};
    std::atomic<int>        unwound{0};
    std::atomic<int>        destroyed{0};
    std::atomic<int>        constructed_payloads{0};
    std::atomic<int>        disposed_payloads{0};
    std::atomic<int>        delivered_payloads{0};
    std::atomic<int>        down_count{0};
    std::atomic<int>        down_reason{-1};
    std::atomic<bool>       natural_completion{false};
    std::atomic<bool>       ready_seen{false};
    std::atomic<bool>       ready_value{false};
    std::atomic<bool>       ready_wait_cancelled{false};
    std::atomic<bool>       done{false};
    std::atomic<bool>       watchdog_fired{false};
    std::mutex              mutex;
    std::condition_variable cv;
};

// Owns its thread and joins before State, Main or ScopedDeadline can leave scope.
// Latency-zero admission cases consume Main::stop on a later pass.
// stop() only advances a signal generation; it does not wake a parked core.
class StopWatchdog {
    std::shared_ptr<State> _state;
    std::thread            _thread;

public:
    explicit StopWatchdog(std::shared_ptr<State> state)
        : _state(std::move(state))
        , _thread([s = _state] {
            std::unique_lock lock(s->mutex);
            if (!s->cv.wait_for(lock, 3s, [&] { return s->done.load(); })) {
                s->watchdog_fired.store(true);
                lock.unlock();
                qb::Main::stop();
            }
        }) {}
    ~StopWatchdog() {
        {
            std::lock_guard lock(_state->mutex);
            _state->done.store(true);
        }
        _state->cv.notify_all();
        _thread.join();
    }
};

struct StashedPayload : qb::Event {
    std::shared_ptr<State> observations;
    explicit StashedPayload(std::shared_ptr<State> s)
        : observations(std::move(s)) {
        observations->constructed_payloads.fetch_add(1);
    }
    ~StashedPayload() {
        observations->disposed_payloads.fetch_add(1);
    }
};
struct ReadyObserved : qb::Event {};
struct FinishSubject : qb::Event {};
struct UnwindMark {
    std::shared_ptr<State> state;
    ~UnwindMark() {
        state->unwound.fetch_add(1);
    }
};

class Subject : public qb::Actor {
    std::shared_ptr<State> _state;
    Verdict                _verdict;

public:
    Subject(std::shared_ptr<State> state, Verdict verdict)
        : _state(std::move(state))
        , _verdict(verdict) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StashedPayload>(*this);
        registerEvent<FinishSubject>(*this);
        const auto state   = _state;
        const auto verdict = _verdict;
        auto       ctx     = context();
        UnwindMark frame{state};
        if (verdict == Verdict::healthy) {
            co_await ctx.sleep(1ms);
            state->natural_completion.store(true);
            co_return true;
        }
        try {
            co_await ctx.until_cancelled();
            state->natural_completion.store(true); // unexpected: awaiter must throw
        } catch (const qb::io::async::cancelled_error &) {
            state->cancelled.fetch_add(1); // actual cancellation observed, not a time guess
            co_return verdict == Verdict::catch_true;
        }
        co_return false;
    }
    void
    on(StashedPayload const &) {
        _state->delivered_payloads.fetch_add(1);
    }
    void
    on(FinishSubject const &) {
        kill();
    }
    ~Subject() override {
        _state->destroyed.fetch_add(1);
    }
};

// The observer is already Active while its child waits. Its own onInit does not
// suspend, so it has no activation deadline that could replace the child's oracle.
class Observer : public qb::Actor {
    qb::ActorId            _child_id = qb::ActorId::NotFound;
    std::shared_ptr<State> _state;
    Verdict                _verdict;
    void
    finish_if_observed() {
        if (_state->ready_seen.load() && _state->down_count.load() == 1) {
            {
                std::lock_guard lock(_state->mutex);
                _state->done.store(true);
            }
            _state->cv.notify_all();
            kill();
        }
    }

public:
    Observer(std::shared_ptr<State> state, Verdict verdict)
        : _state(std::move(state))
        , _verdict(verdict) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ReadyObserved>(*this);
        registerEvent<qb::DownEvent>(*this);
        auto child = addRefActor<Subject>(_state, _verdict);
        _child_id  = child.id();
        watch(child.id());
        push<StashedPayload>(child.id(), _state); // one owned nontrivial message, while Activating
        spawn([child, state = _state](qb::ScopedCoroContext ctx) mutable -> qb::io::async::task<void> {
            try {
                state->ready_value.store(co_await child.ready_async(ctx, 0ns));
                state->ready_seen.store(true);
                ctx.push<ReadyObserved>();
            } catch (const qb::io::async::cancelled_error &) {
                state->ready_wait_cancelled.store(true);
            }
        });
        co_return true;
    }
    void
    on(ReadyObserved const &) {
        // Keep healthy admission alive until the readiness verdict is consumed.
        // Also bound the defective catch-true path without replay killing first.
        if (_state->ready_value.load())
            push<FinishSubject>(_child_id);
        finish_if_observed();
    }
    void
    on(qb::DownEvent const &event) {
        _state->down_count.fetch_add(1);
        _state->down_reason.store(static_cast<int>(event.reason));
        finish_if_observed();
    }
};

void
run_admission_witness(Verdict verdict) {
    const auto     state = std::make_shared<State>();
    ScopedDeadline deadline(200'000'000); // 200 ms; latency stays zero in these cases
    qb::Main       main;
    main.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity});
    main.addActor<Observer>(0, state, verdict);
    {
        StopWatchdog watchdog(state);
        main.start();
        main.join();
    }
    const bool healthy = verdict == Verdict::healthy;
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(state->watchdog_fired.load());
    EXPECT_FALSE(state->ready_wait_cancelled.load());
    EXPECT_TRUE(state->ready_seen.load());
    EXPECT_EQ(state->ready_value.load(), healthy); // phase/admission oracle
    EXPECT_EQ(state->cancelled.load(), healthy ? 0 : 1);
    EXPECT_EQ(state->natural_completion.load(), healthy);
    EXPECT_EQ(state->unwound.load(), 1);
    EXPECT_EQ(state->destroyed.load(), 1);
    EXPECT_EQ(state->constructed_payloads.load(), 1);
    EXPECT_EQ(state->disposed_payloads.load(), 1); // replay/discard both dispose once
    EXPECT_EQ(state->delivered_payloads.load(), healthy ? 1 : 0);
    EXPECT_EQ(state->down_count.load(), 1);
    EXPECT_EQ(state->down_reason.load(), static_cast<int>(healthy ? qb::DownReason::killed : qb::DownReason::init_failed));
}

TEST(InitDeadlineAdmissionWitness, CatchCancellationThenTrueMustRemainFailed) {
    run_admission_witness(Verdict::catch_true);
}
TEST(InitDeadlineAdmissionWitness, CatchCancellationThenFalseFails) {
    run_admission_witness(Verdict::catch_false);
}
TEST(InitDeadlineAdmissionWitness, HealthyAsyncCompletionBeforeDeadlineIsAdmitted) {
    run_admission_witness(Verdict::healthy);
}

} // namespace init_deadline_admission_witness_test

// QB-265: activation deadline must cap both mailbox and I/O park.
namespace init_deadline_park_witness_test {
using namespace std::chrono_literals;
using namespace qb::io;
constexpr qb::duration kDeadline = 200ms;
constexpr qb::duration kLatency  = 8s;
// 200 ms < 500 ms < qev's 743 ms idle fallback: that fallback must not mask a missing cap.
constexpr qb::duration kAdmissionBound = 500ms;
constexpr qb::duration kWatchdogBudget = kDeadline * 10; // 2 s, below latency
static_assert(kDeadline < kAdmissionBound && kAdmissionBound < kWatchdogBudget && kWatchdogBudget < kLatency);

std::int64_t
now_ns() {
    return qb::mono_now().time_since_epoch().count();
}
std::atomic<std::uint16_t> g_port{0};

struct State {
    std::atomic<std::int64_t> started_ns{0};
    std::atomic<std::int64_t> cancelled_ns{0};
    std::atomic<std::int64_t> destroyed_ns{0};
    std::atomic<int>          cancelled{0};
    std::atomic<int>          unwound{0};
    std::atomic<int>          destroyed{0};
    std::atomic<int>          down_count{0};
    std::atomic<int>          down_reason{-1};
    std::atomic<int>          request_armed_passes{0};
    std::atomic<int>          max_watchers{0};
    std::atomic<int>          constructed_payloads{0};
    std::atomic<int>          disposed_payloads{0};
    std::atomic<int>          delivered_payloads{0};
    std::atomic<bool>         natural_completion{false};
    std::atomic<bool>         watchdog_fired{false};
    std::atomic<bool>         done{false};
};
struct StopAfterObservation : qb::Event {};
struct StashedPayload : qb::Event {
    std::shared_ptr<State> observations;
    explicit StashedPayload(std::shared_ptr<State> s)
        : observations(std::move(s)) {
        observations->constructed_payloads.fetch_add(1);
    }
    ~StashedPayload() {
        observations->disposed_payloads.fetch_add(1);
    }
};
struct UnwindMark {
    std::shared_ptr<State> state;
    ~UnwindMark() {
        state->unwound.fetch_add(1);
    }
};
class Subject : public qb::Actor {
    std::shared_ptr<State> _state;

public:
    explicit Subject(std::shared_ptr<State> s)
        : _state(std::move(s)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StashedPayload>(*this);
        const auto state = _state;
        auto       ctx   = context();
        UnwindMark frame{state};
        state->started_ns.store(now_ns());
        try {
            co_await ctx.until_cancelled(); // no timer, request or natural completion
            state->natural_completion.store(true);
        } catch (const qb::io::async::cancelled_error &) {
            state->cancelled_ns.store(now_ns());
            state->cancelled.fetch_add(1);
        }
        co_return false; // QB-363 cannot turn this timing witness into successful admission
    }
    void
    on(StashedPayload const &) {
        _state->delivered_payloads.fetch_add(1);
        kill();
    }
    ~Subject() override {
        _state->destroyed_ns.store(now_ns());
        _state->destroyed.fetch_add(1);
    }
};
class IdleSocketActor;
class IdleSocketSession : public use<IdleSocketSession>::tcp::client<IdleSocketActor> {
public:
    using Protocol = qb::protocol::text::command<IdleSocketSession>;
    explicit IdleSocketSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        *this << msg.text << Protocol::end;
    }
};

class IdleSocketActor
    : public qb::Actor
    , public use<IdleSocketActor>::tcp::server<IdleSocketSession> {
public:
    qb::io::async::task<bool>
    onInit() override {
        if (transport().listen_v4(0, "127.0.0.1") != 0)
            co_return false;
        start();
        g_port.store(transport().local_endpoint().port(), std::memory_order_release);
        co_return true;
    }
    void
    on(IOSession &) {}
};

// A same-core owner watches phase/disposal without creating an activation waiter's
// request deadline. Its callback observes setup; it is not counted as work by the park gate.
class Owner
    : public qb::Actor
    , public qb::ICallback {
    std::shared_ptr<State> _state;
    bool                   _stop_requested{false};
    void
    finish_if_observed() {
        if (_stop_requested && _state->down_count.load() == 1) {
            _state->done.store(true);
            broadcast<qb::KillEvent>(); // removes the optional idle socket holder
            kill();
        }
    }

public:
    explicit Owner(std::shared_ptr<State> s)
        : _state(std::move(s)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StopAfterObservation>(*this);
        registerEvent<qb::DownEvent>(*this);
        registerCallback(*this);
        auto child = addRefActor<Subject>(_state);
        watch(child.id());
        push<StashedPayload>(child.id(), _state);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (qb::detail::deadlines_armed())
            _state->request_armed_passes.fetch_add(1);
        const int active = qb::io::async::listener::current.loop().active_count();
        if (active > _state->max_watchers.load())
            _state->max_watchers.store(active);
    }
    void
    on(qb::DownEvent const &event) {
        _state->down_reason.store(static_cast<int>(event.reason));
        _state->down_count.fetch_add(1);
        finish_if_observed();
    }
    void
    on(StopAfterObservation const &) {
        _stop_requested = true;
        finish_if_observed();
    }
};

// This actor is owned by Main on core 1. Core 0 owns no watchdog timer/request.
// Its one actual cross-core event ends either mailbox wait shape. If the subject
// is already disposed this is cleanup; otherwise watchdog_fired makes the run fail.
class Watchdog
    : public qb::Actor
    , public qb::ICallback {
    std::shared_ptr<State> _state;
    qb::ActorId            _owner;
    std::int64_t           _created_ns{0};

public:
    Watchdog(std::shared_ptr<State> s, qb::ActorId owner)
        : _state(std::move(s))
        , _owner(owner)
        , _created_ns(now_ns()) {}
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        const auto started = _state->started_ns.load();
        const auto elapsed = now_ns() - (started ? started : _created_ns);
        if (_state->destroyed.load() == 0 && elapsed < kWatchdogBudget.count())
            return;
        if (_state->destroyed.load() == 0)
            _state->watchdog_fired.store(true);
        push<StopAfterObservation>(_owner); // mailbox notify, not Main::stop's generation only
        kill();
    }
};

void
run_park_witness(bool with_io) {
    const auto state = std::make_shared<State>();
    g_port.store(0);
    ScopedDeadline deadline(static_cast<std::uint64_t>(kDeadline.count()));
    qb::Main       main;
    main.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity});
    main.core(0).setLatency(kLatency).setIdleSpin(0ns);
    main.core(1).setAffinity(qb::CoreIdSet{qb::NoAffinity});
    main.core(1).setLatency(0ns);
    if (with_io)
        main.addActor<IdleSocketActor>(0); // ephemeral socket only; nobody sends it traffic
    const auto owner = main.addActor<Owner>(0, state);
    main.addActor<Watchdog>(1, state, owner);
    main.start();
    main.join(); // all actors and the optional socket are owned by Main
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(state->done.load());
    EXPECT_FALSE(state->watchdog_fired.load()) << "cross-core rescue must not be needed";
    ASSERT_GT(state->started_ns.load(), 0);
    ASSERT_EQ(state->cancelled.load(), 1);
    EXPECT_FALSE(state->natural_completion.load());
    EXPECT_EQ(state->unwound.load(), 1);
    EXPECT_EQ(state->destroyed.load(), 1);
    EXPECT_EQ(state->constructed_payloads.load(), 1);
    EXPECT_EQ(state->disposed_payloads.load(), 1);
    EXPECT_EQ(state->delivered_payloads.load(), 0);
    EXPECT_EQ(state->down_count.load(), 1);
    EXPECT_EQ(state->down_reason.load(), static_cast<int>(qb::DownReason::init_failed));
    EXPECT_EQ(state->request_armed_passes.load(), 0) << "request cap would mask the defect";
    const auto elapsed = state->cancelled_ns.load() - state->started_ns.load();
    EXPECT_GT(elapsed, 0);
    EXPECT_LT(elapsed, kAdmissionBound.count()) << "200 ms activation, 500 ms bound, 8 s park";
    EXPECT_GE(state->destroyed_ns.load(), state->cancelled_ns.load());
    if (with_io) {
        EXPECT_GT(g_port.load(), 0);
        EXPECT_GT(state->max_watchers.load(), 0);
    } else {
        EXPECT_EQ(state->max_watchers.load(), 0);
    }
}
TEST(InitDeadlineParkWitness, MailboxParkCannotOutlastActivationDeadline) {
    run_park_witness(false);
}
TEST(InitDeadlineParkWitness, IoLoopParkCannotOutlastActivationDeadline) {
    run_park_witness(true);
}
} // namespace init_deadline_park_witness_test
