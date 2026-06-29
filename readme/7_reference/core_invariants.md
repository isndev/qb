# qb-core thread-safety and lifecycle invariants

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 2.6.0 (C++20 default, C++23 supported) — 577606c

The rules `qb-core` assumes and the guarantees it gives in return: which thread owns what, when an actor is alive, and in what order events arrive — so you can reason about correctness without reading the scheduler.

**Prerequisites:** [Threading model](../2_core_concepts/threading_model.md), [Actor model](../2_core_concepts/actor_model.md) — **See also:** [Event system](../2_core_concepts/event_system.md), [qb-io invariants](./io_invariants.md), [API overview](./api_overview.md), [FAQ](./faq.md)

## Summary

`qb-core` is a share-nothing actor runtime. Its correctness rests on one structural decision — **each `VirtualCore` runs on exactly one thread and never shares actor state** — from which every other invariant follows. Because actors are strictly thread-affine, almost nothing in the actor layer needs an atomic, a lock, or a memory fence; the few synchronization primitives that exist guard cross-thread initialization and engine-wide shutdown, not the hot path.

This page consolidates the invariants you must respect (the contract you owe the runtime) and the guarantees the runtime gives back (the contract it owes you). Each is cited to the header or source that enforces it. Breaking one of these does not raise a compile error and often does not crash immediately — it corrupts state silently — so treat them as load-bearing.

## Thread model: one VirtualCore, one worker thread

- Every `qb::VirtualCore` runs on its own `qb::jthread` for the life of the engine. `qb::jthread` aliases `std::jthread` when available and falls back to qb's C++20 implementation otherwise. `qb::Main::start(bool async = true)` spawns one worker per registered core (`source/core/src/Main.cpp:256`); the threads are joined by `Main::join()` or by `~Main()`, whose `qb::jthread` members auto-join (`source/core/src/Main.cpp:172`).
- The active worker is identified by a `thread_local` pointer, `VirtualCore::_handler`, installed by `Main::start_thread` before any actor is instantiated on that worker and never reassigned for the worker's lifetime (`source/core/src/Main.cpp:190`).
- **Actors never migrate between cores.** An actor created on core *N* lives, receives events, and is destroyed on core *N*. Its `this` pointer is only dereferenceable on that one thread. A `VirtualCore` owns its actors exclusively; the actor maps and the service-id pool perform no synchronization (`include/qb/core/VirtualCore.h:172`).
- Cross-actor APIs (`to()`, `push<>()`, `send<>()`, `broadcast<>()`, `qb::Pipe`) never touch the destination actor directly. They enqueue an event into the destination core's mailbox; that core's worker dequeues and dispatches it **on its own thread**.

The consequence is that `qb-core` carries no `std::mutex` on the message path. The atomics that exist are confined to cross-thread service-id registration and the engine-wide `qb::stop_source` used for shutdown (`source/core/src/Main.cpp:172`). See [Memory ordering](#memory-ordering-cheat-sheet) for the full accounting.

## Actor lifecycle

### Construction

- An actor must be constructed from inside a `VirtualCore` worker thread, never from the main thread or an arbitrary user thread. Every `Actor` constructor asserts `VirtualCore::_handler != nullptr` (`source/core/src/Actor.cpp:32`). Construct actors only through `Main::core(idx).addActor<T>(...)`, `CoreInitializer::addActor<T>(...)`, or `addRefActor<T>(...)` / `addRefHandle<T>(...)` from within another actor on the same core.
- Default event registrations (`KillEvent`, `SignalEvent`, `UnregisterCallbackEvent`, `PingEvent`, `RequireEvent`) can be opted out by passing `qb::no_default_events` to the protected constructor (`include/qb/core/Actor.h:277`). Use this for ephemeral, pool-reused actors that never receive those control events.

### Initialization

- `Actor::onInit()` is an async coroutine (`qb::io::async::task<bool>`) invoked once by the owning `VirtualCore` on that core's worker thread, before the actor enters the event loop; it may `co_await` (the actor is *Activating* until it resumes to completion). `co_return false` (or an uncaught exception) cancels registration and the actor is destroyed immediately; `co_return true` activates it. Subscribe to events with `registerEvent<T>(*this)` inside `onInit()`.

### Steady state

- Event handlers (`on(...)`), `on(qb::LoopEvent const&)`, `onInit()`, and any coroutine resumed inside the actor all execute on the owning core's worker thread. You may read and write `this` members without synchronization.
- `on(qb::LoopEvent const&)` (from `qb::ICallback`) runs once per `VirtualCore` loop iteration, near the end of the iteration: the worker flushes its outbound pipes, drains its inbound mailbox, removes any actors killed during the drain, and only then dispatches callbacks (`source/core/src/VirtualCore.cpp:408`). The `qb::LoopEvent` carries per-loop context (`now`, `iteration`). Events an `on(qb::LoopEvent const&)` pushes are therefore flushed at the start of the next iteration. It must be fast and non-blocking; blocking it stalls the entire core and every actor on it (`include/qb/core/ICallback.h:16`).
- `Actor::time()` returns the `VirtualCore`'s cached nanosecond timestamp, refreshed once per loop iteration, so it is constant within a single handler or `on(qb::LoopEvent const&)` invocation (`include/qb/core/Actor.h:512`). For a continuously updating value use `qb::unix_nanos(qb::wall_now())` from `<qb/system/time.h>`.
- `Actor::_alive` is a plain `bool`, not an atomic, by design: it is single-writer / single-reader, always touched on the one owning thread, because remote senders enqueue a `KillEvent` rather than flipping the flag (`include/qb/core/Actor.h:218`). External code **must not** read `_alive`. To test liveness of a referenced actor, dereference a `qb::RefActorHandle<T>` (below), which re-queries the owning core.

### Destruction

- `Actor::kill()` sets `_alive = false` and calls `VirtualCore::killActor(id())` (`source/core/src/Actor.cpp:121`). It only *flags* the actor: the actor stops receiving new events but may still drain events already queued, and `~Actor()` runs later under `VirtualCore` control, at the end of the workflow iteration.
- `Actor::is_alive()` reports `true` until `kill()` has been called *and* the `VirtualCore` has processed the removal. `Actor::is_active()` is stricter — `is_alive()` **and** the actor's `onInit()` has completed — so it is `false` during the brief *Activating* window of a suspended async `onInit()`; it is the phase oracle behind `findActor` / `ActorHandle::get()`.
- A referenced actor obtained with `addRefActor<T>()` returns a phase-aware **`qb::ActorHandle<T>`** (alias `RefActorHandle<T>`), **not** a raw pointer. The handle never dangles: it caches the `ActorId` and resolves the live pointer on demand via `findActor<T>()`, so `get()` / `operator->()` return `nullptr` while the child is Activating, after a failed init, or once it has called `kill()`. Send to `handle.id()` at any time (events to a still-Activating child are stashed and replayed FIFO once active); gate direct method calls on `handle.ready()` (or `co_await handle.ready_async(context())` for an async-init child).
- An `ActorHandle<T>` may only be dereferenced from the owning `VirtualCore`'s worker thread — the thread that created the referenced actor. Cross-thread or cross-core dereference is undefined behavior, *not* a diagnosed one: `get()` (`include/qb/core/Actor.tpp:74`) is `noexcept` and simply returns `nullptr` when the handle cannot be resolved on the current core (it reads the thread-local `VirtualCore::_handler`, which is null off any worker thread, and `findActor` misses on the wrong core). `operator->()` / `operator*()` then `assert` only that the resolved pointer is non-null — a generic "actor not resolvable on this core" check that also fires after the actor dies — and there is no dedicated thread-identity assertion. Release builds perform no check at all, so cross-thread misuse is a silent null dereference (`include/qb/core/Actor.h:1238`, `:1245`).
- Service-actor ids are reserved for the lifetime of the process. `VirtualCore::removeActor` recycles only non-service ids back into the pool, keeping each `ServiceIndex` stable (`source/core/src/VirtualCore.cpp:509`).

## Event system

### Identity

- Each event type has a stable 16-bit `qb::TypeId` assigned at first use through a magic-static, atomically incremented counter (`qb::type_id<T>()`, `include/qb/core/Event.h:61`). `ServiceActor<Tag>` indices are allocated once under that same magic-static guarantee, with a mutex guarding the shared service-id map insertion, so the id is unique and valid even under concurrent first use (`include/qb/core/Actor.tpp:153`).
- A `type_id<T>()` value is stable within a single process run but is **not** stable across runs. Do not persist it.

### Identifiers

- `qb::ActorId` is a 32-bit compound of a `ServiceId` (the actor's slot within its core) and a `CoreId` (which `VirtualCore` hosts it). Default-construction yields `ActorId::NotFound` (`== 0`, invalid); `is_valid()` checks the value against `NotFound` (`include/qb/core/ActorId.h:403`).
- `ActorId::BroadcastSid` (`ServiceId` max) marks an id as a broadcast target. A `qb::BroadcastId(core)` used with `push<>()` delivers to every actor on one core; `Actor::broadcast<Event>()` fans out to every active core (`include/qb/core/Actor.h:849`).

### Sending: push vs send

| Primitive | Ordering | Constraint | Use when |
|---|---|---|---|
| `push<Event>(dest, args...)` | FIFO per (source, dest) pair | any event type, including non-trivial members | the default — most sends |
| `send<Event>(dest, args...)` | **unordered** | event type must be trivially destructible | order is irrelevant and you have measured a need to skip ordering |
| `broadcast<Event>(args...)` | per-core independent | any event type | fan-out to every active core |

- `push<Event>()` guarantees ordered delivery to the same destination from the same source (`include/qb/core/Actor.h:728`, mirrored by `qb::Pipe::push`, `include/qb/core/Pipe.h:118`). It returns a mutable reference to the event in the pipe buffer; you may set fields on it before it is consumed, but you must not store the reference past the current scope.
- `send<Event>()` is unordered and **requires trivially-destructible events for the EventQOS0-derived (`QoS < 2`) path** — `VirtualCore::fill_event` enforces this with `static_assert(std::is_trivially_destructible_v<T>, ...)` gated on `event_qos0_type<T>` (`include/qb/core/VirtualCore.tpp:130-132`). Such events holding `std::string`, `std::vector`, and similar non-trivial members are rejected at compile time. `qb::string<N>` and POD payloads are fine. Prefer `push()` unless you have measured a need.

> The 16-bit `EventId` keeps an event's metadata (`state`, `bucket_size`, `id`, `dest`, `source`) within one cacheline. This is a deliberate trade-off; do not assume room for a wider id.

### reply and forward

- `reply(event)` returns a received event to its source by swapping `dest` and `source` (`source/core/src/Actor.cpp:135`). `forward(dest, event)` re-routes it to a new destination while **preserving the original source** (`source/core/src/Actor.cpp:144`).
- Both reuse the received event object, so the `on()` handler must take the event by non-const reference. After the call the event is consumed and must not be touched again.
- Broadcast events cannot be replied to or forwarded; the attempt is logged and dropped (`source/core/src/Actor.cpp:137`, `source/core/src/Actor.cpp:149`).

### Events must be trivially relocatable

The pipe buffer relocates existing event buckets when it grows. Events must therefore be **trivially relocatable**: no self-pointers, and no member that registers itself with an external registry from its constructor or destructor. PODs, `qb::string<N>`, `std::string` with SSO, and `std::unique_ptr` satisfy this. If an event must hold a self-reference, redesign it to keep the indirection elsewhere.

### noexcept on the message path

`push()`, `send()`, `broadcast()`, and `Pipe::push()` / `Pipe::allocated_push()` are all `noexcept`, yet they may grow the pipe buffer (which can throw `std::bad_alloc`) or run an event constructor that throws. Because a throw cannot escape a `noexcept` function, **any such failure calls `std::terminate()` and aborts the process** (`include/qb/core/Actor.h:723`, `include/qb/core/Pipe.h:126`). This is intentional, not a recoverable error. Keep events small and their constructors allocation-light.

### Bounded inter-core flush (no cross-core deadlock)

`VirtualCore::__flush_all__` drains outbound pipes into peer mailboxes (`source/core/src/VirtualCore.cpp:219`). When a peer's mailbox is full:

- **Best-effort events** (`event.state.qos == 0`): a single `try_send` attempt, then dropped on failure.
- **QoS-guaranteed events**: a bounded spin-then-yield backoff before partial flush, with tunables `kFlushSpinAttempts = 64` and `kFlushYieldAttempts = 512` (`source/core/src/VirtualCore.cpp:213`):
  - attempts `[0, 64)` — `qb::spin_loop_pause()` (CPU hint, no scheduler involvement);
  - attempts `[64, 512)` — `std::this_thread::yield()` (give the peer consumer a slot);
  - at `512` — partial flush: keep the unsent tail in the pipe, notify the peer, and return to the workflow.

Termination is guaranteed in bounded time: after a partial flush the workflow drains its own mailbox, which frees space for peers, whose next `try_send` then succeeds. The design admits no cross-core deadlock and no starvation.

## Coroutines inside actors

- Two entry points run a C++20 coroutine inside an actor: `Actor::spawn(func)` (recommended — *scoped*, detailed below) and `Actor::spawn_detached(func)` (*detached*) (`include/qb/core/Actor.h:1080` and `:1043`). Both must be called from the actor's worker thread; a debug assertion checks that a thread-local coroutine scheduler exists on the calling thread (`include/qb/core/Actor.tpp:269` and `:256`).
- A coroutine spawned this way runs in an **isolated context** and **must not access actor members after any `co_await`** — the actor may be destroyed while the coroutine is suspended, which is undefined behavior (`include/qb/core/Actor.h:988`). Capture everything you need by value before the first suspension point, and communicate back only through the `qb::CoroContext` handed to the lambda: `ctx.push<Event>(...)`, `ctx.id()`, `ctx.time()`. The context carries the actor's `ActorId` by value, never a raw `this`.
- The active-coroutine counter is an eagerly allocated `shared_ptr<atomic<size_t>>` (`include/qb/core/Actor.h:1167`), so each `spawn`/`spawn_detached` is a single `fetch_add` plus a spawn on the hot path. Query it with `has_active_coroutines()` (`include/qb/core/Actor.h:1087`) or `active_coroutine_count()` (`include/qb/core/Actor.h:1123`).
- `Actor::spawn(func)` is the **recommended** variant: the coroutine is bound to a per-actor cancellation scope (`include/qb/core/Actor.h`). When the actor is killed/destroyed, the scope is cancelled (cooperatively) by `kill()` and, as a catch-all, by `VirtualCore::removeActor`. The lambda receives a `qb::ScopedCoroContext` (a superset of `CoroContext`) whose `ctx.sleep(...)`, `ctx.until_cancelled()`, `ctx.cancellation_point()` and `ctx.cancellable(task)` helpers are cancellation-aware: a scoped coroutine awaiting one of them **wakes within the next loop iteration, throws `qb::io::async::cancelled_error`, and unwinds cleanly** (RAII + `catch` run) instead of blocking on a long timeout/I/O. This makes actor coroutines safe and bounded by construction.
- Cancellation is **cooperative, not forced**: a scoped coroutine awaiting a *non*-cancellation-aware operation (a bare `qb::io::async::sleep`, a raw socket await) is not interrupted — it falls back to the same "orphan-and-complete" behavior as `spawn_detached`, which is safe. `spawn_detached` itself is unchanged: its coroutine stays **detached** and runs to completion even after its actor dies. The capture rule still applies to both — **never capture `this`**; the scope bounds the coroutine's lifetime, it does not legalize member access after a `co_await`.
- **`ask` (request/response).** Inside a scoped coroutine, `co_await qb::ask(ctx, target, req, timeout)` (a free function from `qb/patterns.h`) sends a request and resolves to the reply, with correlation, timeout (`timeout_error`) and cancel-on-kill (`cancelled_error`) handled by a single awaiter — **no detached helper task**. `E` derives from `qb::AskEvent` and round-trips through one event type: the responder fills the response fields and `reply()`s it back (preserving the correlation id), and the asker routes replies by calling `resolve_ask(e)` first in its own `on(E&)` handler. The correlation registry is a per-worker-thread `thread_local` map (mono-thread, no locks); slots live inside the awaiter (in the asking coroutine's frame) and are auto-unregistered on resume/timeout/cancel/destroy.
- The strict mono-thread rule for the coroutine layer is documented in [qb-io invariants](./io_invariants.md).

## CPU affinity and shutdown

- `qb::CoreInitializer::setAffinity(CoreIdSet)` pins a worker's thread, best-effort. A logical `CoreId` need not map to a physical CPU, so a failed `pthread_setaffinity_np` / `SetThreadAffinityMask` only warns and never fails `VirtualCore` init (`source/core/src/VirtualCore.cpp:302`). Core ids `>= qb::MaxCores`, including the `qb::NoAffinity` sentinel, are filtered out before pinning — pass a set containing only `qb::NoAffinity` to opt out of pinning explicitly.
- Shutdown has three triggers wired to the same plumbing (`source/core/src/Main.cpp:172`): a POSIX signal (`SIGINT` / `SIGTERM` via `sigaction`), `Main::stop()` setting a `std::atomic<std::sig_atomic_t>` that the workflow polls, and the C++20 `qb::stop_source` (`request_stop()` on `~Main` or programmatically). A worker that observes any of them synthesizes a virtual `SIGINT` and broadcasts a `SignalEvent`, so existing shutdown handlers keep working on platforms with or without POSIX signals.

## Memory ordering cheat-sheet

| Data | Access pattern | Why it is safe |
|---|---|---|
| `Actor::_alive` | plain read/write | single-thread owner (`include/qb/core/Actor.h:218`) |
| `Actor` member fields | plain read/write | same |
| `VirtualCore` actor maps / callback list | plain read/write | mutated only on the owning worker thread (`include/qb/core/VirtualCore.h:172`) |
| `Main` signal flag | `std::atomic<std::sig_atomic_t>` | async-signal-safe poll in the worker loop |
| `Main::_stop_source` / `_stop_token` | `qb::stop_*` | standard library ordering when backed by `std::stop_*`; acquire/release atomic ordering in qb's fallback |
| Service-id registration | atomic + magic-static + mutex | one-time cross-thread publish (`include/qb/core/Actor.tpp:153`) |
| Inter-core mailbox | lock-free MPSC ring buffer | own internal acquire/release (see [lock-free primitives](./lockfree_primitives.md)) |

There is no `std::mutex` on the message path. The only locks in `qb-core` guard the one-time service-id map (static-init only) and the mailbox condition variable used when a core parks while truly idle at non-zero latency.

## Pitfalls

- **Touching another actor's state directly.** Calling a public method via `handle->` bypasses the mailbox and the actor model's guarantees. It is safe only when `handle.ready()` (the actor is active); on a non-ready handle `get()` returns `nullptr` (debug `assert`), so never `operator->` an Activating / dead handle. Prefer sending an event to `handle.id()`.
- **Reading `_alive` from outside the owning thread.** It is not atomic; a cross-thread read is a data race (`include/qb/core/Actor.h:218`). Use `qb::ActorHandle<T>` (alias `RefActorHandle<T>`).
- **Capturing `this` (or any member by reference) into a `spawn_detached` coroutine.** The actor can die while the coroutine is suspended (`include/qb/core/Actor.h:988`). Capture by value; reach the actor only through `CoroContext`.
- **Blocking the worker thread.** `std::this_thread::sleep_for`, a heavy syscall, or a long computation in a handler or `on(qb::LoopEvent const&)` stalls every actor on the core (`include/qb/core/ICallback.h:16`). Offload to a coroutine or a dedicated I/O actor.
- **Letting an event constructor throw under load.** The `noexcept` message path terminates the process on a thrown exception (`include/qb/core/Pipe.h:126`). Keep events small and allocation-light.
- **Using `send()` for ordered work.** `send()` is unordered and rejects non-trivially-destructible events at compile time on the EventQOS0-derived (`QoS < 2`) path (`include/qb/core/VirtualCore.tpp:130-132`). When ordering matters, use `push()`.
- **Persisting `type_id<T>()`.** It is process-local and not stable across runs.

## See also

- [Threading model](../2_core_concepts/threading_model.md) — the one-core-one-thread design in narrative form.
- [Event system](../2_core_concepts/event_system.md) — event types, QoS levels, and routing.
- [qb-io invariants](./io_invariants.md) — the matching contract for the async I/O layer and coroutine scheduler.
- [API overview](./api_overview.md) · [FAQ](./faq.md) · [Glossary](./glossary.md)
