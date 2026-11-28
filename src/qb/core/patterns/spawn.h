/**
 * @file qb/core/patterns/spawn.h
 * @brief Create an actor on ANOTHER core while the engine runs: `qb::spawn_on<A>(ctx, core, timeout,
 *        args...)` and the per-core `qb::Spawner` service it asks (since 3.4, Huly QB-54).
 *
 * `Main::addActor` works before `start()` only, and `Actor::addRefActor` creates on the caller's own
 * core: an elastic pool, or a shard moved to a core with room, used to need a hand-written factory
 * actor per core. `spawn_on` asks the `qb::Spawner` of the target core to run `addRefActor<A>(args...)`
 * there -- the core's own admission (the reservation, the constructor, `onInit()`), on its own thread --
 * and answers once the actor is active, or why it is not. No actor pointer crosses a core: only its id.
 *
 * The answer is the `Spawner`'s, always: `spawn_on` sets no deadline of its own. The timeout is the `Spawner`'s wait
 * for the new actor's `onInit()`, on the target core; a host that stalls delays the answer and never stands in for
 * it. `unreachable` is a fact, not the end of a wait: no `Spawner` took the request, and nothing was created -- none
 * was added on that core, the core does not exist, or the one that was asked ended before the request reached it.
 *
 * The constructor arguments travel by value: they are moved into a type-erased factory on the calling
 * core, and moved into the constructor on the target core. Only the factory's pointer crosses the
 * mailbox, so any movable argument works -- a `std::string` included -- but none may refer to the
 * calling actor's state.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 * @ingroup Patterns
 */

#ifndef QB_CORE_PATTERNS_SPAWN_H
#define QB_CORE_PATTERNS_SPAWN_H

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <qb/core/Actor.h>
#include <qb/core/VirtualCore.h> // addRefActor / registerEvent / spawn bodies (the tail of VirtualCore.h)
#include <qb/core/patterns/request.h>
#include <qb/io.h> // QB_LOG_WARN
#include <qb/io/async/coroutine.h>
#include <qb/system/time.h>

namespace qb {

/**
 * @enum spawn_status
 * @ingroup Patterns
 * @brief How a `spawn_on` ended.
 */
enum class spawn_status : std::uint8_t {
    spawned,    ///< Created on the target core and active: its `onInit()` returned true
    refused,    ///< The target core admitted nothing: no slot left, a service of that tag already there, tearing down,
                ///< a constructor that threw, or an `onInit()` that failed before its first suspension (the core rolls
                ///< that admission back)
    not_ready,  ///< Created, but not known active: its async init failed, threw, or still runs when the timeout passes
                ///< on the target core or when the `Spawner` is killed, it ended itself as soon as it was active -- or
                ///< the `Spawner` created it and ended at once, its answer unsent or unread (the id is valid)
    unreachable ///< No `qb::Spawner` took the request, and nothing was created: none on that core, a core the engine
                ///< does not run, or one that ended before the request reached it
};

/**
 * @struct spawn_result
 * @ingroup Patterns
 * @brief What `spawn_on` yields: the new actor's id and the outcome. `true` only for `spawned`.
 */
struct spawn_result {
    qb::ActorId  id{};                               ///< The new actor; valid for `spawned` and `not_ready`
    spawn_status status = spawn_status::unreachable; ///< The outcome

    [[nodiscard]] explicit
    operator bool() const noexcept {
        return status == spawn_status::spawned;
    }
};

namespace detail {

/// The constructor of an actor and its arguments, run once on the target core (Huly QB-54). The request and the caller
/// that sent it share it: what the `Spawner` learns and the caller must know even when no answer comes back is here.
struct spawn_factory_base {
    /// The last `Spawner` born when the request was sent (`spawner_census::seen`): one born later was never asked,
    /// and creates nothing for it. A request built by hand names none, and any `Spawner` serves it.
    std::uint64_t stamp = std::numeric_limits<std::uint64_t>::max();
    /// The actor a `Spawner` created for this request, as `ActorId::raw64()`; 0 until then. What `spawn_on` returns
    /// of a `Spawner` that ended with its answer unsent.
    std::atomic<std::uint64_t> created{0};
    /// A `Spawner` took this request and its core admitted nothing: the same, for a refusal that never left.
    std::atomic<bool> refused{false};

    virtual ~spawn_factory_base() = default;
    /// `addRefActor` on @p spawner's core; the new id, or an invalid one when the core refused.
    virtual qb::ActorId create(qb::Actor const &spawner) = 0;
    /// Whether the created actor becomes active within @p wait (false at once when it never will).
    virtual qb::io::async::task<bool> ready(qb::ScopedCoroContext ctx, qb::duration wait) const = 0;
    /// Whether the created actor is active now, read on its own core.
    [[nodiscard]] virtual bool active() const noexcept = 0;
};

template <class A, class... Args>
class spawn_factory final : public spawn_factory_base {
    std::tuple<Args...> _args;
    qb::ActorHandle<A>  _handle;
    std::atomic<bool>   _created{false}; ///< A request copied to two `Spawner`s shares this factory between two cores

public:
    explicit spawn_factory(Args &&...args)
        : _args(std::move(args)...) {}

    qb::ActorId
    create(qb::Actor const &spawner) override {
        if (_created.exchange(true))
            return {}; // one construction per request: the arguments were moved out
        _handle = std::apply([&spawner](Args &...args) { return spawner.template addRefActor<A>(std::move(args)...); }, _args);
        return _handle.id();
    }

    qb::io::async::task<bool>
    ready(qb::ScopedCoroContext ctx, qb::duration wait) const override {
        return _handle.ready_async(std::move(ctx), wait);
    }

    [[nodiscard]] bool
    active() const noexcept override {
        return _handle.ready();
    }
};

/**
 * @brief The `Spawner`s of the process: what lets `spawn_on` wait for a `Spawner`'s answer alone, with no deadline
 *        of its own (Huly QB-54).
 * @details An asker that sets no deadline must know when nothing will answer. A `Spawner` is in the census from its
 *          constructor to its destructor, under the service id of its core -- which `spawn_on` computes without
 *          asking anyone -- and under its BIRTH, a number no other `Spawner` of the process has. `seen()` is read
 *          before the request leaves: no `Spawner` there, `unreachable` at once; else the request is stamped with
 *          the last birth so far. `holds()` is read while it waits, for that stamp: a `Spawner` born later -- a
 *          second one built on the core once the first was killed -- stands for nobody that was asked before it.
 *          Shared by the cores, hence the lock; one object across the shared images of a program on ELF and Mach-O
 *          (`QB_ABI_ANCHOR`), one per DLL on Windows, where qb has no export macro yet: keep a `Spawner` and its
 *          callers in one image. An `ActorId` names a place in ONE engine: two engines alive at once in one process
 *          that both run a `Spawner` on the same core index hold the same id, each under its own birth, and a
 *          caller is given no way to tell them apart here (its engine is the kernel's to name) -- a `spawn_on` whose own engine runs no
 * `Spawner` on that core, or whose `Spawner` ends with the request still on its way, waits for as long as the other engine's lives.
 */
class spawner_census {
    struct entry {
        qb::ActorId   id;
        std::uint64_t born;
    };
    mutable std::mutex _lock;
    std::vector<entry> _spawners;
    std::uint64_t      _births = 0;

    [[nodiscard]] bool
    held(qb::ActorId id, std::uint64_t stamp) const noexcept {
        return std::any_of(_spawners.begin(), _spawners.end(), [id, stamp](entry const &e) { return e.id == id && e.born <= stamp; });
    }

public:
    /// A `Spawner` holds `id` from now on. Its birth: greater than every one before it.
    [[nodiscard]] std::uint64_t
    enter(qb::ActorId id) {
        std::lock_guard<std::mutex> guard(_lock);
        _spawners.push_back(entry{id, ++_births});
        return _births;
    }
    /// The `Spawner` born `born` is destroyed.
    void
    leave(std::uint64_t born) {
        std::lock_guard<std::mutex> guard(_lock);
        const auto                  it = std::find_if(_spawners.begin(), _spawners.end(), [born](entry const &e) { return e.born == born; });
        if (it != _spawners.end())
            _spawners.erase(it);
    }
    /// The stamp of a request sent now: the last birth so far when a `Spawner` holds `id`, 0 when none does.
    [[nodiscard]] std::uint64_t
    seen(qb::ActorId id) {
        std::lock_guard<std::mutex> guard(_lock);
        return held(id, _births) ? _births : 0;
    }
    /// Whether a `Spawner` born at or before `stamp` still holds `id`: one that a request stamped `stamp` was sent to.
    [[nodiscard]] bool
    holds(qb::ActorId id, std::uint64_t stamp) {
        std::lock_guard<std::mutex> guard(_lock);
        return held(id, stamp);
    }
};

/// The census of the process. Built on first use and never destroyed: a `Spawner` may end after the statics have.
QB_ABI_ANCHOR inline spawner_census &
spawners() noexcept {
    alignas(spawner_census) static unsigned char storage[sizeof(spawner_census)];
    static spawner_census *const                 census = new (storage) spawner_census;
    return *census;
}

/// How often a waiting `spawn_on` reads whether a `Spawner` it asked still exists. Not a deadline: the reading decides
/// nothing while one does, however long its answer takes. A core that parks wakes for it, for its idle spin.
inline constexpr qb::duration spawner_watch_period = std::chrono::milliseconds(100);

/**
 * @brief A wait on the core's own clock that only a PASS of the core ends: `co_await core_wait{d}`.
 * @details A request deadline (`request_deadline`), like the timeout of a `qb::ask`: linked in the core's list, fired
 *          at the head of a pass, resumed through the scheduler in that pass's run. Two things a timer or a yield
 *          does not give `spawn_on`. A pump of the loop does not end it: an `onInit()` that runs its thread's loop
 *          before the engine's start barrier (`qb::io::async::run_for`, a module's blocking call) resumes every
 *          timer and every yield of that thread, and no deadline -- a wait of zero is "the next pass of this core",
 *          by which every core has built its actors. And where it resumes is known: in a pass's turn of the loop,
 *          ahead of that pass's receive and behind the receive of the pass before -- a timer may fire while a core
 *          parks, or under a pump, with no receive on either side. (A turn of the loop resumes 65536 coroutines at
 *          most: past that many ready ahead of it, this one waits for the next resumption like any other.) It arms
 *          no `ev_timer`. Not cancel-aware: what waits on it reads the scope when it resumes, or is raced against a
 *          wait that does and reclaimed with it.
 */
struct core_wait {
    static constexpr char const *qb_suspension_kind = "spawn_on pass"; ///< suspension tracking (coroutine/tracking.h)

    qb::duration            wait;
    request_deadline        deadline{};
    std::coroutine_handle<> cont;

    explicit core_wait(qb::duration d) noexcept
        : wait(d) {}
    core_wait(const core_wait &)            = delete;
    core_wait(core_wait &&)                 = delete;
    core_wait &operator=(const core_wait &) = delete;
    core_wait &operator=(core_wait &&)      = delete;
    ~core_wait() {
        deadline_disarm(deadline); // a frame destroyed while it waits: the core's list no longer names it
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> h) noexcept {
        ::qb::io::async::detail::track_suspension(h.address(), qb_suspension_kind);
        cont = h;
        deadline_arm(deadline, wait, &core_wait::on_pass, this);
    }
    void
    await_resume() const noexcept {}

private:
    // Fired by the core's pass (`deadlines_check`), outside any handler; the node is already unlinked.
    static void
    on_pass(void *self) noexcept {
        qb::io::async::schedule_via_current(static_cast<core_wait *>(self)->cont);
    }
};

/// A `Spawner`'s wait for one actor's activation ended: its coroutine tells the `Spawner`, which reads what the actor
/// is and answers the asker.
struct SpawnSettled : qb::Event {
    std::uint64_t ticket = 0;

    explicit SpawnSettled(std::uint64_t t)
        : ticket(t) {}
};

} // namespace detail

/**
 * @struct SpawnRequest
 * @ingroup Patterns
 * @brief The exchange `spawn_on` asks a `qb::Spawner` with. Register it in the actor that calls
 *        `spawn_on`, as for any `qb::ask` exchange, so the answer resolves:
 *        `registerEvent<qb::SpawnRequest>(*this);` and `void on(qb::SpawnRequest &e) { resolve_ask(e); }`.
 */
struct SpawnRequest : qb::Request<spawn_result> {
    std::shared_ptr<detail::spawn_factory_base> factory;           ///< The constructor and its arguments
    qb::duration                                activation_wait{}; ///< How long the spawner waits for `onInit()`

    SpawnRequest() = default;
    SpawnRequest(std::shared_ptr<detail::spawn_factory_base> f, qb::duration wait)
        : factory(std::move(f))
        , activation_wait(wait) {}
};

template <class A, class... Args>
[[nodiscard]] qb::io::async::task<spawn_result> spawn_on(qb::ScopedCoroContext ctx, qb::CoreId core, qb::duration timeout, Args... args);

/**
 * @class Spawner
 * @ingroup Patterns
 * @brief The per-core service `spawn_on` asks: add one to every core that accepts actors created at
 *        run time -- `engine.addActor<qb::Spawner>(core)` before `start()`.
 * @details On a `SpawnRequest` it runs `addRefActor` on its own core (the core's admission: a refused
 *          reservation, a constructor that throws, or an `onInit()` failing before it suspends, answers `refused` at
 *          once), then waits, in a coroutine of its own, for the new actor to become active -- event-driven,
 *          `ActorHandle::ready_async`, bounded by the request's timeout on THIS core's clock -- and answers `spawned`,
 *          or `not_ready` with the id: what the actor is when the answer is written, whatever the wait saw. A core
 *          held past the timeout does not report `not_ready` an actor that is active, nor `spawned` one that
 *          ended itself meanwhile. It answers every request it handles, once. Killed
 *          (`qb::KillEvent`), it answers the requests still waiting the same way -- their activation is no longer
 *          its to wait for -- and any it reads before the pass it ends in `unreachable` (one read in that last
 *          pass is answered by nobody: `spawn_on` reads the `Spawner` gone, a raw `qb::ask` needs a deadline of its
 *          own). It dies once every core it ever told what an actor is, and its own, has read what it was sent:
 *          each is asked, behind the answers, a question the core itself answers -- the death watch of an id
 *          nobody holds there -- and one that has stopped is known to have. The census does not lose a `Spawner`
 *          whose word on an actor is still to be read. The new actor manages its own lifecycle: the spawner does
 *          not own it, and a spawner killed meanwhile kills nothing but its wait.
 *          A `Spawner` that ends AT ONCE -- with its engine (a terminal signal), by a `kill()` called on it
 *          directly, or on a core that stops on an error -- answers nothing more and waits for nobody: what it had
 *          pushed may still be on its core. `spawn_on` reads that it is gone, and returns what the request itself
 *          says -- an actor created, a refusal, or neither. The events of its own working count only from where
 *          they must come: a settlement from itself, a core's answer from a core it asked.
 */
class Spawner : public qb::ServiceActor<Spawner> {
    struct pending {
        std::uint64_t                               ticket;      ///< Matches the settlement of its wait
        qb::ActorId                                 asker;       ///< Where the answer goes
        std::uint64_t                               correlation; ///< The asker's `correlation_id`
        qb::ActorId                                 created;     ///< The actor it waits for
        std::shared_ptr<detail::spawn_factory_base> factory;     ///< Says what that actor is now
    };
    std::shared_ptr<bool> _serving = std::make_shared<bool>(true); ///< Until it ends; its waits hold it too. Declared first:
                                                                   ///< nothing is in the census if this throws
    std::vector<pending>      _pending;
    std::uint64_t             _born; ///< Its entry in the census
    std::uint64_t             _next_ticket = 0;
    std::bitset<qb::MaxCores> _answered; ///< The cores it sent, after a wait or at its kill, what an actor is
    std::bitset<qb::MaxCores> _asked;    ///< The cores a kill asked, until each says it has read

    /// What a request is answered once its actor exists: what the actor is when the answer is written. The wait only
    /// says when to look: its own verdict is read at the head of a pass, ahead of the turn that ends an init, and
    /// before a kill the actor may have sent itself.
    void
    answer(pending const &p) noexcept {
        _answered.set(p.asker.index());
        auto &reply          = push<SpawnRequest>(p.asker);
        reply.correlation_id = p.correlation;
        reply.response       = spawn_result{p.created, p.factory->active() ? spawn_status::spawned : spawn_status::not_ready};
    }
    /// The answer that waits for nothing, sent back along the request. Its core is not asked, at a kill, whether
    /// it has read it: a caller that reads this `Spawner` gone first returns the same from the request itself.
    void
    answer_at_once(SpawnRequest &e, spawn_status status) noexcept {
        e.response = spawn_result{{}, status};
        reply(e);
    }
    /// Ends once no `Spawner` the request was sent to is left -- @p stamp says which -- and this core has then run
    /// one more pass: what a waiting `spawn_on` races its request against. A `Spawner` that is killed leaves the
    /// census only once this core has read its answer: the request has won before any reading says "gone". One
    /// that ends with its core has published all it ever will: an answer that left is in this core's mailbox by the
    /// reading. Both waits are `core_wait`s: the reading is made in a pass's turn of the loop, ahead of that pass's
    /// receive, and the verdict in the next pass's, behind it -- a receive reads every ring to its end. (A timer
    /// would not do: one that fires while the core parks, or under a pump of its loop, has no receive before the
    /// pass that follows.) One that ends at once on a core that runs on (a terminal signal, a direct `kill()`) may
    /// leave an answer on that core, to be read after this ends: `spawn_on` then returns what the request says was
    /// created, and the late answer resolves nothing.
    static qb::io::async::task<void>
    gone(qb::ActorId spawner, std::uint64_t stamp) {
        do
            co_await detail::core_wait{detail::spawner_watch_period};
        while (detail::spawners().holds(spawner, stamp));
        co_await detail::core_wait{qb::duration::zero()};
    }

    /// The request itself, with no deadline of the asker's: the `Spawner`'s answer, whenever it comes.
    static qb::io::async::task<spawn_result>
    request(qb::ScopedCoroContext ctx, qb::ActorId spawner, std::shared_ptr<detail::spawn_factory_base> factory, qb::duration wait) {
        auto answer = co_await qb::ask(ctx, spawner, SpawnRequest{std::move(factory), wait}, qb::duration::zero());
        co_return answer.response;
    }

    template <class A, class... Args>
    friend qb::io::async::task<spawn_result> spawn_on(qb::ScopedCoroContext ctx, qb::CoreId core, qb::duration timeout, Args... args);

public:
    /// In the census under the id `spawn_on` computes for its core -- the service's address, not this instance's.
    Spawner()
        : _born(detail::spawners().enter(qb::Actor::getServiceId<Spawner>(getIndex()))) {}
    ~Spawner() noexcept override {
        *_serving = false; // ended at once, a wait may still resume: it settles nothing with nobody
        detail::spawners().leave(_born);
    }

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SpawnRequest>(*this);
        registerEvent<detail::SpawnSettled>(*this);
        registerEvent<qb::KillEvent>(*this);
        registerEvent<qb::DownEvent>(*this);
        co_return true;
    }

    void
    on(SpawnRequest &e) {
        if (resolve_ask(e))
            return;
        auto factory = std::move(e.factory);
        // Killed, or asked of a Spawner that ended before this one was born -- its caller has been told, or will be,
        // that nothing took the request: it creates nothing, and says so.
        if (!*_serving || (factory && factory->stamp < _born)) {
            answer_at_once(e, spawn_status::unreachable);
            return;
        }
        qb::ActorId created{};
        try {
            if (factory)
                created = factory->create(*this);
        } catch (std::exception const &ex) { // a constructor that throws: refused, never a dead core
            QB_LOG_WARN("qb::Spawner: the actor's constructor threw: " << ex.what());
        } catch (...) {
            QB_LOG_WARN("qb::Spawner: the actor's constructor threw a non-std exception");
        }
        if (!created.is_valid()) {
            if (factory)
                factory->refused.store(true); // the caller knows of it, answer or none
            answer_at_once(e, spawn_status::refused);
            return;
        }
        factory->created.store(created.raw64()); // from here the caller knows of it, answer or none
        const auto ticket = ++_next_ticket;
        _pending.push_back(pending{ticket, e.getSource(), e.correlation_id, created, factory});
        // A wait may end after the kill answered its request, in the Spawner's last pass: a settlement pushed then is
        // read once the Spawner is dead -- a dead letter of the core's, for an event no user sent. The wait reads
        // whether the Spawner still serves, in a flag both hold: the Spawner itself is not its to read.
        spawn([factory = std::move(factory), ticket, wait = e.activation_wait,
               serving = _serving](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await factory->ready(ctx, wait);
            if (*serving)
                ctx.push<detail::SpawnSettled>(ticket);
        });
    }

    /// The wait for one actor ended: its asker is answered, unless a kill answered it first. Its own waits only: a
    /// settlement anybody else pushed would answer a request whose wait still runs.
    void
    on(detail::SpawnSettled const &s) {
        if (s.getSource() != id())
            return;
        const auto it = std::find_if(_pending.begin(), _pending.end(), [&s](pending const &p) { return p.ticket == s.ticket; });
        if (it == _pending.end())
            return;
        answer(*it);
        _pending.erase(it);
    }

    /// A kill. An asker waits for this `Spawner`'s answer and for nothing else, so it leaves none behind: every
    /// request still waiting for its actor is answered what the actor is now -- it exists, and lives on. Then each
    /// core it ever told so is asked whether it has read what it was sent: the death watch of that core's broadcast
    /// id, which no actor holds. The request travels behind the answers; the core answers it when it reads it, and
    /// this core answers for one that has stopped. Its own core is asked too: that answer comes back a pass later,
    /// behind what this pass sent to actors of this core, and is the kernel's -- left to nobody if this `Spawner` is
    /// ended at once meanwhile, where an event of its own would be a dead letter. A second kill finds nothing
    /// waiting, and asks again whoever has yet to answer: a watch that is open is not opened twice.
    void
    on(qb::KillEvent const &) {
        *_serving = false;
        for (pending const &p : _pending)
            answer(p);
        _pending.clear();
        _asked = _answered;
        _asked.set(getIndex());
        for (std::size_t core = 0; core < _asked.size(); ++core)
            if (_asked.test(core))
                watch(qb::BroadcastId(static_cast<std::uint32_t>(core)));
    }

    /// A core it asked has read what this `Spawner` sent it, or has stopped: the last one ends it. The answer to
    /// the watch of a broadcast id comes from that id, once for each core asked: a `DownEvent` from anybody else,
    /// or of a core it did not ask, is a death it never asked about. (One an actor that watches the same core
    /// forwards is not told apart from the kernel's: nothing in it differs.)
    void
    on(qb::DownEvent const &down) {
        const auto from = down.getSource();
        if (!from.is_broadcast() || !_asked.test(from.index()))
            return;
        _asked.reset(from.index());
        if (_asked.none())
            kill();
    }
};

/**
 * @brief Create an `A` on @p core while the engine runs, and wait until it is active.
 * @ingroup Patterns
 * @tparam A The actor type (constructible from `Args&&...`).
 * @tparam Args Constructor arguments, taken BY VALUE: they are moved to the target core.
 * @param ctx The caller's context; its actor registers `qb::SpawnRequest` (see `SpawnRequest`).
 * @param core The target core: it runs a `qb::Spawner`. The caller's own core works too.
 * @param timeout How long the `Spawner` waits for the new actor's `onInit()`, on the TARGET core's clock -- itself
 *        bounded by that core's activation deadline (`qb::VirtualCore::activation_deadline_ns`, 5 s by default; 0
 *        disables it): an `onInit()` that outlasts it is answered `not_ready`, whatever @p timeout says. Zero or less
 *        sets no deadline of the `Spawner`'s. The caller sets none either way: it waits for the `Spawner`'s answer,
 *        however late a stalled host delivers it.
 * @param args The constructor arguments.
 * @return `spawned` with the id once the actor is active; `refused` when the core admitted nothing (an
 *         `onInit()` that fails before it first suspends included); `not_ready` (the id valid) when its init
 *         failed after suspending, outlasted @p timeout, or was still running when the `Spawner` was killed -- or
 *         when the `Spawner` created it and then ended at once, its answer unsent or unread (its core stopped on
 *         an error, or its engine was stopped; a refusal lost the same way is still read `refused`);
 *         `unreachable` when no `Spawner` took the request, and nothing was created -- at once, with nothing sent,
 *         when none holds that core's service id (none was added there, or the engine runs no such core), and once
 *         the `Spawner` that was asked is seen gone and the request says nobody took it (read every
 *         `detail::spawner_watch_period`; what the `Spawner` had published is read first).
 * @throws qb::io::async::cancelled_error when the calling actor is killed meanwhile. The spawn may still
 *         complete on the target core: the new actor then lives on, managed by itself.
 * @note The request leaves at the caller's next pass, never before: from an `onInit()` every core has built its
 *       actors by then, the target's `Spawner` included -- whatever runs the thread's loop meanwhile. While it
 *       waits, the census is read every `detail::spawner_watch_period`, at the head of a pass. A caller
 *       whose actor does not register `qb::SpawnRequest` hears no answer once it is active (while it activates the
 *       core resolves the reply itself): `spawn_on` then waits for as long as the `Spawner` lives, and returns
 *       what the request says once it is gone -- `not_ready` and the id, of an actor that may well be active. Two
 *       engines alive at once in one process: see `detail::spawner_census`.
 * @code
 * #include <string>
 * #include <qb/patterns.h>
 *
 * struct Job : qb::Event {};
 * class Worker : public qb::Actor {
 *     std::string _shard;
 * public:
 *     explicit Worker(std::string shard) : _shard(std::move(shard)) {}
 *     qb::io::async::task<bool> onInit() override {
 *         registerEvent<Job>(*this);
 *         co_return true;
 *     }
 *     void on(Job const &) {}
 * };
 * class Pool : public qb::Actor {
 * public:
 *     qb::io::async::task<bool> onInit() override {
 *         registerEvent<qb::SpawnRequest>(*this); // so the answer resolves here
 *         spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
 *             const auto worker = co_await qb::spawn_on<Worker>(ctx, 1, std::chrono::seconds(1), std::string("shard-7"));
 *             if (worker)
 *                 ctx.push_to<Job>(worker.id); // active, on core 1
 *         });
 *         co_return true;
 *     }
 *     void on(qb::SpawnRequest &e) { resolve_ask(e); }
 * };
 * // before start(): engine.addActor<qb::Spawner>(1); engine.addActor<Pool>(0);
 * @endcode
 */
template <class A, class... Args>
[[nodiscard]] qb::io::async::task<spawn_result>
spawn_on(qb::ScopedCoroContext ctx, qb::CoreId core, qb::duration timeout, Args... args) {
    static_assert(std::is_base_of_v<qb::Actor, A>, "qb::spawn_on: A is a qb::Actor");
    static_assert(std::is_constructible_v<A, Args &&...>, "qb::spawn_on: A is constructible from the moved arguments");
    (qb::io::async::pin_frame_copy(args), ...); // QB-213: an over-aligned argument stays at its own alignment
    // A timeout of zero or less sets no deadline of the Spawner's (the core's activation deadline still applies, as
    // to every actor). The caller's side has none in any case: a deadline of its own here -- the timeout plus a grace
    // -- came due in the same pass as a late answer, ran first, and reported `unreachable` an actor that was alive.
    const auto wait    = timeout > qb::duration::zero() ? timeout : qb::duration::zero();
    auto       factory = std::make_shared<detail::spawn_factory<A, Args...>>(std::move(args)...);
    const auto spawner = qb::Actor::getServiceId<Spawner>(core);
    // The next pass of this core, which no pump of its loop stands in for: called from an onInit(), every core has
    // built its actors by then.
    co_await detail::core_wait{qb::duration::zero()};
    if (ctx.cancelled()) // killed meanwhile: the wait above does not say so, and nothing below might
        throw qb::io::async::cancelled_error();
    factory->stamp = detail::spawners().seen(spawner);
    if (factory->stamp == 0)
        co_return spawn_result{}; // nothing would take it: nothing is sent
    const auto race = co_await qb::io::async::when_any(Spawner::request(ctx, spawner, factory, wait), Spawner::gone(spawner, factory->stamp));
    race.rethrow_if_exception(); // the caller was killed meanwhile
    if (race.index == 0)
        co_return race.template get<spawn_result>();
    // No Spawner that was asked is left, and what it had published has been read: no answer will come. What one
    // did with the request all the same is the caller's to know: an actor created, a refusal; neither, and nothing
    // took the request.
    if (const auto created = qb::ActorId::from_raw(factory->created.load()); created.is_valid())
        co_return spawn_result{created, spawn_status::not_ready};
    co_return spawn_result{{}, factory->refused.load() ? spawn_status::refused : spawn_status::unreachable};
}

} // namespace qb

#endif // QB_CORE_PATTERNS_SPAWN_H
