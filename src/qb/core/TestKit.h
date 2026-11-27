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
 *   so the probe may live on any core -- and the probe's clock is the only one: the asker sets no
 *   deadline of its own, and takes the probe's answer however late a stalled host delivers it.
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
#include <qb/core/detail/monotonic_wait.h> // std::condition_variable, its timed wait off the time of day (Huly QB-1227)
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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
    bool          gone     = false; ///< Set by the probe as it ends: it answers nothing else any more
    bool          counted  = false; ///< Set by `expect` / `expect_none`: the census counts it on its way (`probe_census`)
    std::uint64_t lend     = 0;     ///< Set by the probe with an event: the asker acknowledges it (`ExpectationTaken`)
    std::uint64_t token    = 0;     ///< The asker's name for it: a withdrawal names it (`ExpectationWithdrawn`)

    Expectation() = default;
    Expectation(qb::TypeId type, qb::duration window, bool none, std::uint64_t name, bool census = false)
        : event_type(type)
        , wait(window)
        , absence(none)
        , counted(census)
        , token(name) {}
};

/// A probe's reminder to itself that the expectation `ticket` reached its deadline. It comes round twice: a core reads
/// what it sent itself before what the other cores sent it, so the first reminder is handled AHEAD of the events
/// already in the probe's mailbox when its timer fired. `settled` is the second lap, behind them.
struct ExpectationDeadline : qb::Event {
    std::uint64_t ticket  = 0;
    bool          settled = false;

    explicit ExpectationDeadline(std::uint64_t t, bool behind_the_mailbox = false)
        : ticket(t)
        , settled(behind_the_mailbox) {}
};

/// The asker's acknowledgement of the event an `Expectation` carried: the probe forgets it.
struct ExpectationTaken : qb::Event {
    std::uint64_t lend = 0;

    explicit ExpectationTaken(std::uint64_t l)
        : lend(l) {}
};

/// The asker's withdrawal of the expectation `token`: its frame ended without the answer -- the losing branch of a
/// `when_any`, a frame destroyed. The probe forgets its waiter, and an event it lent it comes back at once.
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

/// How long a probe keeps an event it answered with, unacknowledged, before it records it again: the asker
/// acknowledges at once, an expectation it abandons is withdrawn at once (`ExpectationWithdrawn`), and a dead
/// asker's loans come back at its `qb::DownEvent`, so only an asker whose core stalls past it reaches it.
inline constexpr qb::duration lend_hold = std::chrono::seconds(2);

/// The names of the expectations of the process: unique, so a probe tells two of one asker apart. Anchored as the
/// core's other process-wide counters (`QB_ABI_ANCHOR`): one object across the shared images of a program on ELF
/// and Mach-O; on Windows qb has no export macro yet, and each DLL counts its own.
QB_ABI_ANCHOR inline std::atomic<std::uint64_t> expectation_tokens{0};

class ScenarioDriverBase;

/// Who announced a probe: a `TestKit` says so to the constructor of each probe it adds (`TestKit::add`).
struct announced_by {
    void const *kit = nullptr;
};

/// What a second probe at an id meets: `std::logic_error::what()`.
inline constexpr char const *one_probe_per_id =
    "qb::test: a probe or a scenario of another kit, or of another engine, alive in this process holds this id -- one at a time";

/**
 * @brief The probes of the process, and for each the expectations on their way to it: what lets `expect` and
 *        `expect_none` wait for the probe's answer alone, with no deadline of their own.
 * @details An asker that sets no deadline must never ask an id nothing will answer from. A probe is in the census from
 *          the moment its id exists -- a `TestKit` announces the probes it adds, before the engine starts, and a probe
 *          enters it in its constructor otherwise -- until it is destroyed. `departs()` -- one lock, on the asker's
 *          core -- says whether a probe holds the id AND counts the expectation as sent, in one step: an id that
 *          holds none, or one whose probe was killed, is refused before anything is sent. A probe a `TestKit` did
 *          not add is known from its constructor only: an `onInit()` on ANOTHER core that expects on it may run
 *          before it is built, and is refused (a spawned coroutine never is: it runs once every core has built its
 *          actors). A probe that a `qb::KillEvent` ends reads who still has expectations on their way (`ends()`): they
 *          are in a pipe or a mailbox, and it stays until each has arrived (`arrived()`) or its asker is gone
 *          (`lost()`: the probe watches it), answering each that it is gone. Shared by the cores, hence the lock;
 *          anchored like `expectation_tokens` (on Windows each DLL has its own: keep a probe and its askers in one
 *          image). An id is an engine's own, and nothing a coroutine can read names its engine: the census holds ONE
 *          probe per id, and a second -- a probe of another kit, or of another engine, alive in the same process --
 *          is a `std::logic_error` where it is added or built, never a probe silently taken for the other: an
 *          announcement is its kit's alone (the probe says who adds it), a built probe's id is its own. The
 *          census also knows the kits' drivers (`drives()`): an expectation of a scenario is asked through its driver,
 *          which watches the probe (`ScenarioDriverBase::watching`).
 */
class probe_census {
    struct departure {
        qb::ActorId asker;
        std::size_t count;
    };
    struct entry {
        qb::ActorId            id;
        std::vector<departure> on_their_way;        ///< Sent by an `expect` / `expect_none`, not yet received by the probe
        bool                   ending    = false;   ///< The probe was killed: nothing more departs for it
        bool                   built     = false;   ///< Its constructor ran: a second probe built at this id is another engine's
        void const            *announcer = nullptr; ///< The `TestKit` that announced it, if one did
    };
    struct scenario {
        qb::ActorId         id;
        ScenarioDriverBase *driver;
        std::thread::id     thread; ///< Its core's: the only thread its coroutines run on
    };
    std::mutex            _lock;
    std::vector<entry>    _probes;
    std::vector<scenario> _drivers;

    [[nodiscard]] entry *
    find(qb::ActorId id) noexcept {
        const auto it = std::find_if(_probes.begin(), _probes.end(), [id](entry const &e) { return e.id == id; });
        return it == _probes.end() ? nullptr : &*it;
    }
    [[nodiscard]] static std::size_t
    total(entry const &e) noexcept {
        std::size_t n = 0;
        for (departure const &d : e.on_their_way)
            n += d.count;
        return n;
    }

public:
    /// `kit` adds a probe at `id`: expectations may depart for it before its core has built it.
    /// @throws std::logic_error when a probe holds the id already -- another kit's, or another engine's.
    void
    announce(qb::ActorId id, void const *kit) {
        std::lock_guard<std::mutex> guard(_lock);
        if (find(id) != nullptr)
            throw std::logic_error(one_probe_per_id);
        _probes.push_back(entry{id, {}, false, false, kit});
    }
    /// A probe is built at `id`: the one `kit` announced, or one added to an engine by hand (`kit` null).
    /// @throws std::logic_error when a probe built already holds the id, or when another kit than the one that adds
    ///         this probe announced it -- an announcement is its kit's alone, a probe of another engine does not take
    ///         it. Thrown from the probe's constructor: its core refuses to start, or stops if it runs already.
    void
    born(qb::ActorId id, void const *kit) {
        std::lock_guard<std::mutex> guard(_lock);
        if (entry *const e = find(id)) {
            if (e->built || e->announcer != kit)
                throw std::logic_error(one_probe_per_id);
            e->built = true;
            return;
        }
        _probes.push_back(entry{id, {}, false, true, kit});
    }
    /// The probe is destroyed: no probe holds `id` any more.
    void
    gone(qb::ActorId id) noexcept {
        std::lock_guard<std::mutex> guard(_lock);
        if (entry *const e = find(id))
            _probes.erase(_probes.begin() + (e - _probes.data()));
    }
    /// `kit` ends: what it announced and no core ever built -- an engine that did not start -- leaves with it. An
    /// entry another kit made since, at an id this one's probe has left, is that kit's.
    void
    withdraw(qb::ActorId id, void const *kit) noexcept {
        std::lock_guard<std::mutex> guard(_lock);
        if (entry *const e = find(id); e != nullptr && !e->built && e->announcer == kit)
            _probes.erase(_probes.begin() + (e - _probes.data()));
    }
    /// The driver of a kit's scenario lives at `id`, on the thread that calls this.
    /// @throws std::logic_error when a driver holds the id already: two kits run at once.
    void
    drives(qb::ActorId id, ScenarioDriverBase *driver) {
        std::lock_guard<std::mutex> guard(_lock);
        if (std::any_of(_drivers.begin(), _drivers.end(), [id](scenario const &d) { return d.id == id; }))
            throw std::logic_error(one_probe_per_id);
        _drivers.push_back(scenario{id, driver, std::this_thread::get_id()});
    }
    void
    driven(qb::ActorId id) noexcept {
        std::lock_guard<std::mutex> guard(_lock);
        std::erase_if(_drivers, [id](scenario const &d) { return d.id == id; });
    }
    /// The driver that lives at `asker` ON THE CALLING THREAD, or null. A driver's coroutines run on its core's
    /// thread and nowhere else: an actor of another engine that holds the same id is on another thread, and is not
    /// taken for it.
    [[nodiscard]] ScenarioDriverBase *
    driver(qb::ActorId asker) noexcept {
        std::lock_guard<std::mutex> guard(_lock);
        const auto                  here = std::this_thread::get_id();
        const auto it = std::find_if(_drivers.begin(), _drivers.end(), [&](scenario const &d) { return d.id == asker && d.thread == here; });
        return it == _drivers.end() ? nullptr : it->driver;
    }
    /// An expectation of `asker` leaves for `id`, counted on its way: false, and nothing counted, when no probe
    /// holds the id or the one that does was killed.
    [[nodiscard]] bool
    departs(qb::ActorId id, qb::ActorId asker) {
        std::lock_guard<std::mutex> guard(_lock);
        entry *const                e = find(id);
        if (e == nullptr || e->ending)
            return false;
        const auto sent =
            std::find_if(e->on_their_way.begin(), e->on_their_way.end(), [asker](departure const &d) { return d.asker == asker; });
        if (sent == e->on_their_way.end())
            e->on_their_way.push_back(departure{asker, 1});
        else
            ++sent->count;
        return true;
    }
    /// An expectation of `asker` reached the probe `id` (`counted`: one `departs()` counted): how many are still on
    /// their way, of every asker.
    std::size_t
    arrived(qb::ActorId id, qb::ActorId asker, bool counted) noexcept {
        std::lock_guard<std::mutex> guard(_lock);
        entry *const                e = find(id);
        if (e == nullptr)
            return 0;
        const auto sent =
            std::find_if(e->on_their_way.begin(), e->on_their_way.end(), [asker](departure const &d) { return d.asker == asker; });
        if (counted && sent != e->on_their_way.end() && --sent->count == 0)
            e->on_their_way.erase(sent);
        return total(*e);
    }
    /// The probe `id` was killed: nothing more departs for it. The askers whose expectations are still on their way.
    [[nodiscard]] std::vector<qb::ActorId>
    ends(qb::ActorId id) {
        std::lock_guard<std::mutex> guard(_lock);
        std::vector<qb::ActorId>    askers;
        if (entry *const e = find(id)) {
            e->ending = true;
            for (departure const &d : e->on_their_way)
                askers.push_back(d.asker);
        }
        return askers;
    }
    /// `asker` is gone: what it sent the probe `id` that has not arrived never will. How many are still on their way.
    std::size_t
    lost(qb::ActorId id, qb::ActorId asker) noexcept {
        std::lock_guard<std::mutex> guard(_lock);
        entry *const                e = find(id);
        if (e == nullptr)
            return 0;
        std::erase_if(e->on_their_way, [asker](departure const &d) { return d.asker == asker; });
        return total(*e);
    }
};

/// The census of the process. Built on first use and never destroyed: a probe may end after the statics have.
QB_ABI_ANCHOR inline probe_census &
probes() noexcept {
    alignas(probe_census) static unsigned char storage[sizeof(probe_census)];
    static probe_census *const                 census = new (storage) probe_census;
    return *census;
}

/// Withdraws the expectation `token` from `probe` when its frame ends without taking the answer -- unwound by a
/// `when_any` that another branch won, destroyed -- so what the probe holds for it goes to the next
/// expectation at once. A killed asker pushes nothing (its scope is cancelled before its frames
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
    bool                    built     = false; ///< The driver exists, or did: its destructor ends the wait if nothing else does
    bool                    finished  = false;
    bool                    completed = false;
    std::exception_ptr      error;

    void
    finish(bool ran_to_end, std::exception_ptr thrown) { // the first call decides: the frame's, or the driver's destruction
        if (std::lock_guard<std::mutex> guard(lock); !std::exchange(finished, true)) {
            completed = ran_to_end;
            error     = std::move(thrown);
        }
        done.notify_all();
    }
};

/// The scenario behind an interface of its own type. A scenario is usually a lambda written in a test body: a type
/// with no linkage. The coroutine the driver spawns is a frame defined in a header (qb::Actor::spawn's), and a field
/// of such a type there is g++'s -Wsubobject-linkage -- an error under QB_TESTS_WERROR -- in every test that runs a
/// scenario, qb's and a user's. Behind this interface the frame holds a `std::shared_ptr<ScenarioRunner>` only.
struct ScenarioRunner {
    virtual ~ScenarioRunner() = default;
    /// The scenario's task. Not a coroutine itself: the frame is the scenario's own, of the caller's file.
    virtual qb::io::async::task<void> run(qb::ScopedCoroContext ctx) = 0;
};

template <typename Scenario>
struct ScenarioRunnerOf final : ScenarioRunner {
    Scenario scenario;

    explicit ScenarioRunnerOf(Scenario s)
        : scenario(std::move(s)) {}

    qb::io::async::task<void>
    run(qb::ScopedCoroContext ctx) override {
        return scenario(ctx); // the closure lives here, the runner in the spawned frame: as long as the scenario runs
    }
};

/// What every driver does, in code that depends on no scenario type: the spawned coroutine is this class's own.
class ScenarioDriverBase : public qb::Actor {
    /// A probe the scenario expects on, and the scope its expectations on it are asked in.
    struct watched {
        qb::ActorId                       probe;
        qb::io::async::cancellation_token stopped; ///< Cancelled when the probe's core stopped under it
    };
    std::shared_ptr<ScenarioRunner> _runner;
    std::shared_ptr<ScenarioState>  _state;
    std::vector<watched>            _watched;

protected:
    ScenarioDriverBase(std::shared_ptr<ScenarioRunner> runner, std::shared_ptr<ScenarioState> state)
        : _runner(std::move(runner))
        , _state(std::move(state)) {
        probes().drives(id(), this); // a second driver at this id -- two kits at once -- stops here, before `built`
        std::lock_guard<std::mutex> guard(_state->lock);
        _state->built = true;
    }
    ~ScenarioDriverBase() override { // destroyed before its scenario ended: the run ends, not run to its end (Huly QB-1227)
        probes().driven(id());
        _state->finish(false, nullptr);
    }
    void
    start_scenario() {
        spawn([runner = std::move(_runner), state = _state](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await runner->run(ctx);
                state->finish(true, nullptr);
            } catch (...) {
                state->finish(false, std::current_exception());
            }
            qb::Main::stop();
        });
    }

public:
    void
    on(Expectation &e) {
        (void) resolve_ask(e); // a reply to the scenario's expect; anything else is dropped
    }

    /// The context an expectation of this scenario on `probe` is asked in. A probe on a core that stops on an error --
    /// an exception out of a handler -- answers nobody: that core sends nothing more. The driver watches each probe
    /// its scenario expects on, and a watch IS answered then (`core_stopped`), behind everything that core had sent:
    /// the scope is a child of the driver's own, cancelled when that answer comes, and the expectations waiting in it
    /// end -- `expect` reads which of the two scopes ended.
    [[nodiscard]] qb::ScopedCoroContext
    watching(qb::ActorId probe) {
        auto it = std::find_if(_watched.begin(), _watched.end(), [probe](watched const &w) { return w.probe == probe; });
        if (it == _watched.end()) {
            it = _watched.insert(_watched.end(), watched{probe, context().child_token()});
            watch(probe); // ahead of the first expectation, in the one pipe to the probe's core
        }
        return qb::ScopedCoroContext{this, it->stopped};
    }

    /// The answer of a watch -- the core's own, whose source is the actor it names: a `qb::DownEvent` another actor
    /// sends the driver ends nothing. Only a core's stop ends the scope: a probe that a `qb::KillEvent` ended has
    /// answered for itself (`TestProbe::on(qb::KillEvent const &)`), and one that stops with its engine stops with
    /// this driver.
    void
    on(qb::DownEvent const &e) {
        if (e.reason != qb::DownReason::core_stopped || e.getSource() != e.watched)
            return;
        const auto it = std::find_if(_watched.begin(), _watched.end(), [&e](watched const &w) { return w.probe == e.watched; });
        if (it != _watched.end())
            it->stopped.cancel();
    }
};

/**
 * @brief The actor a `TestKit` adds to run its scenario: it registers the exchanges the scenario asks
 *        (`Expectation` and the `Asks` named to `run<Asks...>()`), so their replies resolve.
 */
template <typename Scenario, typename... Asks>
class ScenarioDriver final : public ScenarioDriverBase {
public:
    ScenarioDriver(Scenario scenario, std::shared_ptr<ScenarioState> state)
        : ScenarioDriverBase(std::make_shared<ScenarioRunnerOf<Scenario>>(std::move(scenario)), std::move(state)) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Expectation>(*this);
        registerEvent<qb::DownEvent>(*this); // the watch of the probes the scenario expects on
        (registerEvent<Asks>(*this), ...);
        start_scenario();
        co_return true;
    }

    using ScenarioDriverBase::on;

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
 * @brief What `expect` and `expect_none` throw when no probe can answer: no probe holds the id -- it ended, or the
 *        id never was a probe's -- or the probe was killed while the expectation waited on it or was on its way.
 * @details A probe answers every expectation that reaches it, once: with an event, at its deadline, or -- killed
 *          meanwhile -- with this. Neither "an event arrived" nor "none did": a test asserts one or the other, and a
 *          probe that is gone proves neither, so no return value of `expect` or `expect_none` stands for it. One type
 *          for the two: an id tells nothing of what held it, so the kit cannot say "never a probe" from "a probe that
 *          ended", and a test does the same with both -- it fails. `what()` says which the kit met. Not a
 *          `std::logic_error`, which the kit keeps for what only the code can be wrong about (a type the probe does not
 *          record): a probe ends at run time.
 */
class probe_gone_error : public std::runtime_error {
public:
    explicit probe_gone_error(char const *met)
        : std::runtime_error(met) {}
};

namespace detail {
/// `probe_gone_error::what()` when the census refused the id, before anything was sent.
inline constexpr char const *no_live_probe =
    "qb::test: no probe takes expectations at this id -- it ended, it was killed, or it never was a probe";
/// `probe_gone_error::what()` when the probe said so itself, killed.
inline constexpr char const *probe_ended = "qb::test: the probe was killed while this expectation waited on it";
/// `probe_gone_error::what()` when the scenario's driver heard the probe's core stop.
inline constexpr char const *probe_core_stopped = "qb::test: the probe's core stopped on an error while this expectation waited on it";

/// The scope an expectation of `ctx`'s actor on `probe` is asked in: the driver's watch of that probe when the actor
/// is a kit's driver, its own scope otherwise.
[[nodiscard]] inline qb::ScopedCoroContext
asking(qb::ScopedCoroContext const &ctx, qb::ActorId probe) {
    ScenarioDriverBase *const driver = probes().driver(ctx.id());
    return driver != nullptr ? driver->watching(probe) : ctx;
}
} // namespace detail

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
 *          abandons -- the losing branch of a `when_any`, a frame destroyed -- is withdrawn as its frame
 *          ends: its waiter is forgotten, and an event lent to it returns at once to its place in the queue, in
 *          arrival order, for the next expectation. The probe watches every actor that asks it (`watch()`): when
 *          one is gone, its waiters are dropped and what it was lent returns the same way, oldest event first. A
 *          loan nobody acknowledges otherwise -- an asker whose core stalls -- returns after `detail::lend_hold`
 *          (2 s). A probe may record `qb::DownEvent` too: it records the ones it is sent, and keeps the answers of
 *          its own watches -- a DownEvent about an asker it watches whose source is that asker -- to itself. A
 *          DownEvent of that shape it cannot tell from one: an asker that sends one about itself (a `forward()`
 *          keeps the source) is taken as gone: what waits for it is dropped, unanswered. That asker apart, the probe
 *          answers every expectation that reaches it, once, and its clock is the only one an expectation is timed
 *          on: with an event, at the deadline, or, when a `qb::KillEvent` ends it first, that it is gone
 *          (`probe_gone_error` on the asker's side). The deadline is wall time on the probe's core: the probe answers
 *          it once it has read the events ADDRESSED TO IT that had reached that core by then, so an event that waited
 *          in its mailbox through a stall is an arrival -- but one that the same stall kept an actor from sending in
 *          time may be read as late, as by any timeout. A kill closes the probe to new expectations (`expect` throws at once,
 *          nothing sent), answers the ones waiting, and keeps it up until each one already sent has arrived and been
 *          answered the same, or its asker is gone. A probe that ends otherwise says nothing: stopped with its
 *          engine, its askers stop with it; on a core that stops on an error -- an exception out of a handler -- it
 *          can send nothing; `kill()` called on it through a handle skips the handler. A `TestKit`'s driver hears the
 *          second of those -- it watches the probes its scenario expects on, and a watch is answered when a core
 *          stops, behind all that core sent -- and the scenario's expectations on that probe throw `probe_gone_error`.
 *          An expectation of any other actor waiting on it then, or on its way, waits; the next `expect` throws at
 *          once. An answer goes to its asker whatever became of it: to one that died meanwhile it is a dead letter
 *          on that asker's core, as any event sent to a dead actor. A probe that records `qb::KillEvent` records the
 *          ones it is sent, and ends with the engine. The queues are unbounded -- a probe is for tests, not for traffic.
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
    bool                                 _ending      = false; ///< Killed: up only until what was sent to it has arrived

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

    /// The probe was killed: every expectation still waiting is told so -- neither an event nor a quiet window.
    void
    answer_gone() noexcept {
        for (auto &q : _queues) {
            for (waiter const &w : q.waiting) {
                auto &reply          = push<detail::Expectation>(w.asker);
                reply.correlation_id = w.correlation;
                reply.absence        = w.absence;
                reply.gone           = true;
            }
            q.waiting.clear();
        }
    }

public:
    /// A probe added to an engine by hand: it enters the census here.
    TestProbe() {
        detail::probes().born(id(), nullptr);
    }
    /// A probe a `TestKit` adds: the one that kit announced, and no other's (see `detail::probe_census`).
    explicit TestProbe(detail::announced_by by) {
        detail::probes().born(id(), by.kit);
    }

    /// Every end of a probe passes here, and it says nothing: a kill has answered everything by now
    /// (`on(qb::KillEvent const &)`), an engine that stops takes the askers with it -- an answer pushed from here
    /// reached actors the same stop had just ended, a dead letter of the kit's own exchange each -- and a core that
    /// stopped on an error sends nothing. What every end does: no expectation departs for this id any more.
    ~TestProbe() noexcept override {
        detail::probes().gone(id());
    }

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
        if constexpr (!(std::is_same_v<qb::KillEvent, Events> || ...))
            registerEvent<qb::KillEvent>(*this); // the probe's own end; a recorded KillEvent is registered above
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
        const auto on_their_way = detail::probes().arrived(id(), e.getSource(), e.counted);
        if (_ending) { // sent before the kill closed the probe: answered like the ones that waited, then it may go
            e.gone = true;
            reply(e);
            if (on_their_way == 0)
                kill();
            return;
        }
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

    /// The deadline of the expectation `e.ticket`. Answered on the reminder's second lap only: the first is read from
    /// this core's own pipe, ahead of what the other cores sent in the meantime -- after a stall of this core, an
    /// event that reached its mailbox within the window would be recorded AFTER the probe had answered "none".
    void
    on(detail::ExpectationDeadline const &e) {
        for (auto &q : _queues)
            for (auto it = q.waiting.begin(); it != q.waiting.end(); ++it)
                if (it->ticket == e.ticket) {
                    if (!e.settled) {
                        push<detail::ExpectationDeadline>(id(), e.ticket, true); // behind this pass's mailbox
                        return;
                    }
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
    /// parked meanwhile takes the events in arrival order. Any other DownEvent touches no asker: it is recorded when
    /// the probe records the type. The shape is all the probe can read: a DownEvent an asker sends about itself,
    /// or forwards (the source kept), is taken as the core's.
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
        if (_ending && detail::probes().lost(id(), asker) == 0)
            kill(); // killed, and the last expectation it waited for went with its asker
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

    /// A kill. An asker waits for the probe's answer and for nothing else -- it times nothing itself -- so the probe
    /// leaves no expectation behind: it closes (the census refuses what would depart now, and `expect` throws at
    /// once), answers every expectation waiting that it is gone, and stays up until the ones already sent -- in a
    /// pipe, in a mailbox -- have arrived and been answered the same (`on(detail::Expectation &)`), or their asker
    /// is gone: it watches each asker it waits for, and what a core lost with its actors never arrives
    /// (`on(qb::DownEvent const &)`). With none on its way it dies here. A probe that records `qb::KillEvent`
    /// records this one like any event it is sent.
    void
    on(qb::KillEvent const &e) {
        if constexpr ((std::is_same_v<qb::KillEvent, Events> || ...)) {
            record(_queues[index_of<qb::KillEvent>()], entry{++_next_seq, std::make_shared<qb::KillEvent const>(e)});
        } else {
            (void) e;
            if (std::exchange(_ending, true))
                return; // killed again while it waits for what is on its way
            const auto askers = detail::probes().ends(id());
            answer_gone();
            if (askers.empty()) {
                kill();
                return;
            }
            for (const auto asker : askers)
                if (std::find(_askers.begin(), _askers.end(), asker) == _askers.end()) {
                    _askers.push_back(asker); // its first expectation is still on its way: not watched yet
                    watch(asker);
                }
        }
    }
};

/**
 * @brief The next `E` the probe recorded -- at once when one is waiting, else the first to arrive
 *        within @p timeout -- or an empty optional.
 * @tparam E A type the probe records.
 * @param ctx The scenario's context (any `spawn`ed coroutine of an actor that registers
 *            `qb::test::detail::Expectation`, as a `TestKit` driver does).
 * @param probe A `TestProbe` recording `E`, on any core.
 * @param timeout How long the PROBE waits for one, on its own clock; zero or less takes only an `E` already
 *        recorded. The asker times nothing: it waits for the probe's answer, however late a stalled host delivers it.
 * @return The event, a copy; empty when none reached the probe in time.
 * @throws std::logic_error when the probe does not record `E` -- a test written against the wrong probe.
 * @throws qb::test::probe_gone_error at once, nothing sent, when no live probe holds @p probe (it ended, or the id never
 *         was a probe's); when a `qb::KillEvent` ends the probe while the expectation waits on it or is on its way; and,
 *         in a `TestKit`'s scenario, when the probe's core stops on an error under it.
 * @throws qb::io::async::cancelled_error when the asking actor is killed, before or meanwhile.
 * @note Under a `when_any` an expectation that throws WINS the race, and the exception stays in the result: read the
 *       winner with `get<T>()` (it rethrows) or test `has_exception()` -- `index` alone does not tell an answer from an
 *       error. Out of a `spawn` body of an actor that is not the kit's driver, the error is reported and dropped.
 * @note A probe that ends otherwise than by a `qb::KillEvent` answers nobody: an expectation waiting on it, or on its
 *       way to it, gets no answer from it -- and is never read as "none". Stopped with its engine, the asker stops too
 *       (one that outlives the stop waits). On a core that stops on an error (an exception out of a handler there), a
 *       `TestKit`'s scenario is told -- its driver watches the probe -- and the expectation throws; an expectation of
 *       an actor that is not the kit's driver waits for as long as that actor lives.
 * @note Call it from a spawned coroutine. From an `onInit()`, a probe on ANOTHER core that no `TestKit` added may not
 *       be built yet: the census does not know it, and `expect` throws.
 */
template <typename E>
[[nodiscard]] qb::io::async::task<std::optional<E>>
expect(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration timeout) {
    static_assert(std::is_base_of_v<qb::Event, E> && std::is_copy_constructible_v<E>, "qb::test::expect: E is a copyable qb::Event");
    if (ctx.cancelled())
        throw qb::io::async::cancelled_error(); // the asker is being killed: nothing departs (`qb::ask` would send nothing)
    if (!detail::probes().departs(probe, ctx.id()))
        throw probe_gone_error(detail::no_live_probe); // nothing sent: nothing would answer
    // Counted as sent from here. Nothing below stops before the expectation is in the pipe but a failed allocation,
    // and a count that never arrives ends with its asker: a killed probe watches the askers it waits for.
    const auto             wait  = timeout > qb::duration::zero() ? timeout : qb::duration::zero();
    const auto             token = ++detail::expectation_tokens;
    detail::WithdrawOnExit withdraw{ctx, probe, token};
    // No deadline of the asker's (zero: `qb::ask` waits for the reply): the probe's clock is the only one. A timeout
    // of its own here -- the probe's wait plus a grace -- came due in the same pass as a late answer, and ran first.
    std::optional<detail::Expectation> met;
    try {
        met.emplace(co_await qb::ask(detail::asking(ctx, probe), probe, detail::Expectation{qb::type_id<E>(), wait, false, token, true},
                                     qb::duration::zero()));
    } catch (qb::io::async::cancelled_error const &) {
        withdraw.armed = false; // nobody is left to withdraw from, or to withdraw
        if (ctx.cancelled())
            throw;                                          // the asker is being killed
        throw probe_gone_error(detail::probe_core_stopped); // the scope its driver watches the probe in ended alone
    }
    auto const &answer = *met;
    withdraw.armed     = false; // answered: the probe holds nothing more for it
    if (answer.gone)
        throw probe_gone_error(detail::probe_ended);
    if (!answer.recorded)
        throw std::logic_error("qb::test::expect: the probe does not record this event type");
    if (!answer.response)
        co_return std::nullopt;
    if (answer.lend != 0)
        ctx.push_to<detail::ExpectationTaken>(probe, answer.lend); // taken: the probe forgets it
    co_return *static_cast<E const *>(answer.response.get());
}

/**
 * @brief Whether no `E` reached the probe during @p window: false at once when an `E` is already
 *        recorded and not consumed, false the moment one arrives, true at the end of the window.
 * @tparam E A type the probe records.
 * @param ctx The scenario's context (see `expect`).
 * @param probe A `TestProbe` recording `E`, on any core.
 * @param window How long the PROBE watches, on its own clock: the asker times nothing (see `expect`).
 * @return true when none arrived; false when one did. Each is the probe's own answer, never the asker's guess.
 *         Nothing is consumed: a following `expect<E>` still gets the event.
 * @throws std::logic_error when the probe does not record `E`.
 * @throws qb::test::probe_gone_error at once when no live probe holds @p probe, when the probe is killed meanwhile, and
 *         in a `TestKit`'s scenario when its core stops on an error (see `expect`): neither true nor false stands for
 *         a probe that is gone.
 * @throws qb::io::async::cancelled_error when the asking actor is killed, before or meanwhile.
 * @note A probe that ends otherwise than by a `qb::KillEvent` answers nobody: see `expect`.
 */
template <typename E>
[[nodiscard]] qb::io::async::task<bool>
expect_none(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration window) {
    static_assert(std::is_base_of_v<qb::Event, E>, "qb::test::expect_none: E is a qb::Event");
    if (ctx.cancelled())
        throw qb::io::async::cancelled_error();
    if (!detail::probes().departs(probe, ctx.id()))
        throw probe_gone_error(detail::no_live_probe);
    const auto             wait  = window > qb::duration::zero() ? window : qb::duration::zero();
    const auto             token = ++detail::expectation_tokens;
    detail::WithdrawOnExit withdraw{ctx, probe, token};
    // No deadline of the asker's: its timeout was read as "an event arrived" (see `expect`).
    std::optional<detail::Expectation> met;
    try {
        met.emplace(co_await qb::ask(detail::asking(ctx, probe), probe, detail::Expectation{qb::type_id<E>(), wait, true, token, true},
                                     qb::duration::zero()));
    } catch (qb::io::async::cancelled_error const &) {
        withdraw.armed = false;
        if (ctx.cancelled())
            throw;
        throw probe_gone_error(detail::probe_core_stopped);
    }
    auto const &answer = *met;
    withdraw.armed     = false;
    if (answer.gone)
        throw probe_gone_error(detail::probe_ended);
    if (!answer.recorded)
        throw std::logic_error("qb::test::expect_none: the probe does not record this event type");
    co_return !answer.response;
}

namespace detail {
/// Whether `A` is a `TestProbe`: a kit announces the ones it adds to the census (`probe_census`).
template <typename A>
inline constexpr bool is_probe = false;
template <typename... Events>
inline constexpr bool is_probe<TestProbe<Events...>> = true;
} // namespace detail

/**
 * @brief An engine for one test: add the actors and probes, then `run()` one scenario.
 * @details `run()` adds the driver, starts the engine on its own threads, waits for the scenario to
 *          end -- the driver stops the engine at once when it returns or throws -- for the watchdog, or for the
 *          driver's end (an engine stopped first), then joins and reads `hasError()`. A core other than the driver's that
 *          stops on an error ends nothing: the scenario runs on, and `RunResult::engine_error` says so. The watchdog's
 *          verdict is final: what a scenario still does before its core reads the stop is not reported. One run per
 *          kit: an engine does not restart.
 *          `qb::Main::stop()` stops every engine of the process, and a probe's id is known to the kit by itself,
 *          not by its engine: one kit alive at a time -- built, run, destroyed, then the next. A probe added while
 *          another kit's probe holds the same id throws `std::logic_error`. Configure
 *          the cores through `engine()` before `run()` (a core runs at latency 0, spinning, by default).
 */
class TestKit {
    qb::Main                 _engine;
    std::vector<qb::ActorId> _probes; ///< Announced to the census as they are added: see `add()`
    bool                     _ran = false;

public:
    TestKit()                           = default;
    TestKit(TestKit const &)            = delete;
    TestKit &operator=(TestKit const &) = delete;
    ~TestKit() {
        for (const auto id : _probes)
            detail::probes().withdraw(id, this); // one no core ever built -- an engine that did not start -- leaves here
    }

    /// The engine, for anything the kit does not wrap: `core(id).setLatency(...)`, `addActor`.
    [[nodiscard]] qb::Main &
    engine() noexcept {
        return _engine;
    }

    /// `engine().addActor<A>(core, args...)`: an actor under test, before `run()`. A `TestProbe` added here is
    /// announced to the census at once: an `expect` on it may run before its core has built it (an `onInit()`).
    /// @throws std::logic_error when a probe of another kit, or of another engine, alive in the process holds the id
    ///         it was given: probes of one kit at a time.
    template <typename A, typename... Args>
    qb::ActorId
    add(qb::CoreId core, Args &&...args) {
        if constexpr (detail::is_probe<A>) {
            static_assert(sizeof...(Args) == 0, "qb::test::TestKit::add: a TestProbe takes no argument");
            const auto id = _engine.addActor<A>(core, detail::announced_by{this}); // the probe is this kit's: it says so as it is built
            if (id.is_valid()) {
                detail::probes().announce(id, this);
                _probes.push_back(id);
            }
            return id;
        } else {
            return _engine.addActor<A>(core, std::forward<Args>(args)...);
        }
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
     * @param scenario A callable `qb::io::async::task<void>(qb::ScopedCoroContext)`, kept by the driver for the run: capture by value.
     * @param watchdog The longest the run may take, stopped when it passes; `qb::duration::max()`: none. An engine stopped first ends it.
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
        // A start that was refused before the driver was built leaves nobody to end the wait: end it here. A driver that
        // was built ends it itself, at its destruction at the latest -- and `hasError()` alone does not say "refused":
        // a core that stops on an exception in its first passes sets it too, under a scenario that runs on.
        bool refused_before_the_driver = false;
        if (_engine.hasError()) {
            std::lock_guard<std::mutex> guard(state->lock);
            refused_before_the_driver = !state->built;
        }
        if (refused_before_the_driver)
            state->finish(false, nullptr);
        {
            std::unique_lock<std::mutex> guard(state->lock);
            result.timed_out = !qb::detail::wait_for_monotonic(state->done, guard, watchdog, [&state] { return state->finished; });
            // The watchdog's verdict is the run's, decided here: a scenario its core resumes before it reads the stop --
            // an answer was ahead of it -- and that returns or throws then, changes nothing in the result.
            state->finished = true;
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
