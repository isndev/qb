# qb-core features and capabilities

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

This page catalogs the capabilities of `qb-core` — the actor runtime built on `qb-io` — and links each one to the page that documents it in depth.

**Prerequisites:** [Core concepts: the actor model](../2_core_concepts/actor_model.md), [qb-io module overview](../3_qb_io/README.md) — **See also:** [qb-core overview](./README.md)

---

## Summary

`qb-core` provides the actor model on top of the `qb-io` asynchronous event loop: a base [`qb::Actor`](./actor.md) class, a typed event system, a multi-threaded engine ([`qb::Main`](./engine.md) plus one [`qb::VirtualCore`](./engine.md) worker thread per core), lock-free inter-core messaging, and C++23 coroutine integration. Each capability below is a one-line entry pointing at its owning page; the owning page holds the authoritative signatures and contracts.

The public surface is reachable through three umbrella headers: `<qb/actor.h>` (actor and pipe), `<qb/main.h>` (engine), and `<qb/event.h>` (event base and system events).

---

## Actor lifecycle and management

Owned by [Mastering `qb::Actor`](./actor.md).

| Capability | API | One-line summary |
|---|---|---|
| Actor base class | `qb::Actor` | Base class for every user actor; encapsulates state behind sequential, single-threaded event handling. |
| System-unique identity | `qb::ActorId` | Compound `CoreId` + `ServiceId` identifier used to address every event; obtained via `id()`. |
| Initialization checkpoint | `virtual bool onInit()` | Runs after construction and ID assignment; register events and acquire resources here. Returning `false` aborts startup and destroys the actor. |
| Graceful termination | `kill()` | Marks the actor for removal; the owning `VirtualCore` finishes the in-flight handler before destruction. |
| Liveness query | `is_alive()` | Returns `true` until `kill()` takes effect. |
| RAII destruction | `virtual ~Actor()` | Runs only after the actor is fully removed from its core; member RAII cleanup is safe here. |
| Lightweight actors | `qb::no_default_events` | Constructor tag that skips the four default system-event subscriptions (`KillEvent`, `SignalEvent`, `PingEvent`, `UnregisterCallbackEvent`). |
| Identity and timing accessors | `id()`, `getIndex()`, `getName()`, `getCoreSet()`, `time()` | Read-only accessors; `time()` returns a `uint64_t` nanosecond count cached once per loop iteration. |

Actor creation entry points:

| API | Where it lives | Summary |
|---|---|---|
| `qb::Main::addActor<A>(core_id, args...)` | [Engine](./engine.md) | Add one actor to a specific core before `start()`; returns its `qb::ActorId`. |
| `main.core(id).builder().addActor<A>(args...)` | [Engine](./engine.md) | Fluent `ActorBuilder` for adding several actors to one core and collecting their IDs. |
| `Actor::addRefActor<A>(args...)` | [Actor](./actor.md) | Create a child actor on the **same core**; returns a raw, non-owning `A*` (or `nullptr` if `onInit()` failed). |
| `Actor::addRefHandle<A>(args...)` | [Actor](./actor.md) | Create a same-core child wrapped in a liveness-checked `qb::RefActorHandle<A>`. |

---

## Event system and asynchronous messaging

Owned by [Event messaging between actors](./messaging.md).

| Capability | API | One-line summary |
|---|---|---|
| Event base class | `qb::Event` | Cache-line-aligned base for every inter-actor message; carries header state, a type id, and dest/source IDs. |
| Per-type identifiers | `qb::type_id<T>()` | Assigns a dense, collision-free `TypeId` from a monotonic atomic counter the first time it is queried for a type. |
| Event subscription | `registerEvent<E>(*this)` / `unregisterEvent<E>(*this)` | Subscribe (typically in `onInit()`) or unsubscribe an actor from an event type at runtime. |
| Type-safe dispatch | `void on(E&)` / `void on(E const&)` | The handler invoked for each registered event type; a non-const reference is required to use `reply()` or `forward()`. |

Quality-of-service levels:

| Type | Priority | Notes |
|---|---|---|
| `qb::Event` (alias `qb::EventQOS2`) | High | Default base type; processed before lower QoS levels. |
| `qb::EventQOS1` | Medium | Alias of `qb::Event`; processed after QOS2 and before QOS0. |
| `qb::EventQOS0` | Low | Distinct subclass that sets `state.qos = 0`; processed last. |

Sending methods:

| Method | Ordering | Constraint | Use case |
|---|---|---|---|
| `push<E>(dest, args...)` | Ordered per source/dest pair | None; supports non-trivially-destructible events | Default, recommended. |
| `send<E>(dest, args...)` | Unordered | Event type **must be trivially destructible** | Low-latency fire-and-forget. |
| `broadcast<E>(args...)` | N/A | None | Deliver to every actor on every core. |
| `push<E>(qb::BroadcastId(core), args...)` | Ordered | None | Deliver to every actor on one core. |
| `reply(event)` | N/A | Non-const `Event&` | Return a received event to its source by swapping dest/source. |
| `forward(dest, event)` | N/A | Non-const `Event&` | Re-route a received event to a new destination, preserving its source. |

Lower-level and batched sending:

| API | Summary |
|---|---|
| `to(dest)` returns `qb::Actor::EventBuilder` | Fluent helper that chains ordered `push<E>()` calls over one destination pipe, avoiding repeated pipe lookups. |
| `getPipe(dest)` returns `qb::Pipe` | Direct access to the typed channel to a destination for performance-critical paths. |
| `pipe.allocated_push<E>(size_hint, args...)` | Pre-size the pipe buffer for a large-payload event to avoid internal reallocation. |

---

## Concurrency, parallelism, and scheduling

Owned by [Engine — `qb::Main` and `VirtualCore`](./engine.md). See also [Core concepts: the threading model](../2_core_concepts/threading_model.md).

| Capability | API | One-line summary |
|---|---|---|
| Engine controller | `qb::Main` (alias `qb::engine`) | Owns the `CoreInitializer`s, spawns one worker thread per `VirtualCore`, and drives start/stop/join and signal handling. |
| Worker thread | `qb::VirtualCore` | Per-thread worker that owns its actors and runs an independent event loop backed by a `qb::io::async::listener`. |
| Static actor affinity | actor placement at `addActor` time | Actors are assigned to a core at creation and never migrate between cores. |
| Lock-free inter-core messaging | `qb::lockfree::mpsc::ringbuffer` mailboxes | Cross-core events travel through MPSC ring buffers with no mutex on the hot path. |
| Idle latency policy | `CoreInitializer::setLatency(qb::duration)` / `Main::setLatency(qb::duration)` | `qb::duration::zero()` (the default) busy-spins for lowest latency; a positive value sleeps when idle to trade latency for CPU. |
| CPU affinity | `CoreInitializer::setAffinity(qb::CoreIdSet)` | Pin a worker thread to specific physical CPUs; `qb::NoAffinity` lets the OS scheduler choose. |
| Lifecycle control | `Main::start(bool async = true)`, `Main::stop()`, `Main::join()`, `Main::hasError()` | Start the engine (optionally blocking), request a clean stop, wait for workers, and check for startup/runtime errors. |
| Stop-token cancellation | `std::stop_source` / `std::stop_token` | The engine uses C++20 stop tokens for signal-free shutdown across all platforms. |
| Signal handling | `Main::registerSignal`, `Main::unregisterSignal`, `Main::ignoreSignal` | Route OS signals into engine shutdown. |

---

## C++23 coroutine integration

Owned by [Common actor patterns and utilities](./patterns.md#6-c23-coroutine-pattern--spawn_async). The coroutine runtime itself is documented under [qb-io: C++23 coroutines](../3_qb_io/coroutines.md).

| Capability | API | One-line summary |
|---|---|---|
| Launch a coroutine from a handler | `Actor::spawn_async(func)` | Starts a `qb::io::async::task<void>` that runs concurrently with the actor's event processing; the actor keeps handling events while the coroutine is suspended at a `co_await`. |
| Lifetime-safe context | `qb::CoroContext` | Captures the actor's `ActorId` by value at spawn time; its `push<E>(...)` and `push_to<E>(dest, ...)` remain valid even if the parent actor is destroyed during a `co_await`. |
| Coroutine introspection | `has_active_coroutines()`, `active_coroutine_count()` | Inspect pending asynchronous work before destroying an actor. |

The safety contract — never touch actor members after a `co_await`, copy all needed state by value before the first suspension, and use only `CoroContext` afterward — is detailed and illustrated under [qb-io: safe integration with `qb::Actor`](../3_qb_io/coroutines.md#safe-integration-with-qbactor).

---

## Actor patterns and utilities

Owned by [Common actor patterns and utilities](./patterns.md).

| Capability | API | One-line summary |
|---|---|---|
| Periodic callbacks | `qb::ICallback` + `registerCallback(*this)` | Mixin granting an `onCallback()` tick once per `VirtualCore` loop iteration, after the core has received and dispatched that iteration's events; the body must be fast and non-blocking. |
| Service actors | `qb::ServiceActor<Tag>` | One instance per `VirtualCore`, identified by a unique `Tag`; reached on the same core via `getService<MyService>()`. |
| Dependency discovery | `require<TargetActor>()` | Broadcasts a `PingEvent`; live actors of the target type reply with `qb::RequireEvent`, handled with `is<TargetActor>(event)`. |
| Safe child references | `qb::RefActorHandle<T>` | Wraps the raw pointer from `addRefActor<T>()` with an O(1) liveness check on every dereference, preventing dangling-pointer use after the child terminates. |

---

## Pitfalls

- **`send()` is restricted to trivially destructible events.** Use `push()` for events holding `std::string`, `std::vector`, or any type with a non-trivial destructor. The trivial-destructibility constraint applies to `send()`, not to `EventQOS0` — QoS only sets dispatch priority.
- **`onInit()` is the only safe place to call `registerEvent<E>()`.** Returning `false` from it aborts startup and destroys the actor before it processes any message.
- **`no_default_events` actors do not respond to `KillEvent` or signals** unless you subscribe to those events explicitly in `onInit()`.
- **`reply()` and `forward()` consume the event.** After either call the received event object must not be used again in the handler; both require a non-const `on(E&)` handler.
- **`onCallback()` and every handler run on the `VirtualCore` thread.** Blocking or long-running work stalls every actor on that core; offload to a coroutine via `spawn_async()` instead.

---

## See also

- [Mastering `qb::Actor`](./actor.md) — defining, initializing, and managing actors.
- [Event messaging between actors](./messaging.md) — events, QoS, and every sending method in depth.
- [Engine — `qb::Main` and `VirtualCore`](./engine.md) — startup, shutdown, affinity, and inter-core communication.
- [Common actor patterns and utilities](./patterns.md) — service actors, periodic callbacks, discovery, and referenced actors.
- [Core concepts: the threading model](../2_core_concepts/threading_model.md) — how cores, threads, and actor affinity fit together.
