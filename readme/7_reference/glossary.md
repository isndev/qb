# Glossary

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

A definition for every domain term used across the qb documentation, each grounded in the header that owns it and linking to the page that explains it in full.

**Prerequisites:** none — **See also:** [Actor model](../2_core_concepts/actor_model.md), [Event system](../2_core_concepts/event_system.md), [Threading model](../2_core_concepts/threading_model.md), [Core invariants](./core_invariants.md), [qb-io invariants](./io_invariants.md), [API overview](./api_overview.md)

## How to read this page

Terms are grouped by subsystem and alphabetized within each group. A term in `monospace` is an identifier that exists in the source; the qualified name (for example `qb::Actor`) is the one you write in code. Each definition states what the term *is*, not how to use it — the linked page owns the usage. This page never redefines an API; it points to the owner.

The framework splits into two layers. `qb-io` is a standalone asynchronous I/O library — sockets, transports, protocols, an event loop, and coroutines — usable without the actor runtime. `qb-core` is the actor runtime layered on top of it: the engine, worker threads, actors, and events.

---

## Actor runtime (qb-core)

**Actor (`qb::Actor`)** — The unit of computation and state. An actor is an isolated C++ object with a unique [`ActorId`](#actor-runtime-qb-core) that owns its state and communicates with other actors only by passing [events](#event-qbevent). Each actor is processed on a single [`VirtualCore`](#virtualcore-qbvirtualcore) worker thread and never migrates between threads, so its members need no locks. Actors must be constructed from within a `VirtualCore` worker thread — via `Main::core(idx).addActor<T>(...)` or `addRefActor<T>()` — never from the main thread (the constructor asserts this). Defined in `include/qb/core/Actor.h`. See [Actor](../4_qb_core/actor.md).

**Actor model** — The model of concurrent computation in which independent "actors" communicate only through asynchronous messages, each processing its own messages sequentially. State is never shared; there is no shared-memory contention to guard. qb implements this model directly. See [Actor model](../2_core_concepts/actor_model.md).

**`ActorId` (`qb::ActorId`)** — The system-wide identity of an actor. It is a compound 32-bit value built from a [`ServiceId`](#serviceid-qbserviceid) (the actor's slot within a core, `sid()`) and a [`CoreId`](#coreid-qbcoreid) (the hosting `VirtualCore`, `index()`); it converts to and from `uint32_t`. A default-constructed `ActorId()` equals `ActorId::NotFound` (`== 0`) and is invalid; `is_valid()` reports whether the value differs from `NotFound`. `is_broadcast()` is true when the service id is `BroadcastSid` (`ServiceId` max). Defined in `include/qb/core/ActorId.h`.

**`addRefActor<T>()` / `addRefHandle<T>()`** — Create a [referenced actor](#referenced-actor) on the caller's own core. `addRefActor<T>(args...)` returns a raw `T*` (or `nullptr` if `onInit()` returned false); the caller does not own the child and the pointer dangles once the child calls `kill()`. `addRefHandle<T>(args...)` wraps the same child in a [`RefActorHandle<T>`](#refactorhandlet-qbrefactorhandle) that performs an O(1) liveness check on dereference. Both are declared in `include/qb/core/Actor.h`. See [Actor](../4_qb_core/actor.md).

**Affinity (CPU affinity)** — The configured mapping of a [`VirtualCore`](#virtualcore-qbvirtualcore) thread onto a set of physical CPUs, set with `CoreInitializer::setAffinity` before the engine starts. Affinity is best-effort: a failed `pthread_setaffinity_np` (or `SetThreadAffinityMask` on Windows) only warns and never fails core initialization, and core ids at or above `qb::MaxCores` — including the `qb::NoAffinity` sentinel (`CoreId` max) — are filtered out before pinning. See [Engine](../4_qb_core/engine.md).

**`BroadcastId` (`qb::BroadcastId`)** — A specialized [`ActorId`](#actorid-qbactorid) whose service id is `BroadcastSid`, used as the destination of `push<Event>(qb::BroadcastId(core_id), ...)` to deliver one event to every actor on a single core. Constructed only from a `core_id`; the default constructor is deleted. Defined in `include/qb/core/ActorId.h`. Contrast [broadcast](#broadcast-actorbroadcastt).

**broadcast (`actor.broadcast<T>()`)** — Fan an event out to every actor on every core. `broadcast<Event>(args...)` is `noexcept` and sets this actor as the source; to target a single core, use `push<Event>(qb::BroadcastId(core_id), ...)` instead. A broadcast event cannot be replied to or forwarded (the framework logs and drops the attempt). Declared in `include/qb/core/Actor.h`. See [Messaging](../4_qb_core/messaging.md).

**`build_event<T>()`** — Construct an event object locally without sending it. `build_event<Event>(source, args...)` returns a `_Event` whose `dest` is set to this actor's id, for passing directly to an `on()` handler or a referenced actor's method; it does not enter the messaging queues. Declared in `include/qb/core/Actor.h`.

**Callback, actor (`qb::ICallback`)** — A mixin an actor implements to run periodic work. After `registerCallback(*this)` in `onInit()`, the framework calls `onCallback()` once per [`VirtualCore`](#virtualcore-qbvirtualcore) loop iteration — after the outbound pipes flush and the inbound mailbox drains; events pushed from `onCallback()` flush at the start of the next iteration — until `unregisterCallback()`. `onCallback()` runs on the core's event-loop thread and must not block; blocking it stalls every actor on that core. Defined in `include/qb/core/ICallback.h`. See [Actor](../4_qb_core/actor.md).

**`CoreId` (`qb::CoreId`)** — A `uint16_t` identifying a single [`VirtualCore`](#virtualcore-qbvirtualcore). It is the high component of an [`ActorId`](#actorid-qbactorid), readable via `ActorId::index()`. Defined in `include/qb/core/ActorId.h`.

**`CoreIdSet` (`qb::CoreIdSet`)** — A bitset-backed set of `CoreId` values (alias for `CoreIdBitSet`, capacity `qb::MaxCores == 256`), used for affinity masks. Out-of-range ids — notably `qb::NoAffinity` — are silently skipped on construction, so `CoreIdSet{NoAffinity}` means "no pinning" rather than an error. Defined in `include/qb/core/ActorId.h`.

**`CoreInitializer` (`qb::CoreInitializer`)** — A per-core configuration handle obtained from `qb::Main::core(id)` before the engine starts. It seeds actors (`addActor`, `builder`) and sets per-core properties (`setAffinity`, `setLatency`). Listed in the qb-core public API; see [Engine](../4_qb_core/engine.md).

**`CoreSet` (`qb::CoreSet`)** — The engine's immutable mapping from a logical `CoreId` to a dense, zero-based mailbox index, giving O(1) inter-core routing. It is internal plumbing; actor code does not construct it. See [Threading model](../2_core_concepts/threading_model.md).

**`CoroContext` (`qb::CoroContext`)** — The safe handle passed to a coroutine spawned by [`spawn_async()`](#spawn_async). It carries the actor's `ActorId` *by value*, so the coroutine can `ctx.push<Event>(...)` events back to the actor and read `ctx.id()` / `ctx.time()` without touching the actor object — which may be destroyed while the coroutine is suspended. Declared in `include/qb/core/Actor.h`. See [Async in actors](../5_core_io_integration/async_in_actors.md).

**EventBuilder (`Actor::to(dest)`)** — A fluent helper returned by `to(ActorId dest)` that chains multiple ordered `push<Event>(...)` calls over one destination [pipe](#pipe-communication-channel-qbpipe), preserving send order. Declared in `include/qb/core/Actor.h`. See [Messaging](../4_qb_core/messaging.md).

**forward (`actor.forward(dest, event)`)** — Re-route a received event to a new destination, reusing the event object. `forward(ActorId dest, Event& event)` updates `dest` and **preserves the original source**, then sends; the handler must take the event by non-const reference, and the event is consumed afterward. Broadcast events cannot be forwarded. Declared in `include/qb/core/Actor.h`. Contrast [reply](#reply-actorreplyevent). See [Messaging](../4_qb_core/messaging.md).

**`getPipe(dest)`** — Return the [`Pipe`](#pipe-communication-channel-qbpipe) to a destination actor for lower-level sends — repeated pushes to one destination, or `Pipe::allocated_push()` for large events that benefit from a size hint. `getPipe(ActorId dest)` is declared in `include/qb/core/Actor.h`. See [Messaging](../4_qb_core/messaging.md).

**`ICallback`** — See [Callback, actor](#callback-actor-qbicallback).

**`kill()`** — Flag an actor for termination. It sets `_alive = false` and asks the `VirtualCore` to retire the actor; the actor stops receiving new events but may still drain already-queued events, and `~Actor()` runs later under core control. Declared in `include/qb/core/Actor.h`. See [Core invariants](./core_invariants.md).

**Mailbox** — The per-core queue into which *other* cores enqueue cross-core events: a lock-free [MPSC](#mpsc-queue-multiple-producer-single-consumer) ring buffer, optionally backed by a `condition_variable` for parking when [latency](#latency-setlatency) is greater than zero. Conceptually, each actor's incoming events queue here and are processed sequentially by its owning core. See [Threading model](../2_core_concepts/threading_model.md).

**`Main` / engine (`qb::Main`, alias `qb::engine`)** — The top-level controller. It owns the [`CoreInitializer`](#coreinitializer-qbcoreinitializer)s, spawns one `std::jthread` per [`VirtualCore`](#virtualcore-qbvirtualcore), wires the inter-core mailboxes, and drives `start` / `stop` / `join` and signal handling. With `start(false)` the calling thread becomes the last worker and `start()` blocks until shutdown; with `start(true)` (the default) it returns once all cores report ready and `join()` is called later. Listed in the qb-core public API; see [Engine](../4_qb_core/engine.md).

**Latency (`setLatency`)** — The idle wait budget of a [`VirtualCore`](#virtualcore-qbvirtualcore), expressed as a [`qb::duration`](#qbduration). `setLatency(qb::duration::zero())` — the default — puts the core in busy-spin low-latency mode (100% CPU on its core); a positive value lets the core park on a `condition_variable` for up to that span when idle, trading worst-case latency for lower CPU. See [Engine](../4_qb_core/engine.md).

**`onCallback()`** — See [Callback, actor](#callback-actor-qbicallback).

**Pipe (communication channel — `qb::Pipe`)** — A unidirectional, ordered channel from a source actor to a destination actor, obtained via [`getPipe(dest)`](#getpipedest). It wraps an internal `VirtualPipe` (the lock-free ring-buffer segment owned by the source core) and exposes `push<Event>()` and `allocated_push<Event>()`. Defined in `include/qb/core/Pipe.h`. Distinct from the [memory `pipe`](#pipe-memory-buffer-qballocatorpipet). See [Messaging](../4_qb_core/messaging.md).

**push (`actor.push<T>()`)** — The primary way to send an event. `push<Event>(dest, args...)` is `noexcept`, guarantees **ordered** delivery to the same destination from the same source, supports event types with non-trivial members, and returns a mutable reference to the constructed event so you can fill fields before it is dispatched. Because it is `noexcept`, an allocation failure cannot be reported and calls `std::terminate()` — keep events small. Declared in `include/qb/core/Actor.h`. Contrast [send](#send-actorsendt). See [Messaging](../4_qb_core/messaging.md).

**`RefActorHandle<T>` (`qb::RefActorHandle`)** — A liveness-checked wrapper around a [referenced actor](#referenced-actor) returned by [`addRefHandle<T>()`](#addrefactort-addrefhandlet). Dereference performs an O(1) check, avoiding the dangling-pointer hazard of a raw `addRefActor` result. It may only be dereferenced from the owning `VirtualCore`'s worker thread; a cross-thread dereference is a logic error, asserted in debug builds. Declared in `include/qb/core/Actor.h`.

**Referenced actor** — A child actor created on the *same* [`VirtualCore`](#virtualcore-qbvirtualcore) as its parent via `addRefActor<T>()`. The parent receives a raw, non-owning pointer and may call the child's methods directly, bypassing the event queue. The child owns its own lifecycle and must call `kill()` to terminate; the parent's pointer dangles afterward. See [Patterns](../4_qb_core/patterns.md).

**reply (`actor.reply(event)`)** — Return a received event to its sender, reusing the event object. `reply(Event& event)` swaps the event's `dest` and `source` and sends it back; the handler must take the event by non-const reference, and the event is consumed afterward. Broadcast events cannot be replied to. Declared in `include/qb/core/Actor.h`. Contrast [forward](#forward-actorforwarddest-event). See [Messaging](../4_qb_core/messaging.md).

**`require<T>()` / discovery** — A runtime actor-discovery mechanism. `require<ActorTypes...>()` broadcasts a `qb::PingEvent` for each type; live actors of those types reply with a `qb::RequireEvent` carrying their [`ActorStatus`](#actorstatus). The requesting actor must register for `RequireEvent` and use `is<T>(event)` to match responses. Declared in `include/qb/core/Actor.h`. See [Patterns](../4_qb_core/patterns.md).

**`send` (`actor.send<T>()`)** — Send an event in **unordered** fashion. `send<Event>(dest, args...)` is `noexcept` and may offer slightly lower same-core latency than [`push`](#push-actorpusht), but provides no ordering guarantee and requires the event type to be **trivially destructible** (POD members or [`qb::string`](#qbstringn); `std::string` and `std::vector` are not permitted). Prefer `push` unless order genuinely does not matter. Declared in `include/qb/core/Actor.h`. See [Messaging](../4_qb_core/messaging.md).

**`ServiceActor<Tag>` (`qb::ServiceActor`)** — A singleton-style actor: one instance per [`VirtualCore`](#virtualcore-qbvirtualcore) per `Tag` struct, reachable from sibling actors on the same core via `getService<T>()`. Its `ServiceIndex` is registered once under a magic-static guard, so the service id is unique and valid even under concurrent first use. Declared in `include/qb/core/Actor.h`. See [Patterns](../4_qb_core/patterns.md).

**`ServiceId` (`qb::ServiceId`)** — A `uint16_t` identifying an actor *within* its core — the low component of an [`ActorId`](#actorid-qbactorid), readable via `ActorId::sid()`. The reserved value `BroadcastSid` (`ServiceId` max) marks a [`BroadcastId`](#broadcastid-qbbroadcastid). Defined in `include/qb/core/ActorId.h`.

**`spawn_async()`** — The only supported way to run a C++20 coroutine inside an actor. It launches the coroutine in an isolated context and must be called from the actor's `VirtualCore` worker thread. The coroutine must capture everything it needs by value before the first `co_await` and communicate back only through its [`CoroContext`](#corocontext-qbcorocontext) — accessing actor members after a `co_await` is undefined behavior, because the actor may be destroyed while the coroutine is suspended. Declared in `include/qb/core/Actor.h`. See [Async in actors](../5_core_io_integration/async_in_actors.md).

**`ActorStatus`** — An enum carried by a [`RequireEvent`](#requiret-discovery) reporting whether a discovered actor is `Alive` (or otherwise). Defined alongside the discovery events in `include/qb/core/Event.h`.

**`Actor::time()`** — Return the `VirtualCore`'s cached timestamp as whole nanoseconds (`uint64_t`). It is constant within a single event-handler or `onCallback()` invocation; for a continuously updating high-precision value use `qb::unix_nanos(qb::wall_now())`. Declared in `include/qb/core/Actor.h`.

**`VirtualCore` (`qb::VirtualCore`)** — A worker thread (`std::jthread`) owned by [`Main`](#main-engine-qbmain-alias-qbengine). It exclusively owns a set of actors, runs the event loop dispatching their events and callbacks, flushes inter-core messages, and runs its own [`qb-io` event loop](#listener-qbioasynclistener). A `VirtualCore` is strictly single-threaded: its actor maps and `ServiceIdPool` use no synchronization, so actors must communicate only through events. Listed in the qb-core public API; see [Threading model](../2_core_concepts/threading_model.md).

---

## Events

**Event (`qb::Event`)** — The base message type for all inter-actor communication. It is cache-line-aligned and carries header state (alive flag, QoS), a type id, and `dest`/`source` [`ActorId`](#actorid-qbactorid)s; every message is a subclass of `qb::Event`. Defined in `include/qb/core/Event.h`. See [Event system](../2_core_concepts/event_system.md).

**Event sourcing** — A persistence pattern in which state changes are stored as an ordered log of events and the current state is rebuilt by replaying them. It is an application pattern, not a framework primitive. See [Patterns](../4_qb_core/patterns.md).

**`KillEvent` (`qb::KillEvent`)** — The built-in event that requests an actor terminate; its default `on()` handler calls [`kill()`](#kill). A remote sender retires an actor only by enqueuing a `KillEvent` into the core's [mailbox](#mailbox) — never by touching the actor's state directly. Defined in `include/qb/core/Event.h`.

**`RequireEvent` (`qb::RequireEvent`)** — The reply a live actor sends in response to a discovery [`require<T>()`](#requiret-discovery) ping; it carries the responder's type and [`ActorStatus`](#actorstatus). Defined in `include/qb/core/Event.h`.

**`SignalEvent` (`qb::SignalEvent`)** — The event delivered to actors when an OS signal registered via `Main::registerSignal` fires. Defined in `include/qb/core/Event.h`.

---

## Asynchronous I/O (qb-io)

**Asynchronous I/O (`qb-io`)** — I/O operations (network, file) that do not block the calling thread while waiting for completion. `qb-io` is a standalone library; `qb-core` builds its concurrency model on top of it. See [qb-io overview](../3_qb_io/README.md).

**Async system (`qb::io::async`)** — The `qb-io` subsystem that owns the per-thread event loop, timers, asynchronous callbacks, and coroutine scheduling. Public entry points include `init`, `run`, `run_once`, `run_until`, `callback`, and `sleep(qb::duration)`. See [Async system](../3_qb_io/async_system.md).

**`AProtocol<IO_Type>` (`qb::io::async::AProtocol`)** — The CRTP base for a custom [protocol](#protocol-qbioasyncaprotocolio_type). It refines the `IProtocol` interface — the three pure virtuals `getMessageSize()`, `onMessage(size)`, and `reset()` — for a concrete I/O component. Defined in `include/qb/io/async/protocol.h`. See [Protocols](../3_qb_io/protocols.md).

**Awaiter** — The object a `co_await` expression operates on. It exposes `await_ready()`, `await_suspend(handle)`, and `await_resume()`; qb-io's libev-backed awaiters arm a watcher in `await_suspend` and stop it in `await_resume` (and in the destructor) to guard against a watcher firing after resumption. Defined in `include/qb/io/async/coroutine/awaiter.h`. See [Coroutines](../3_qb_io/coroutines.md).

**`callback` (I/O — `qb::io::async::callback`)** — A `qb-io` utility that schedules a callable on the current thread's event loop, optionally after a [`qb::duration`](#qbduration) delay. Distinct from an [actor callback](#callback-actor-qbicallback). See [Async system](../3_qb_io/async_system.md).

**`coro_scheduler` / CoroutineScheduler** — The thread-local cooperative scheduler that holds the ready queue and frame bookkeeping and resumes coroutines on the `VirtualCore` (or listener) thread driving the libev loop. `coro_scheduler().spawn(std::move(task))` enqueues a top-level coroutine. Defined in `include/qb/io/async/coroutine/scheduler.h`. See [Coroutines](../3_qb_io/coroutines.md).

**CRTP (Curiously Recurring Template Pattern)** — A C++ idiom where a class derives from a template parameterized on the derived class itself (for example `qb::io::async::io<MyClass>`). `qb-io` uses it for static polymorphism — resolving calls at compile time without vtable overhead. See [Async system](../3_qb_io/async_system.md).

**Endpoint (`qb::io::endpoint`)** — A `qb-io` value representing a network address: an IPv4 or IPv6 address with a port, or a Unix-domain socket path. Defined in `include/qb/io/system/sys__socket.h`. See [Transports](../3_qb_io/transports.md).

**Event loop** — See [listener](#listener-qbioasynclistener).

**Framing (message framing)** — Recognizing message boundaries within a continuous byte stream (for example a TCP connection). qb-io delegates framing to a [protocol](#protocol-qbioasyncaprotocolio_type). See [Protocols](../3_qb_io/protocols.md).

**io_handler / session pool (`qb::io::async::io_handler`)** — A CRTP mixin that owns the map of active client [sessions](#session-networking) on a server component (`registerSession`, `extractSession`, `disconnected`); an override of `disconnected` must forward to the base to avoid leaks. Typically reached via `qb::io::use<...>::tcp::server`. See [Network actors](../5_core_io_integration/network_actors.md).

**libev** — The C event-loop library qb-io uses internally to interact with the OS notification mechanism (epoll, kqueue, and so on). The single seam between [`qb::duration`](#qbduration) nanoseconds and libev's `ev_tstamp` double-seconds is `qb::detail::to_ev_seconds` / `qb::detail::from_ev_seconds`. See [Async system](../3_qb_io/async_system.md).

**Listener (`qb::io::async::listener`)** — The thread-local event-loop manager (`qb::io::async::listener::current`) that registers watchers, runs the loop, and dispatches kernel events to handlers within one thread — and therefore within one [`VirtualCore`](#virtualcore-qbvirtualcore). It is built on [libev](#libev). Defined in `include/qb/io/async/listener.h`. See [Async system](../3_qb_io/async_system.md).

**Listener (networking — `qb::io::tcp::listener`, `qb::io::tcp::ssl::listener`)** — A `qb-io` class that binds a local address and port and accepts incoming TCP or TLS connections. Distinct from the [event-loop listener](#listener-qbioasynclistener). Defined under `include/qb/io/tcp/`. See [Transports](../3_qb_io/transports.md).

**Protocol (`qb::io::async::AProtocol<IO_Type>`)** — A class defining how a raw byte stream is interpreted as a sequence of discrete application messages: it owns [framing](#framing-message-framing) and the initial parse. Concrete protocols implement `getMessageSize()`, `onMessage(size)`, and `reset()`. Defined in `include/qb/io/async/protocol.h`. See [Protocols](../3_qb_io/protocols.md).

**Session (networking)** — A single, stateful client connection managed by a server-side component (for example `ChatSession` in the chat example). Sessions are tracked by an [`io_handler`](#io_handler-session-pool-qbioasyncio_handler). See [Network actors](../5_core_io_integration/network_actors.md).

**Stream (`qb::io::istream`, `qb::io::ostream`, `qb::io::stream`)** — The base templates in `qb-io` that layer buffered I/O over an underlying [transport](#transport-qbiotransport). See [Transports](../3_qb_io/transports.md).

**`task<T>` (`qb::io::async::task`)** — The coroutine return type for qb-io coroutines (`template <typename T = void>`, so the bare default is `task<void>`). It enables `co_await` and `co_return` and integrates the coroutine frame with the libev-driven [scheduler](#coro_scheduler-coroutinescheduler); `co_yield` is not a `task` facility — it belongs to `qb::io::async::generator` / `async_generator`. A coroutine's local objects live until `co_return`; capture loop variables by value, not by reference, to avoid dangling. Defined in `include/qb/io/async/coroutine/task.h`. See [Coroutines](../3_qb_io/coroutines.md).

**Transport (`qb::io::transport::*`)** — A `qb-io` component implementing communication over one medium — `transport::tcp`, `transport::udp`, `transport::file`, `transport::stcp` (SSL/TLS) — typically by specializing a [stream](#stream-qbioistream-qbioostream-qbiostream). Defined under `include/qb/io/transport/`. See [Transports](../3_qb_io/transports.md).

**URI (`qb::io::uri`)** — A `qb-io` class that parses and represents Uniform Resource Identifiers (RFC 3986). Defined in `include/qb/io/uri.h`. See [Utilities](../3_qb_io/utilities.md).

**`use<>` (`qb::io::use<DerivedActor>`)** — A CRTP helper that injects asynchronous I/O behavior — TCP client/server, UDP endpoint, an `io_handler` session pool — into an actor by supplying the right base classes (for example `qb::io::use<MyClient>::tcp::client<>`). Defined in `include/qb/io/async.h`. See [Network actors](../5_core_io_integration/network_actors.md).

---

## Time vocabulary

These three types are the single source of truth for time across qb and its modules. Defined in `include/qb/system/timestamp.h`. The earlier capitalized spellings (the old `Timestamp` / `Duration` / `TimePoint` aliases) were removed; use the lowercase chrono types below. See [Async system](../3_qb_io/async_system.md).

**`qb::duration`** — An alias for `std::chrono::nanoseconds`: the span type used for every timeout, delay, TTL, interval, and core [latency](#latency-setlatency) in public APIs. `qb::duration::zero()` is the busy-spin / no-expiry sentinel depending on context.

**`qb::mono_time`** — A monotonic instant, `std::chrono::steady_clock::time_point`, obtained from `qb::mono_now()`. Use it for deadlines, timers, the event-loop "now", latency, and RTT — it is immune to wall-clock adjustments (NTP, DST).

**`qb::wall_time`** — A wall-clock instant, `std::chrono::system_clock::time_point`, obtained from `qb::wall_now()`. Use it for dates, expiry, token validity, logs, and wire formats. Epoch helpers include `qb::unix_seconds`, `qb::unix_millis`, `qb::unix_micros`, and `qb::unix_nanos`.

---

## Concurrency primitives and utilities

**Lock-free** — Describes data structures that allow concurrent access from multiple threads without mutexes, relying on atomic CPU operations. qb uses lock-free queues for inter-core event delivery. See [Concurrency](../2_core_concepts/concurrency.md), [Lock-free primitives](./lockfree_primitives.md).

**MPSC queue (multiple-producer, single-consumer)** — A queue allowing many producer threads but exactly one consumer thread, used for the inter-core [mailbox](#mailbox) (`qb::lockfree::mpsc::ringbuffer`). See [Lock-free primitives](./lockfree_primitives.md).

**SPSC queue (single-producer, single-consumer)** — A queue with exactly one producer and one consumer (`qb::lockfree::spsc::ringbuffer`). See [Lock-free primitives](./lockfree_primitives.md).

**Pipe (memory buffer — `qb::allocator::pipe<T>`)** — A dynamically resizable memory buffer used throughout qb for I/O buffering and event serialization. Distinct from the inter-actor [communication `Pipe`](#pipe-communication-channel-qbpipe). Defined in `include/qb/system/allocator/pipe.h`.

**RAII (Resource Acquisition Is Initialization)** — The C++ technique of binding resource lifetime to object lifetime — acquire in the constructor, release in the destructor — the recommended way to manage memory, sockets, and file handles inside actors.

**Spinlock (`qb::lockfree::SpinLock`)** — A mutual-exclusion lock that busy-waits (spins) instead of yielding, suited to very short, low-contention critical sections. Defined in `include/qb/system/lockfree/spinlock.h`. See [Lock-free primitives](./lockfree_primitives.md).

**`qb::string<N>`** — A fixed-capacity string holding up to `N` characters inline without heap allocation, preferred over `std::string` for direct event members because it is trivially destructible — a requirement for [`send`](#send-actorsendt) — and ABI-stable. Defined in `include/qb/string.h`.

**UUID (`qb::uuid`)** — A Universally Unique Identifier (RFC 4122). `qb::generate_random_uuid()` produces version-4 UUIDs. Defined in `include/qb/uuid.h`. See [Utilities](../3_qb_io/utilities.md).

---

**See also:** [API overview](./api_overview.md) · [Frequently asked questions](./faq.md) · [Core invariants](./core_invariants.md) · [qb-io invariants](./io_invariants.md) · [Lock-free primitives](./lockfree_primitives.md)
