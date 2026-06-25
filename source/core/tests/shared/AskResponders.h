/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/AskResponders.h
 * @brief Shared responder "zoo" + a kill-then-stop cancel helper for the `ask` / resilience tests.
 *
 * The native `ask` request/response stack (`qb::ask`, `qb::ask_retry`, `qb::ask_guarded`,
 * `ask_all` / `ask_any`, saga) is exercised against a small, well-understood cast of responders.
 * Every engine-half suite in the cluster needs the same cast, so it lives here as the single
 * source of truth (hoisted from the byte-for-byte clones in the former
 * test-actor-coroutine-resilience / ask suites):
 *
 *   - `Ping`         — a typed `qb::Request<int>` exchange event (request `seq`, response `int`).
 *   - `Echoer`       — always answers; `response == seq * 2` (the canonical "healthy" dependency).
 *   - `Market`       — alias-by-behaviour for `Echoer` (the "always-answers market"); kept as a
 *                      distinct name so resilience/ask suites read in their own domain vocabulary.
 *   - `SilentMarket` — registers the handler but never replies → every `ask` against it times out.
 *   - `SlowMarket`   — answers after a fixed delay via a *scoped* coroutine, so a kill cancels the
 *                      pending reply cleanly (no dangling `this`); models a slow-but-alive backend.
 *   - `FlakyMarket`  — drops the first `(reply_on - 1)` requests then answers, and records the
 *                      VirtualCore arrival timestamp of every request it sees. The timestamps are
 *                      the engine-observable oracle for "the retry backoff actually grew between
 *                      attempts" (see coroutine-resilience.cpp).
 *
 * `KillThenStopHelper<Victim>` is the shared shutdown driver: it kills a victim actor after a
 * short delay (to interrupt an in-flight `ask`) and then stops the engine slightly later, so the
 * victim's coroutine unwinds with `cancelled_error` *before* `qb::Main::stop()` tears the loop
 * down. Calling `qb::Main::stop()` directly from inside the cancelled coroutine can leave a
 * just-completed frame un-drained at teardown, which is exactly the race this helper avoids.
 *
 * All types live in `namespace qb::test`. Single-thread per-VirtualCore semantics hold: the
 * per-responder counters/timestamps are touched only from that responder's own core.
 */

#ifndef QB_CORE_TESTS_SHARED_ASK_RESPONDERS_H
#define QB_CORE_TESTS_SHARED_ASK_RESPONDERS_H

#include <cstdint>
#include <vector>

#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

namespace qb::test {

/// Typed `ask` exchange: request carries `seq`, the `Request<int>` base carries `response`.
/// (Named `seq`, not `id`, to avoid shadowing the Event base's type-id field.)
struct Ping : public qb::Request<int> {
    int seq{0};
    Ping() = default;
    explicit Ping(int s)
        : seq(s) {}
};

/// Always answers: `response == seq * 2`. The canonical healthy dependency.
class Echoer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &p) {
        qb::answer(*this, p, [](Ping const &r) { return r.seq * 2; });
    }
};

/// Behavioural alias for `Echoer` — the "always-answers market" in resilience/ask vocabulary.
class Market : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &p) {
        qb::answer(*this, p, [](Ping const &r) { return r.seq * 2; });
    }
};

/// Registers the handler but never replies → every `ask` against it times out.
class SilentMarket : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &) { /* intentionally silent — drives timeout / cancel paths */ }
};

/// Answers after `delay` via a scoped coroutine (a kill cancels the pending reply cleanly).
/// Distinguishable value: `response == seq * 3`.
class SlowMarket : public qb::Actor {
    qb::duration _delay;

public:
    explicit SlowMarket(qb::duration delay = std::chrono::milliseconds(80))
        : _delay(delay) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &p) {
        Ping resp     = p; // copy preserves correlation_id
        resp.response = p.seq * 3;
        auto src      = p.getSource();
        auto delay    = _delay;
        spawn([resp, src, delay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(delay);              // cancelled if this market is killed first
            ctx.template push_to<Ping>(src, resp);  // safe via context (dropped if asker is gone)
        });
    }
};

/**
 * @brief Drops the first `(reply_on - 1)` requests, then answers — models transient failures.
 * @details Optionally publishes the VirtualCore arrival timestamp (`time()`, nanoseconds) of every
 *          request into a caller-owned `ArrivalLog`, so a retrying asker leaves a timeline of
 *          attempt timestamps. With exponential backoff the gap between consecutive attempts must
 *          grow; that timeline is the engine-observable oracle for "the backoff actually grew".
 *
 *          The log is written ONLY from this responder's own core (single-thread per VirtualCore)
 *          and is meant to be read by the test thread ONLY after `qb::Main::join()` — `join()`
 *          establishes the happens-before, so the plain `std::vector` needs no extra synchronisation.
 */
class FlakyMarket : public qb::Actor {
public:
    /// Caller-owned record of request arrivals; read after `join()`. `count` mirrors `arrivals.size()`.
    struct ArrivalLog {
        std::vector<std::uint64_t> arrivals; ///< VirtualCore timestamp (ns) of each request seen.

        /// Total requests recorded.
        [[nodiscard]] std::size_t
        count() const noexcept {
            return arrivals.size();
        }

        /// Inter-arrival gaps (ns): `gaps[k] == arrivals[k+1] - arrivals[k]`.
        [[nodiscard]] std::vector<std::uint64_t>
        gaps() const {
            std::vector<std::uint64_t> g;
            for (std::size_t i = 1; i < arrivals.size(); ++i)
                g.push_back(arrivals[i] - arrivals[i - 1]);
            return g;
        }
    };

    /// @param reply_on Answer from the `reply_on`-th request onward (drop the earlier ones).
    /// @param log      Optional caller-owned arrival timeline (nullptr to not record).
    explicit FlakyMarket(int reply_on, ArrivalLog *log = nullptr)
        : _reply_on(reply_on)
        , _log(log) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &p) {
        if (_log)
            _log->arrivals.push_back(time());
        if (++_count >= _reply_on)
            qb::answer(*this, p, [](Ping const &r) { return r.seq * 2; });
        // else: drop it → the asker times out and retries after a (growing) backoff.
    }

private:
    int         _count{0};
    int         _reply_on;
    ArrivalLog *_log;
};

/**
 * @brief Kill a victim actor after `kill_after`, then stop the engine after `stop_after`.
 * @tparam Victim Unused tag kept for call-site readability; the victim is addressed by id.
 * @details `kill_after < stop_after` guarantees the victim's in-flight `ask` unwinds with
 *          `cancelled_error` (and any handler runs to completion) *before* the loop is torn down,
 *          avoiding the un-drained-frame race of stopping from inside the cancelled coroutine.
 */
template <typename Victim = void>
class KillThenStopHelper : public qb::Actor {
    const qb::ActorId  _victim;
    const qb::duration _kill_after;
    const qb::duration _stop_after;

public:
    KillThenStopHelper(qb::ActorId victim, qb::duration kill_after, qb::duration stop_after)
        : _victim(victim)
        , _kill_after(kill_after)
        , _stop_after(stop_after) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, _kill_after);
        qb::io::async::callback([] { qb::Main::stop(); }, _stop_after);
        co_return true;
    }
};

} // namespace qb::test

#endif // QB_CORE_TESTS_SHARED_ASK_RESPONDERS_H
