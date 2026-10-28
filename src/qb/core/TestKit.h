/**
 * @file qb/core/TestKit.h
 * @brief A test kit for actor code: run a scenario inside an engine, record what actors send, wait
 *        for it with a deadline -- `qb::test::TestKit`, `qb::test::TestProbe`, `qb::test::expect`,
 *        `qb::test::expect_none` (since 3.4, Huly QB-57).
 *
 * A test of actor code has the same shape every time: an engine, the actors under test, an actor
 * that drives the exchange from a coroutine, a way to know what the actors sent and when, a stop,
 * a join, and a check of `qb::Main::hasError()`. This header ships that shape:
 *
 * - `TestKit` owns a `qb::Main` and runs ONE scenario -- a coroutine taking a
 *   `qb::ScopedCoroContext` -- on a driver actor it adds itself. The engine stops the moment the
 *   scenario returns or throws; a watchdog stops it if the scenario never ends. `run()` returns a
 *   `RunResult`: completed, timed out, the engine's error, what the scenario threw.
 * - `TestProbe<Events...>` is an actor that records a copy of every event of the announced types it
 *   receives, in arrival order, per type. Give its id to the actors under test as the destination
 *   to observe.
 * - `co_await expect<E>(ctx, probe, timeout)` yields the next recorded `E` -- at once when one is
 *   waiting, else when it arrives -- or an empty optional at the deadline; `expect_none<E>(ctx,
 *   probe, window)` is true when no `E` arrived during the window. Both are a `qb::ask` of the probe,
 *   so the probe may live on any core.
 *
 * It is not a deterministic scheduler: the engine runs for real, on its own threads, and a test
 * asserts on what arrives within a deadline, never on interleavings. It depends on no test framework:
 * a scenario checks with whatever the test uses (GoogleTest's `EXPECT_*`; not `ASSERT_*`, whose
 * `return` is not allowed in a coroutine) and the `RunResult` says how the run ended.
 *
 * @code
 * #include <stdexcept>
 * #include <qb/testkit.h>
 *
 * struct Order : qb::Event {
 *     int qty = 0;
 *     explicit Order(int q) : qty(q) {}
 * };
 * struct Filled : qb::Event {
 *     int qty = 0;
 *     explicit Filled(int q) : qty(q) {}
 * };
 * // The actor under test reports to the destination it is given -- in the test, a probe.
 * class Desk : public qb::Actor {
 *     qb::ActorId _report;
 * public:
 *     explicit Desk(qb::ActorId report) : _report(report) {}
 *     qb::io::async::task<bool> onInit() override {
 *         registerEvent<Order>(*this);
 *         co_return true;
 *     }
 *     void on(Order const &o) { push<Filled>(_report, o.qty); }
 * };
 *
 * bool desk_fills_an_order() {
 *     qb::test::TestKit kit;
 *     const auto probe = kit.probe<Filled>(0);
 *     const auto desk  = kit.add<Desk>(0, probe);
 *     const auto run   = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
 *         ctx.push_to<Order>(desk, 7);
 *         const auto filled = co_await qb::test::expect<Filled>(ctx, probe, std::chrono::seconds(1));
 *         if (!filled || filled->qty != 7 || filled->getSource() != desk)
 *             throw std::runtime_error("no fill of 7 from the desk"); // the run reports it
 *         if (!co_await qb::test::expect_none<Filled>(ctx, probe, std::chrono::milliseconds(50)))
 *             throw std::runtime_error("a second fill");
 *     });
 *     return static_cast<bool>(run); // false: run.describe() says why
 * }
 * @endcode
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 * @ingroup Patterns
 */

#ifndef QB_CORE_TESTKIT_H
#define QB_CORE_TESTKIT_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <qb/core/Actor.h>
#include <qb/core/Main.h>
#include <qb/core/VirtualCore.h> // registerEvent / push / spawn bodies (the tail of VirtualCore.h)
#include <qb/core/patterns/request.h>
#include <qb/io/async/coroutine.h>
#include <qb/system/time.h>

namespace qb::test {

namespace detail {

/**
 * @brief The one exchange between a scenario and a probe: "the next `E`", or "no `E` during the
 *        window" (`absence`).
 * @details A single non-template type, so a driver registers it once whatever the probes record. The
 *          probe answers with a copy of the recorded event behind a `shared_ptr` made on its own core;
 *          the mailbox that carries the reply orders the copy before its read on the asker's core.
 */
struct Expectation : qb::Request<std::shared_ptr<void const>> {
    qb::TypeId    event_type{};     ///< `qb::type_id<E>()` of the expected event
    qb::duration  wait{};           ///< How long the probe waits for one before it answers empty
    bool          absence  = false; ///< `expect_none`: answer at the end of the window, consume nothing
    bool          recorded = true;  ///< Set by the probe: false when it does not record `event_type`
    std::uint64_t lend     = 0;     ///< Set by the probe with an event: the asker acknowledges it (`ExpectationTaken`)
    std::uint64_t token    = 0;     ///< The asker's name for it: a withdrawal names it (`ExpectationWithdrawn`)

    Expectation() = default;
    Expectation(qb::TypeId type, qb::duration window, bool none, std::uint64_t name)
        : event_type(type)
        , wait(window)
        , absence(none)
        , token(name) {}
};

/// A probe's reminder to itself that the expectation `ticket` reached its deadline.
struct ExpectationDeadline : qb::Event {
    std::uint64_t ticket = 0;

    explicit ExpectationDeadline(std::uint64_t t)
        : ticket(t) {}
};

/// The asker's acknowledgement of the event an `Expectation` carried: the probe forgets it.
struct ExpectationTaken : qb::Event {
    std::uint64_t lend = 0;

    explicit ExpectationTaken(std::uint64_t l)
        : lend(l) {}
};

/// The asker's withdrawal of the expectation `token`: its frame ended without the answer -- the losing branch of a
/// `when_any`, an ask that timed out first. The probe forgets its waiter, and an event it lent it comes back at once.
struct ExpectationWithdrawn : qb::Event {
    std::uint64_t token = 0;

    explicit ExpectationWithdrawn(std::uint64_t t)
        : token(t) {}
};

/// A probe's reminder to itself that the event lent as `lend` was never acknowledged.
struct LendExpired : qb::Event {
    std::uint64_t lend = 0;

    explicit LendExpired(std::uint64_t l)
        : lend(l) {}
};

/// What the asker waits beyond the probe's own deadline before it gives up on the probe itself.
inline constexpr qb::duration reply_grace = std::chrono::milliseconds(250);

/// How long a probe keeps an event it answered with, unacknowledged, before it records it again: the asker
/// acknowledges at once, an expectation it abandons is withdrawn at once (`ExpectationWithdrawn`), and a dead
/// asker's loans come back at its `qb::DownEvent`, so only an asker whose core stalls past it reaches it.
inline constexpr qb::duration lend_hold = std::chrono::seconds(2);

/// The names of the expectations of the process: unique, so a probe tells two of one asker apart. Anchored as the
/// core's other process-wide counters (`QB_ABI_ANCHOR`): one object across the shared images of a program on ELF
/// and Mach-O; on Windows qb has no export macro yet, and each DLL counts its own.
QB_ABI_ANCHOR inline std::atomic<std::uint64_t> expectation_tokens{0};

/// `a + b`, `qb::duration::max()` past it: a timeout of `max()` means "no deadline", never an overflow.
[[nodiscard]] constexpr qb::duration
add_saturating(qb::duration const a, qb::duration const b) noexcept {
    return a > qb::duration::max() - b ? qb::duration::max() : a + b;
}
static_assert(add_saturating(qb::duration::max(), reply_grace) == qb::duration::max());

/// Withdraws the expectation `token` from `probe` when its frame ends without taking the answer -- unwound by a
/// `when_any` that another branch won, destroyed, an ask that timed out -- so what the probe holds for it goes
/// to the next expectation at once. A killed asker pushes nothing (its scope is cancelled before its frames
/// end, and its core may be tearing down): the probe watches its askers and lets go of a dead one's itself.
struct WithdrawOnExit {
    qb::ScopedCoroContext ctx;
    qb::ActorId           probe;
    std::uint64_t         token;
    bool                  armed = true;

    WithdrawOnExit(qb::ScopedCoroContext c, qb::ActorId p, std::uint64_t t)
        : ctx(std::move(c))
        , probe(p)
        , token(t) {}
    WithdrawOnExit(WithdrawOnExit const &)            = delete;
    WithdrawOnExit &operator=(WithdrawOnExit const &) = delete;

    ~WithdrawOnExit() {
        if (armed && !ctx.cancelled())
            ctx.push_to<ExpectationWithdrawn>(probe, token);
    }
};

/// How a scenario ended, written on the driver's core and read on the test thread.
struct ScenarioState {
    std::mutex              lock;
    std::condition_variable done;
    bool                    finished  = false;
    bool                    completed = false;
    std::exception_ptr      error;

    void
    finish(bool ran_to_end, std::exception_ptr thrown) {
        {
            std::lock_guard<std::mutex> guard(lock);
            finished  = true;
            completed = ran_to_end;
            error     = std::move(thrown);
        }
        done.notify_all();
    }
};

/**
 * @brief The actor a `TestKit` adds to run its scenario: it registers the exchanges the scenario asks
 *        (`Expectation` and the `Asks` named to `run<Asks...>()`), so their replies resolve.
 */
template <typename Scenario, typename... Asks>
class ScenarioDriver final : public qb::Actor {
    Scenario                       _scenario;
    std::shared_ptr<ScenarioState> _state;

public:
    ScenarioDriver(Scenario scenario, std::shared_ptr<ScenarioState> state)
        : _scenario(std::move(scenario))
        , _state(std::move(state)) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Expectation>(*this);
        (registerEvent<Asks>(*this), ...);
        spawn([scenario = std::move(_scenario), state = _state](qb::ScopedCoroContext ctx) mutable -> qb::io::async::task<void> {
            try {
                co_await scenario(ctx);
                state->finish(true, nullptr);
            } catch (...) {
                state->finish(false, std::current_exception());
            }
            qb::Main::stop();
        });
        co_return true;
    }

    void
    on(Expectation &e) {
        (void) resolve_ask(e); // a reply to the scenario's expect; anything else is dropped
    }

    template <typename A>
    requires(std::is_same_v<A, Asks> || ...)
    void
    on(A &e) {
        (void) resolve_ask(e);
    }
};

} // namespace detail

/**
 * @brief How a `TestKit::run()` ended.
 * @details `true` when the scenario ran to its end, the watchdog did not fire and the engine reported
 *          no error; `describe()` says which of those failed, for a test's failure message.
 */
struct RunResult {
    bool               completed    = false; ///< The scenario returned without throwing
    bool               timed_out    = false; ///< The watchdog stopped the engine before the scenario ended
    bool               engine_error = false; ///< `qb::Main::hasError()` after the join: a core stopped on an error
    std::exception_ptr error;                ///< What the scenario threw, if it threw

    [[nodiscard]] explicit
    operator bool() const noexcept {
        return completed && !timed_out && !engine_error;
    }

    /// One line naming what went wrong, or "ok".
    [[nodiscard]] std::string
    describe() const {
        std::string why;
        const auto  add = [&why](std::string part) {
            why += (why.empty() ? "" : "; ") + std::move(part);
        };
        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (std::exception const &e) {
                add(std::string("the scenario threw: ") + e.what());
            } catch (...) {
                add("the scenario threw a non-std exception");
            }
        } else if (!completed)
            add("the scenario did not run to its end");
        if (timed_out)
            add("the watchdog stopped the engine");
        if (engine_error)
            add("the engine reported an error (qb::Main::hasError())");
        return why.empty() ? std::string("ok") : why;
    }
};

/**
 * @brief An actor that records every event of the announced types it receives, and answers the
 *        `expect` / `expect_none` of a scenario.
 * @tparam Events The event types to record: each derives from `qb::Event` and is copy-constructible
 *         (the probe keeps a copy; its source -- `getSource()` -- is the sender).
 * @details Each type has its own queue, in arrival order, and an `expect<E>` consumes the oldest `E`.
 *          An expectation that finds nothing waits, on the probe, until an `E` arrives (the oldest
 *          waiter is served first) or its deadline passes. `expect_none<E>` consumes nothing: it is
 *          answered "present" by an `E` already waiting or arriving within its window, "absent" at the
 *          end of it. Nothing is ever dropped: what no expectation consumed stays recorded for the next. An event
 *          handed to an expectation is only lent until its asker acknowledges it. An expectation its asker
 *          abandons -- the losing branch of a `when_any`, an ask that timed out first -- is withdrawn as its frame
 *          ends: its waiter is forgotten, and an event lent to it returns at once to its place in the queue, in
 *          arrival order, for the next expectation. The probe watches every actor that asks it (`watch()`): when
 *          one is gone, its waiters are dropped and what it was lent returns the same way, oldest event first. A
 *          loan nobody acknowledges otherwise -- an asker whose core stalls -- returns after `detail::lend_hold`
 *          (2 s). A probe may record `qb::DownEvent` too: it records every one sent to it, and keeps those of its
 *          own watches -- the core's answer about an asker -- to itself. The queues are unbounded -- a probe is for
 *          tests, not for traffic.
 */
template <typename... Events>
class TestProbe final : public qb::Actor {
    static_assert(sizeof...(Events) > 0, "qb::test::TestProbe: name at least one event type to record");
    static_assert((std::is_base_of_v<qb::Event, Events> && ...), "qb::test::TestProbe: every recorded type derives from qb::Event");
    static_assert((std::is_copy_constructible_v<Events> && ...), "qb::test::TestProbe: every recorded type is copy-constructible");
    static_assert((!std::is_same_v<Events, detail::Expectation> && ...), "qb::test::TestProbe: the kit's own exchange is not a recorded type");

    struct waiter {
        std::uint64_t ticket;      ///< Matches the deadline reminder
        qb::ActorId   asker;       ///< Where the answer goes
        std::uint64_t correlation; ///< The asker's `correlation_id`
        std::uint64_t token;       ///< The asker's name for it (a withdrawal)
        bool          absence;     ///< An `expect_none`
    };
    /// A recorded event and its rank of arrival: what comes back from a loan goes back to its place.
    struct entry {
        std::uint64_t               seq = 0;
        std::shared_ptr<void const> event;
    };
    struct queue {
        qb::TypeId         type;
        std::deque<entry>  received;
        std::deque<waiter> waiting;
    };
    struct loan {
        std::uint64_t id;
        queue        *from;
        entry         item;
        qb::ActorId   asker;
        std::uint64_t token;
    };
    /// A loan that expired: an acknowledgement arriving after it takes the event back out of its queue.
    struct expired_loan {
        std::uint64_t id;
        queue        *from;
        std::uint64_t seq;
    };
    static constexpr std::size_t expired_memory = 256;

    std::array<queue, sizeof...(Events)> _queues{queue{qb::type_id<Events>(), {}, {}}...};
    std::vector<loan>                    _lent;
    std::deque<expired_loan>             _expired;
    std::vector<qb::ActorId>             _askers; ///< Watched: their DownEvent lets go of what waits for them
    std::uint64_t                        _next_ticket = 0;
    std::uint64_t                        _next_lend   = 0;
    std::uint64_t                        _next_seq    = 0;

    template <typename E>
    static constexpr std::size_t
    index_of() noexcept {
        std::size_t index = 0, found = 0;
        ((std::is_same_v<E, Events> ? (found = index, ++index) : ++index), ...);
        return found;
    }

    queue *
    find(qb::TypeId type) noexcept {
        for (auto &q : _queues)
            if (q.type == type)
                return &q;
        return nullptr;
    }

    /// Lend `item` of `q` to an expectation: it comes back to `q` unless the asker acknowledges it in time.
    std::uint64_t
    lend(queue &q, entry item, qb::ActorId asker, std::uint64_t token) {
        const auto id = ++_next_lend;
        _lent.push_back(loan{id, &q, std::move(item), asker, token});
        spawn([id](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(detail::lend_hold);
            ctx.push<detail::LendExpired>(id);
        });
        return id;
    }

    void
    answer(queue &q, waiter const &w, entry const &item) {
        auto &reply          = push<detail::Expectation>(w.asker);
        reply.correlation_id = w.correlation;
        reply.absence        = w.absence;
        reply.response       = item.event;
        if (item.event && !w.absence)
            reply.lend = lend(q, item, w.asker, w.token);
    }

    /// `item` reaches `q`: it answers every absence waiter, and the oldest expectation, or it is recorded -- at
    /// the back when it arrives, at its rank of arrival when a loan of it ends unacknowledged.
    void
    record(queue &q, entry item, bool returned = false) {
        bool consumed = false;
        for (auto it = q.waiting.begin(); it != q.waiting.end();) {
            if (it->absence || !consumed) {
                answer(q, *it, item);
                consumed = consumed || !it->absence;
                it       = q.waiting.erase(it);
            } else
                ++it;
        }
        if (consumed)
            return;
        if (returned)
            q.received.insert(std::find_if(q.received.begin(), q.received.end(), [&item](entry const &e) { return e.seq > item.seq; }),
                              std::move(item));
        else
            q.received.push_back(std::move(item));
    }

    /// The loan `it` ends unacknowledged: its event goes back to its queue.
    void
    give_back(typename std::vector<loan>::iterator it) {
        auto from = it->from;
        auto item = std::move(it->item);
        _lent.erase(it);
        record(*from, std::move(item), true);
    }

public:
    qb::io::async::task<bool>
    onInit() override {
        (registerEvent<Events>(*this), ...);
        registerEvent<detail::Expectation>(*this);
        registerEvent<detail::ExpectationDeadline>(*this);
        registerEvent<detail::ExpectationTaken>(*this);
        registerEvent<detail::ExpectationWithdrawn>(*this);
        registerEvent<detail::LendExpired>(*this);
        if constexpr (!(std::is_same_v<qb::DownEvent, Events> || ...))
            registerEvent<qb::DownEvent>(*this); // the watch of the askers; a recorded DownEvent is registered above
        co_return true;
    }

    template <typename E>
    requires(std::is_same_v<E, Events> || ...)
    void
    on(E const &e) {
        record(_queues[index_of<E>()], entry{++_next_seq, std::make_shared<E const>(e)});
    }

    void
    on(detail::Expectation &e) {
        if (resolve_ask(e))
            return;
        if (std::find(_askers.begin(), _askers.end(), e.getSource()) == _askers.end()) {
            _askers.push_back(e.getSource()); // watched once: at its DownEvent its waiters and loans are let go of
            watch(e.getSource());
        }
        queue *q = find(e.event_type);
        if (!q) {
            e.recorded = false;
            reply(e);
            return;
        }
        if (!q->received.empty()) {
            e.response = q->received.front().event;
            if (!e.absence) {
                auto item = std::move(q->received.front());
                q->received.pop_front();
                e.lend = lend(*q, std::move(item), e.getSource(), e.token);
            }
            reply(e);
            return;
        }
        if (e.wait <= qb::duration::zero()) {
            reply(e); // nothing recorded, nothing to wait for: empty
            return;
        }
        const auto ticket = ++_next_ticket;
        q->waiting.push_back(waiter{ticket, e.getSource(), e.correlation_id, e.token, e.absence});
        spawn([ticket, wait = e.wait](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(wait);
            ctx.push<detail::ExpectationDeadline>(ticket);
        });
    }

    void
    on(detail::ExpectationDeadline const &e) {
        for (auto &q : _queues)
            for (auto it = q.waiting.begin(); it != q.waiting.end(); ++it)
                if (it->ticket == e.ticket) {
                    answer(q, *it, entry{}); // answered already when an event came first: not found here
                    q.waiting.erase(it);
                    return;
                }
    }

    void
    on(detail::ExpectationTaken const &e) {
        if (std::erase_if(_lent, [&e](loan const &l) { return l.id == e.lend; }) != 0)
            return;
        // Acknowledged after its loan expired: the asker has it, so the copy given back must not be served again.
        const auto late = std::find_if(_expired.begin(), _expired.end(), [&e](expired_loan const &x) { return x.id == e.lend; });
        if (late == _expired.end())
            return;
        auto &received = late->from->received;
        std::erase_if(received, [seq = late->seq](entry const &x) { return x.seq == seq; });
        _expired.erase(late);
    }

    void
    on(detail::ExpectationWithdrawn const &e) {
        const auto asker = e.getSource();
        for (auto &q : _queues)
            std::erase_if(q.waiting, [&](waiter const &w) { return w.asker == asker && w.token == e.token; });
        const auto it = std::find_if(_lent.begin(), _lent.end(), [&](loan const &l) { return l.asker == asker && l.token == e.token; });
        if (it != _lent.end())
            give_back(it); // lent to it: back at once, at its rank
    }

    /// The answer of the probe's own watch -- the core delivers it with the watched asker as its source -- lets go
    /// of a dead asker: what waits for it is dropped, and what it was lent goes back oldest event first, so a waiter
    /// parked meanwhile takes the events in arrival order. Any other DownEvent, one an actor sends, touches no asker:
    /// it is recorded when the probe records the type.
    void
    on(qb::DownEvent const &e) {
        const auto watched = std::find(_askers.begin(), _askers.end(), e.watched);
        if (watched == _askers.end() || e.getSource() != e.watched) {
            if constexpr ((std::is_same_v<qb::DownEvent, Events> || ...))
                record(_queues[index_of<qb::DownEvent>()], entry{++_next_seq, std::make_shared<qb::DownEvent const>(e)});
            return;
        }
        _askers.erase(watched);
        const auto asker    = e.watched;
        const auto of_asker = [&](loan const &l) {
            return l.asker == asker;
        };
        for (auto &q : _queues)
            std::erase_if(q.waiting, [&](waiter const &w) { return w.asker == asker; });
        for (auto it = std::find_if(_lent.begin(), _lent.end(), of_asker); it != _lent.end();
             it      = std::find_if(_lent.begin(), _lent.end(), of_asker)) {
            for (auto other = std::next(it); other != _lent.end(); ++other)
                if (of_asker(*other) && other->item.seq < it->item.seq)
                    it = other;
            give_back(it);
        }
    }

    void
    on(detail::LendExpired const &e) {
        const auto it = std::find_if(_lent.begin(), _lent.end(), [&e](loan const &l) { return l.id == e.lend; });
        if (it == _lent.end())
            return; // acknowledged, or withdrawn
        _expired.push_back(expired_loan{it->id, it->from, it->item.seq});
        if (_expired.size() > expired_memory)
            _expired.pop_front();
        give_back(it);
    }
};

/**
 * @brief The next `E` the probe recorded -- at once when one is waiting, else the first to arrive
 *        within @p timeout -- or an empty optional.
 * @tparam E A type the probe records.
 * @param ctx The scenario's context (any `spawn`ed coroutine of an actor that registers
 *            `qb::test::detail::Expectation`, as a `TestKit` driver does).
 * @param probe A `TestProbe` recording `E`, on any core.
 * @param timeout How long to wait; zero or less takes only an `E` already recorded.
 * @return The event, a copy; empty when none arrived in time, or when the probe did not answer at all
 *         (its core stopped) within @p timeout plus a grace.
 * @throws std::logic_error when the probe does not record `E` -- a test written against the wrong probe.
 * @throws qb::io::async::cancelled_error when the asking actor is killed meanwhile.
 */
template <typename E>
[[nodiscard]] qb::io::async::task<std::optional<E>>
expect(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration timeout) {
    static_assert(std::is_base_of_v<qb::Event, E> && std::is_copy_constructible_v<E>, "qb::test::expect: E is a copyable qb::Event");
    const auto                         wait  = timeout > qb::duration::zero() ? timeout : qb::duration::zero();
    const auto                         token = ++detail::expectation_tokens;
    detail::WithdrawOnExit             withdraw{ctx, probe, token};
    std::optional<detail::Expectation> answer;
    try {
        answer.emplace(co_await qb::ask(ctx, probe, detail::Expectation{qb::type_id<E>(), wait, false, token},
                                        detail::add_saturating(wait, detail::reply_grace)));
    } catch (qb::io::async::timeout_error const &) {
        co_return std::nullopt; // withdrawn on the way out: an answer still on its way comes back to the probe
    }
    if (!answer->recorded)
        throw std::logic_error("qb::test::expect: the probe does not record this event type");
    withdraw.armed = false; // answered: the probe holds nothing more for it
    if (!answer->response)
        co_return std::nullopt;
    if (answer->lend != 0)
        ctx.push_to<detail::ExpectationTaken>(probe, answer->lend); // taken: the probe forgets it
    co_return *static_cast<E const *>(answer->response.get());
}

/**
 * @brief Whether no `E` reached the probe during @p window: false at once when an `E` is already
 *        recorded and not consumed, false the moment one arrives, true at the end of the window.
 * @tparam E A type the probe records.
 * @param ctx The scenario's context (see `expect`).
 * @param probe A `TestProbe` recording `E`, on any core.
 * @param window How long to watch.
 * @return true when none arrived; false when one did, or when the probe did not answer at all.
 *         Nothing is consumed: a following `expect<E>` still gets the event.
 * @throws std::logic_error when the probe does not record `E`.
 * @throws qb::io::async::cancelled_error when the asking actor is killed meanwhile.
 */
template <typename E>
[[nodiscard]] qb::io::async::task<bool>
expect_none(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration window) {
    static_assert(std::is_base_of_v<qb::Event, E>, "qb::test::expect_none: E is a qb::Event");
    const auto                         wait  = window > qb::duration::zero() ? window : qb::duration::zero();
    const auto                         token = ++detail::expectation_tokens;
    detail::WithdrawOnExit             withdraw{ctx, probe, token};
    std::optional<detail::Expectation> answer;
    try {
        answer.emplace(co_await qb::ask(ctx, probe, detail::Expectation{qb::type_id<E>(), wait, true, token},
                                        detail::add_saturating(wait, detail::reply_grace)));
    } catch (qb::io::async::timeout_error const &) {
        co_return false;
    }
    withdraw.armed = false;
    if (!answer->recorded)
        throw std::logic_error("qb::test::expect_none: the probe does not record this event type");
    co_return !answer->response;
}

/**
 * @brief An engine for one test: add the actors and probes, then `run()` one scenario.
 * @details `run()` adds the driver, starts the engine on its own threads, waits for the scenario to
 *          end -- the driver stops the engine at once when it returns or throws -- or for the watchdog,
 *          then joins and reads `hasError()`. One run per kit: an engine does not restart.
 *          `qb::Main::stop()` stops every engine of the process, so kits run one at a time. Configure
 *          the cores through `engine()` before `run()` (a core runs at latency 0, spinning, by default).
 */
class TestKit {
    qb::Main _engine;
    bool     _ran = false;

public:
    TestKit()                           = default;
    TestKit(TestKit const &)            = delete;
    TestKit &operator=(TestKit const &) = delete;

    /// The engine, for anything the kit does not wrap: `core(id).setLatency(...)`, `addActor`.
    [[nodiscard]] qb::Main &
    engine() noexcept {
        return _engine;
    }

    /// `engine().addActor<A>(core, args...)`: an actor under test, before `run()`.
    template <typename A, typename... Args>
    qb::ActorId
    add(qb::CoreId core, Args &&...args) {
        return _engine.addActor<A>(core, std::forward<Args>(args)...);
    }

    /// A `TestProbe<Events...>` on @p core: its id is the destination to hand to the actors under test.
    template <typename... Events>
    qb::ActorId
    probe(qb::CoreId core = 0) {
        return add<TestProbe<Events...>>(core);
    }

    /**
     * @brief Run @p scenario on a driver actor on @p core and wait for it, at most @p watchdog.
     * @tparam Asks The exchange types the scenario `qb::ask`s other actors with: the driver registers
     *         them so their replies resolve (`kit.run<Quote>(...)`). `expect` needs none.
     * @param scenario A callable `qb::io::async::task<void>(qb::ScopedCoroContext)`, kept by the driver
     *        for the run: capture by value.
     * @param watchdog The longest the run may take; the engine is stopped when it passes.
     * @param core The core the driver runs on.
     * @throws std::logic_error on a second run, or when the driver cannot be added.
     */
    template <typename... Asks, typename Scenario>
    RunResult
    run(Scenario &&scenario, qb::duration watchdog = std::chrono::seconds(10), qb::CoreId core = 0) {
        using driver_t = detail::ScenarioDriver<std::decay_t<Scenario>, Asks...>;
        static_assert(std::is_invocable_r_v<qb::io::async::task<void>, std::decay_t<Scenario> &, qb::ScopedCoroContext>,
                      "qb::test::TestKit::run: the scenario is a qb::io::async::task<void>(qb::ScopedCoroContext) callable");
        if (std::exchange(_ran, true))
            throw std::logic_error("qb::test::TestKit::run: one run per kit (an engine does not restart)");
        auto state = std::make_shared<detail::ScenarioState>();
        if (!_engine.addActor<driver_t>(core, std::forward<Scenario>(scenario), state).is_valid())
            throw std::logic_error("qb::test::TestKit::run: the scenario driver could not be added");

        RunResult result;
        _engine.start(true);
        {
            std::unique_lock<std::mutex> guard(state->lock);
            result.timed_out = !state->done.wait_for(guard, watchdog, [&state] { return state->finished; });
        }
        qb::Main::stop(); // the driver stopped it already unless the watchdog fired: idempotent, and no join hangs
        _engine.join();
        result.engine_error = _engine.hasError();
        std::lock_guard<std::mutex> guard(state->lock);
        result.completed = state->completed;
        result.error     = state->error;
        return result;
    }
};

} // namespace qb::test

#endif // QB_CORE_TESTKIT_H
