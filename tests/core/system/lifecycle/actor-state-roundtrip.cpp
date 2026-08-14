/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/lifecycle/actor-state-roundtrip.cpp
 * @brief Per-actor mutable state survives an update/query round-trip, a simulated failure window
 *        drops queries, and a restore re-enables them — all driven by response counting, never a
 *        wall clock.
 *
 * A `StatefulActor` owns a `key → int` map. The coordinator drives a deterministic, *causally
 * chained* sequence: every step is triggered by the observed effect of the previous one (a response
 * received, or a control marker echoed back through the stateful actor's own mailbox), so there is
 * no `400ms` timer and the run ends the instant the last expected response arrives.
 *
 * Strengthened oracles (the original buried its asserts inside a fire-and-forget response handler,
 * so a DROPPED response simply vanished):
 *   - the coordinator COUNTS responses and the test asserts the EXACT expected count after join()
 *     — a dropped response now fails the count, it cannot silently disappear;
 *   - each response's value is asserted against the value the stateful actor actually stored;
 *   - the during-failure query produces NO response: a `Marker` is round-tripped through the
 *     stateful actor AFTER the failure-window query, and the response count is frozen across it —
 *     proving the query was dropped (not merely slow).
 */

#include <atomic>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <string_view>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>
#include <qb/string.h>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Protocol events.
//
// The keys are `qb::string<N>`, not `std::string`. The engine relocates an event with `memcpy`
// and never runs the source destructor, so a payload member may hold no pointer into itself; a
// SHORT std::string on libstdc++ does exactly that (`_M_p` addresses its own inline buffer), and
// the keys here are short. This exchange happens to be single-core, so the trap is latent rather
// than firing -- but "latent" depends on a core assignment two screens away, and pipe growth,
// compaction, `reply()` and `forward()` relocate same-core events too. `relocatable-payload.cpp`
// pins the rule; this file should not be the suite's own counter-example to it.
// ---------------------------------------------------------------------------
struct StateUpdate : public qb::Event {
    qb::string<32> key;
    int            value;
    StateUpdate(std::string_view k, int v)
        : key(k)
        , value(v) {}
};

struct StateQuery : public qb::Event {
    qb::string<32> key;
    qb::ActorId    reply_to;
    int            tag; // echoed back in the response so the coordinator can sequence it
    StateQuery(std::string_view k, qb::ActorId id, int t)
        : key(k)
        , reply_to(id)
        , tag(t) {}
};

struct StateResponse : public qb::Event {
    qb::string<32> key;
    int            value;
    bool           found;
    int            tag;
    StateResponse(std::string_view k, int v, bool f, int t)
        : key(k)
        , value(v)
        , found(f)
        , tag(t) {}
};

struct SimulateFailure : public qb::Event {};
struct RestoreState : public qb::Event {};

// A control marker round-tripped through the stateful actor's mailbox to prove ordering WITHOUT
// querying state (it is always echoed, even while "failed"), so the coordinator can observe that
// the stateful actor has drained past a dropped query.
struct Marker : public qb::Event {
    qb::ActorId reply_to;
    int         marker_id; // not `id`: would shadow the Event base's type-id field
    Marker(qb::ActorId r, int i)
        : reply_to(r)
        , marker_id(i) {}
};
struct MarkerEcho : public qb::Event {
    int marker_id;
    explicit MarkerEcho(int i)
        : marker_id(i) {}
};

// ---------------------------------------------------------------------------
// Global oracles (single-core; read after join()).
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_recovered{false};
std::atomic<int>  g_responses{0}; // total StateResponse events the coordinator received
std::atomic<bool> g_done{false};  // the coordinator reached the end of its script
} // namespace

class StatefulActor : public qb::Actor {
    std::map<std::string, int> _state;
    bool                       _failed = false;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StateUpdate>(*this);
        registerEvent<StateQuery>(*this);
        registerEvent<SimulateFailure>(*this);
        registerEvent<RestoreState>(*this);
        registerEvent<Marker>(*this);
        co_return true;
    }

    void
    on(const StateUpdate &e) {
        if (_failed)
            return;
        _state[e.key.c_str()] = e.value;
    }

    void
    on(const StateQuery &e) {
        if (_failed)
            return; // during the failure window a query produces NO response
        const auto it    = _state.find(e.key.c_str());
        const bool found = (it != _state.end());
        const int  value = found ? it->second : -1;
        to(e.reply_to).push<StateResponse>(e.key, value, found, e.tag);
    }

    void
    on(const SimulateFailure &) {
        _failed = true;
    }

    void
    on(const RestoreState &) {
        _failed = false;
        g_recovered.store(true);
    }

    // Always echoed, even while _failed — a liveness/ordering probe independent of state.
    void
    on(const Marker &e) {
        to(e.reply_to).push<MarkerEcho>(e.marker_id);
    }
};

// ---------------------------------------------------------------------------
// Coordinator: a causally-chained script. Each step is fired by the observed effect of the prior
// one. Response tags: 1 = post-update query, 2 = post-recovery query.
// ---------------------------------------------------------------------------
class StateCoordinator : public qb::Actor {
    qb::ActorId _stateful;
    int         _responses_at_failure_marker = -1;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StateResponse>(*this);
        registerEvent<MarkerEcho>(*this);

        _stateful = addRefActor<StatefulActor>().id();

        // Step 1: write, then query it back (tag 1). The response drives the next step.
        to(_stateful).push<StateUpdate>("test", 42);
        to(_stateful).push<StateQuery>("test", id(), /*tag*/ 1);
        co_return true;
    }

    void
    on(const StateResponse &e) {
        g_responses.fetch_add(1);

        if (e.tag == 1) {
            // Post-update round-trip: the stored value must come back intact.
            EXPECT_TRUE(e.found) << "key 'test' must be found after the update";
            EXPECT_EQ(e.value, 42) << "the stored value must round-trip";

            // Step 2: enter the failure window, fire a query that MUST be dropped, then a Marker.
            // The Marker is echoed regardless of _failed, so its echo tells us the stateful actor
            // drained past the dropped query — at which point the response count must be unchanged.
            to(_stateful).push<SimulateFailure>();
            to(_stateful).push<StateQuery>("test", id(), /*tag*/ 99); // dropped (failed window)
            _responses_at_failure_marker = g_responses.load();        // == 1 here
            to(_stateful).push<Marker>(id(), 1);
        } else if (e.tag == 2) {
            // Post-recovery round-trip: state intact again.
            EXPECT_TRUE(e.found) << "key 'test' must be found after recovery";
            EXPECT_EQ(e.value, 42) << "state must survive the failure/restore cycle";
            g_done.store(true);
            qb::Main::stop();
            kill();
        }
    }

    void
    on(const MarkerEcho &) {
        // The stateful actor has processed everything up to and including the dropped query: the
        // failure-window query produced NO response (the count is frozen at the pre-marker value).
        EXPECT_EQ(g_responses.load(), _responses_at_failure_marker) << "a query during the failure window must produce NO response";

        // Step 3: restore, then re-query (tag 2) to prove state survived the cycle.
        to(_stateful).push<RestoreState>();
        to(_stateful).push<StateQuery>("test", id(), /*tag*/ 2);
    }
};

TEST(StateRoundTrip, SurvivesUpdateFailureRestoreCycle) {
    g_recovered.store(false);
    g_responses.store(0);
    g_done.store(false);

    qb::Main main;
    main.core(0).addActor<StateCoordinator>();

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_done.load()) << "the coordinator must reach the end of its script";
    EXPECT_TRUE(g_recovered.load()) << "the restore step must have run";
    // EXACTLY two responses: the post-update query (tag 1) and the post-recovery query (tag 2).
    // The failure-window query (tag 99) must NOT have produced one — a dropped response would make
    // this 1 (under) and a stray response would make it 3 (over).
    EXPECT_EQ(g_responses.load(), 2) << "exactly two queries are answered; the failed one is dropped";
}
