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

#include <chrono>
#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <qb/core/Actor.h>
#include <qb/core/VirtualCore.h> // addRefActor / registerEvent / spawn bodies (the tail of VirtualCore.h)
#include <qb/core/patterns/request.h>
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
                ///< or an `onInit()` that failed before its first suspension (the core rolls that admission back)
    not_ready,  ///< Created, but not active within the timeout: its async init failed, threw, or still runs (the id is valid)
    unreachable ///< No answer within the timeout: no `qb::Spawner` on that core, or the core stopped
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

/// The constructor of an actor and its arguments, run once on the target core (Huly QB-54).
struct spawn_factory_base {
    virtual ~spawn_factory_base() = default;
    /// `addRefActor` on @p spawner's core; the new id, or an invalid one when the core refused.
    virtual qb::ActorId create(qb::Actor const &spawner) = 0;
    /// Whether the created actor becomes active within @p wait (false at once when it never will).
    virtual qb::io::async::task<bool> ready(qb::ScopedCoroContext ctx, qb::duration wait) const = 0;
};

template <class A, class... Args>
class spawn_factory final : public spawn_factory_base {
    std::tuple<Args...> _args;
    qb::ActorHandle<A>  _handle;
    bool                _created = false;

public:
    explicit spawn_factory(Args &&...args)
        : _args(std::move(args)...) {}

    qb::ActorId
    create(qb::Actor const &spawner) override {
        if (std::exchange(_created, true))
            return {}; // one construction per request: the arguments were moved out
        _handle = std::apply([&spawner](Args &...args) { return spawner.template addRefActor<A>(std::move(args)...); }, _args);
        return _handle.id();
    }

    qb::io::async::task<bool>
    ready(qb::ScopedCoroContext ctx, qb::duration wait) const override {
        return _handle.ready_async(std::move(ctx), wait);
    }
};

/// What the asker waits beyond the target's own activation deadline before it calls the core unreachable.
inline constexpr qb::duration spawn_reply_grace = std::chrono::milliseconds(250);

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

/**
 * @class Spawner
 * @ingroup Patterns
 * @brief The per-core service `spawn_on` asks: add one to every core that accepts actors created at
 *        run time -- `engine.addActor<qb::Spawner>(core)` before `start()`.
 * @details On a `SpawnRequest` it runs `addRefActor` on its own core (the core's admission: a refused
 *          reservation, or an `onInit()` failing before it suspends, answers `refused` at once), then waits, in a coroutine of its own, for the
 * new actor to become active -- event-driven, `ActorHandle::ready_async` -- and answers `spawned`, or `not_ready` with the id. The new actor
 * manages its own lifecycle: the spawner does not own it, and a spawner killed meanwhile kills nothing but its wait.
 */
class Spawner : public qb::ServiceActor<Spawner> {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SpawnRequest>(*this);
        co_return true;
    }

    void
    on(SpawnRequest &e) {
        if (resolve_ask(e))
            return;
        auto              factory = std::move(e.factory);
        const qb::ActorId created = factory ? factory->create(*this) : qb::ActorId{};
        if (!created.is_valid()) {
            e.response = spawn_result{{}, spawn_status::refused};
            reply(e);
            return;
        }
        spawn([factory = std::move(factory), created, asker = e.getSource(), correlation = e.correlation_id,
               wait = e.activation_wait](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const bool active     = co_await factory->ready(ctx, wait);
            auto      &answer     = ctx.push_to<SpawnRequest>(asker);
            answer.correlation_id = correlation;
            answer.response       = spawn_result{created, active ? spawn_status::spawned : spawn_status::not_ready};
        });
    }
};

/**
 * @brief Create an `A` on @p core while the engine runs, and wait until it is active.
 * @ingroup Patterns
 * @tparam A The actor type (constructible from `Args&&...`).
 * @tparam Args Constructor arguments, taken BY VALUE: they are moved to the target core.
 * @param ctx The caller's context; its actor registers `qb::SpawnRequest` (see `SpawnRequest`).
 * @param core The target core: it runs a `qb::Spawner`. The caller's own core works too.
 * @param timeout The bound on the whole spawn, the new actor's `onInit()` included.
 * @param args The constructor arguments.
 * @return `spawned` with the id once the actor is active; `refused` when the core admitted nothing (an
 *         `onInit()` that fails before it first suspends included); `not_ready` (the id valid) when its init
 *         failed after suspending or outlasted @p timeout; `unreachable` when no
 *         answer came within @p timeout plus a grace -- no `Spawner` there, or the core stopped.
 * @throws qb::io::async::cancelled_error when the calling actor is killed meanwhile. The spawn may still
 *         complete on the target core: the new actor then lives on, managed by itself, like one whose
 *         answer came after `unreachable`.
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
    const auto wait    = timeout > qb::duration::zero() ? timeout : qb::duration::zero();
    auto       factory = std::make_shared<detail::spawn_factory<A, Args...>>(std::move(args)...);
    try {
        auto answer = co_await qb::ask(ctx, qb::Actor::getServiceId<Spawner>(core), SpawnRequest{std::move(factory), wait},
                                       wait + detail::spawn_reply_grace);
        co_return answer.response;
    } catch (qb::io::async::timeout_error const &) {
        co_return spawn_result{};
    }
}

} // namespace qb

#endif // QB_CORE_PATTERNS_SPAWN_H
