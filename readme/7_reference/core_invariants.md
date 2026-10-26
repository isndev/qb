# qb-core thread-safety and lifecycle invariants

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 3.3.0 (C++20 default, C++23 supported) — 2d68d3a

The rules `qb-core` assumes and the guarantees it gives in return: which thread owns what, when an actor is alive, and in what order events arrive — so you can reason about correctness without reading the scheduler.

**Prerequisites:** [Threading model](../2_core_concepts/threading_model.md), [Actor model](../2_core_concepts/actor_model.md) — **See also:** [Event system](../2_core_concepts/event_system.md), [qb-io invariants](./io_invariants.md), [API overview](./api_overview.md), [FAQ](./faq.md)

## Summary

`qb-core` is a share-nothing actor runtime. Its correctness rests on one structural decision — **each `VirtualCore` runs on exactly one thread and never shares actor state** — from which every other invariant follows. Because actors are strictly thread-affine, almost nothing in the actor layer needs an atomic, a lock, or a memory fence; the few synchronization primitives that exist guard cross-thread initialization and engine-wide shutdown, not the hot path.

This page consolidates the invariants you must respect (the contract you owe the runtime) and the guarantees the runtime gives back (the contract it owes you). Each is cited to the header or source that enforces it. Breaking one of these does not raise a compile error and often does not crash immediately — it corrupts state silently — so treat them as load-bearing.

## Thread model: one VirtualCore, one worker thread

- With default `start(true)`, each `qb::VirtualCore` runs on a `qb::jthread`; `start(false)` runs the last core on the calling thread. `qb::jthread` aliases `std::jthread` when available and falls back to qb's C++20 implementation otherwise. `qb::Main::start(bool async = true)` spawns one worker per registered core in async mode (`src/qb/core/Main.cpp:851-874`); the threads are joined by `Main::join()` or by `~Main()`, whose `qb::jthread` members auto-join (`src/qb/core/Main.cpp:553-567,912-927`).
- The active worker is identified by a `thread_local` pointer, `VirtualCore::_handler`, bound by `VirtualCore::__bind_thread__` -- together with the core index `Event::__broadcast_here__` compares a broadcast's to, `Event::__core_scope__` (Huly QB-268) -- from `Main::start_thread` before any actor is instantiated on that worker, and never reassigned for the worker's lifetime (`src/qb/core/Main.cpp:671`).
- **Actors never migrate between cores.** An actor created on core *N* lives, receives events and is destroyed on core *N*. Its state is dereferenceable only on that thread. Ordinary/service registry banks and incarnation metadata are exclusively owned by that worker and use no synchronization. Source: `src/qb/core/VirtualCore.h` (`ActorMap`, `_actors`, `_service_actors`, `_ids`), `src/qb/core/detail/actor_id_pool.h` (`ActorIdPool`).
- Cross-actor APIs (`to()`, `push<>()`, `send<>()`, `broadcast<>()`, `qb::Pipe`) never touch the destination actor directly. They enqueue an event into the destination core's mailbox; that core's worker dequeues and dispatches it **on its own thread**.

The common unparked enqueue path uses SPSC atomics and fences without a mutex. When the consumer is parked, mailbox `notify()` takes its mutex to coordinate the wake; `_parked` and the ring cursors are atomic (`src/qb/core/Main.h:514-527`, `:667-700`). Service-id registration and the engine-wide `qb::stop_source` add separate synchronization. See [Memory ordering](#memory-ordering-cheat-sheet) for the full accounting.

## Actor lifecycle

### Construction

- An actor must be constructed from inside a `VirtualCore` worker thread, never from the main thread or an arbitrary user thread. Every `Actor` constructor asserts `VirtualCore::_handler != nullptr` (`src/qb/core/Actor.cpp:593`). Construct actors only through `Main::core(idx).addActor<T>(...)`, `CoreInitializer::addActor<T>(...)`, or `addRefActor<T>(...)` / `addRefHandle<T>(...)` from within another actor on the same core.
- Default event registrations (`KillEvent`, `SignalEvent`, `UnregisterCallbackEvent`, `PingEvent`, `RequireEvent`) can be opted out by passing `qb::no_default_events` to the protected constructor (`src/qb/core/Actor.h:437-440,448`). Use this for ephemeral, pool-reused actors that never receive those control events. Such an actor must still register `qb::SignalEvent` if it is to honour `Main::stop()` or a terminal signal: those are synthesised per core as a `SignalEvent` (`src/qb/core/VirtualCore.cpp:1683-1688`) and nothing in the engine ever sends a `KillEvent`.

### Initialization

- `Actor::onInit()` is an async coroutine (`qb::io::async::task<bool>`) invoked once by the owning `VirtualCore` on that core's worker thread, before the actor enters the event loop; it may `co_await` (the actor is *Activating* until it resumes to completion). `co_return false` (or an uncaught exception) cancels registration and the actor is destroyed immediately; `co_return true` activates it. Subscribe to events with `registerEvent<T>(*this)` inside `onInit()`.

### Steady state

- Event handlers (`on(...)`), `on(qb::LoopEvent const&)`, `onInit()`, and any coroutine resumed inside the actor all execute on the owning core's worker thread. You may read and write `this` members without synchronization.
- `on(qb::LoopEvent const&)` (from `qb::ICallback`) runs once per `VirtualCore` loop iteration, near the end of the iteration: the worker flushes its outbound pipes, drains its inbound mailbox, removes any actors killed during the drain, and only then dispatches callbacks (`src/qb/core/VirtualCore.cpp:1041-1067`). The `qb::LoopEvent` carries per-loop context (`now`, `iteration`). Events an `on(qb::LoopEvent const&)` pushes are therefore flushed at the start of the next iteration. It must be fast and non-blocking; blocking it stalls the entire core and every actor on it (`src/qb/core/ICallback.h:16-19`).
- `Actor::time()` returns the `VirtualCore`'s cached nanosecond timestamp, refreshed once per loop iteration, so it is constant within a single handler or `on(qb::LoopEvent const&)` invocation (`src/qb/core/Actor.h:804-822`). For a continuously updating value use `qb::unix_nanos(qb::wall_now())` from `<qb/system/time.h>`.
- `Actor::_alive` is a plain `bool`, not an atomic, by design: it is single-writer / single-reader, always touched on the one owning thread, because remote senders enqueue a `KillEvent` rather than flipping the flag (`src/qb/core/Actor.h:345`). External code **must not** read `_alive`. To test liveness of a referenced actor, dereference a `qb::RefActorHandle<T>` (below), which re-queries the owning core.

### Destruction

- `Actor::kill()` sets `_alive = false` and calls `VirtualCore::killActor(id())` (`src/qb/core/Actor.cpp:745-758`). It only *flags* the actor: the actor stops receiving new events while the core still drains and disposes events already queued, and `~Actor()` runs later under `VirtualCore` control, at the end of the workflow iteration.
- `Actor::is_alive()` becomes `false` as soon as `kill()` sets `_alive = false`, before the later removal (`src/qb/core/Actor.cpp:746-757`). `Actor::is_active()` is stricter — `is_alive()` **and** the actor's `onInit()` has completed — so it is `false` during the brief *Activating* window of a suspended async `onInit()`. It is the phase oracle, and it has **exactly two** consumers: `VirtualCore::findActor<T>()` (hence every `ActorHandle` accessor — `get()`, `operator->`, `ready()`, `operator bool`) and `VirtualCore::isActorAlive()` (hence `is_actor_alive()`). `getService<T>()` is deliberately **not** one of them, and the inbound-event dispatch gate keys off `VirtualCore::_activating` membership rather than this flag (`src/qb/core/Actor.h:365-372`).
- A referenced actor obtained with `addRefActor<T>()` returns a phase-aware **`qb::ActorHandle<T>`** (alias `RefActorHandle<T>`), **not** a raw pointer. Check `valid()` first: terminal teardown can refuse creation and return an empty handle. A valid handle never dangles: it caches the `ActorId` and resolves the live pointer on demand via `findActor<T>()`, so `get()` returns `nullptr` while the child is Activating, after failed init, or once it has called `kill()`. `operator->()` requires `ready()` and asserts in debug when unavailable (`src/qb/core/Actor.h:2568-2573`). Send to a valid `handle.id()` while the child is Activating (events are stashed and replayed FIFO once active); gate direct method calls on `handle.ready()` (or `co_await handle.ready_async(context())` for an async-init child).
- An `ActorHandle<T>` may only be dereferenced from the owning `VirtualCore`'s worker thread — the thread that created the referenced actor. Cross-thread or cross-core dereference is undefined behavior, *not* a diagnosed one: `get()` (`src/qb/core/VirtualCore.h:1461-1472`) is `noexcept` and simply returns `nullptr` when the handle cannot be resolved on the current core (it reads the thread-local `VirtualCore::_handler`, which is null off any worker thread, and `findActor` misses on the wrong core). `operator->()` / `operator*()` then `assert` only that the resolved pointer is non-null — a generic "actor not resolvable on this core" check that also fires after the actor dies — and there is no dedicated thread-identity assertion. Release builds perform no check at all, so cross-thread misuse is a silent null dereference (`src/qb/core/Actor.h:2570-2573`, `:2578-2581`).
- Service tags remain process-stable, but every actual service has a nonzero-generation instance ID. Both failed admission and final removal release the incarnation and advance its generation; an exhausted slot retires instead of wrapping. Ordinary slots are independent of service tags. Source: `src/qb/core/VirtualCore.cpp` (`__rollback_failed_admission__`, `removeActor`, `ActorIdPool::release`).
- Initial factories and dynamic adds reserve identity/storage before the user allocator/constructor runs. Each factory binds its own promised ID; nested child construction cannot steal another promise. Startup moves the pool into the core and terminal teardown returns its metadata only after actors/frames/pipes are cleaned up. Source: `src/qb/core/Main.cpp` (`Main::start_thread`), `src/qb/core/VirtualCore.h` (`addReferencedActor`), `src/qb/core/VirtualCore.cpp` (`__install_id_pool__`, `VirtualCore::~VirtualCore`).
- Exhaustion refuses dynamic admission before construction/`onInit()`. A failed constructor's queued events carry the old full identity and cannot hit the next owner of its slot. Source: `src/qb/core/VirtualCore.h` (`addReferencedActor`, `__actor_slot__`), `src/qb/core/VirtualCore.cpp` (`__rollback_failed_admission__`).
- A handle retains the complete instance ID; it never resolves a replacement within the engine run. `ready_async()` captures its target at the call, so moving/reassigning/destroying the handle before awaiting the returned task does not change that task's identity. Source: `src/qb/core/Actor.h` (`ActorHandle::ready_async`, `ready_async_impl`), `src/qb/core/VirtualCore.h` (`ActorHandle::get`).


## Event system

### Identity

- Each event type has a process-stable 16-bit `TypeId` assigned through `type_id<T>()`. Service tags separately receive process-stable `ServiceId` indices through `Actor::registerIndex<Tag>()`, including its registry fallback before `ServiceIndex` initializes. First-time service registration while a core lives remains refused; the policy no longer supplies an ordinary-pool floor. Source: `src/qb/core/Event.h` (`type_id`), `src/qb/core/VirtualCore.h` (`Actor::registerIndex`), `src/qb/core/VirtualCore.cpp` (`__register_service__`).
- A `type_id<T>()` value is stable within a single process run but is **not** stable across runs. Do not persist it.

### Identifiers

- `ActorId` is one 64-bit numeric value: `[generation:32 | core:8 | slot:24]`; public `CoreId` remains 16 bits and encoded cores are 0..255. Ordinary slots `1..0xFEFFFF` provide 16,711,679 arithmetic positions per core before reservations, retired slots and resource limits. Equality/hash include every bit. Source: `src/qb/core/ActorId.h` (`ActorId`, `std::hash<qb::ActorId>`).
- Service tags `1..65534` occupy `0xFF0000 | tag`. Generation zero is a logical service address; actual ordinary/service instances use nonzero generation. Slot zero and the service zero-tag slot are invalid. `BroadcastSid == 0xFFFFFF` is valid only with generation zero. `is_valid()` accepts instance, logical-service and broadcast values; use `is_instance()` for exact actor identity. Source: `src/qb/core/ActorId.h` (`service_address`, `service_instance`, `is_valid`, `BroadcastId`).
- A logical service destination binds once to its current instance at receipt, before activation/stashing; stashed traffic cannot follow a replacement. An exact instance event/handle/waiter never follows a reused slot. Logical watch tickets retain the requested address and report the instance present when registration reaches its core; instance watches retain the full generation. Source: `src/qb/core/VirtualCore.cpp` (`__receive_events__`, `__register_watch__`, `__on_actor_down__`).
- Identity safety is scoped to one engine run; IDs carry no engine/node epoch or durable/process-wide uniqueness. Normal `join()` does not clear a successfully started Main's running state: `core()` still rejects configuration and `start()` returns. Source: `src/qb/core/Main.cpp` (`Main::join`, `Main::core`, `Main::start`).
- The checked full-width `from_raw` / `raw64()` codec replaces the removed 32-bit constructor/conversion and changes source compatibility/ABI. Rebuild all consumers/modules/plugins consistently. No release-version or performance conclusion follows from the layout. Source: `src/qb/core/ActorId.h` (`ActorId`).
- A `qb::BroadcastId(core)` used with `push<>()` delivers to every actor on one core -- one the engine runs: for any other core it reaches no actor and is a `not_found` dead letter on the first core, where every id outside the core set lands (Huly QB-268); a core id past `qb::MaxCores` is no core at all, and `BroadcastId` gives the invalid id. `Actor::broadcast<Event>()` fans out to every active core (`src/qb/core/Actor.h:1255-1262`).

### Sending: push vs send

| Primitive | Ordering | Constraint | Use when |
|---|---|---|---|
| `push<Event>(dest, args...)` | FIFO per (source, dest) pair | any event type, including non-trivial members | the default — most sends |
| `send<Event>(dest, args...)` | **unordered** | trivially destructible — enforced for `EventQOS0`, a guideline otherwise | order is irrelevant and you have measured a need to skip ordering |
| `broadcast<Event>(args...)` | per-core independent | any event type | fan-out to every active core |

- `push<Event>()` (`src/qb/core/Actor.h:1129`) guarantees ordered delivery to the same destination from the same source (`src/qb/core/Actor.h:1078-1080`, mirrored by `qb::Pipe::push`, `src/qb/core/Pipe.h:140-143`). It returns a mutable reference to the event in the pipe; **that reference lives until the handler or callback that obtained it returns**, whatever is queued in between — the pipe is segmented (`qb::VirtualPipe`, `src/qb/core/Event.h:983`), so a later `push`/`send`/`broadcast` to that core links a segment when it needs room and never reallocates or compacts what an earlier push placed. The engine consumes the event between handlers, which is when the reference dies: finish with it before a coroutine's first `co_await`, and never keep it in a member (`src/qb/core/Actor.h:1110-1122`).
- `send<Event>()` is unordered and **requires trivially-destructible events for the EventQOS0-derived (`QoS < 2`) path** — the assertion lives in `qb::detail::routing_safe_type_id<T>` (`src/qb/core/Event.h`), which every enqueue sink calls, plus the older one in `VirtualCore::fill_event` (`src/qb/core/VirtualCore.h:1251-1259`). Such events holding `std::string`, `std::vector`, and similar non-trivial members are rejected at compile time on all four spellings — `push`, `send`, `to(dest).push` and `getPipe(dest).allocated_push`. Until the check moved, the last two were unguarded and their payloads leaked on the drop path. `qb::string<N>` and POD payloads are fine. Prefer `push()` unless you have measured a need.

> `EventId` remains 16 bits while destination/source are complete 64-bit ActorIds. The occupied Event header is 24 bytes and the base is cache-line aligned. On the observed arm64/64-byte-bucket layout, a plain byte-array payload crosses from one bucket at 40 bytes to two at 41; other bases, fields or alignment change that boundary. Source: `src/qb/core/Event.h` (`Event`, `detail::event_wire`), `tests/core/unit/core/event-header.cpp` (payload-boundary assertions).

### reply and forward

- `reply(event)` returns a received event to its source by swapping `dest` and `source` (`src/qb/core/Actor.cpp:775-781`). `forward(dest, event)` re-routes it to a new destination while **preserving the original source** (`src/qb/core/Actor.cpp:784-792`).
- Both reuse the received event object, so the `on()` handler must take the event by non-const reference. After the call the event is consumed and must not be touched again.
- Broadcast events cannot be replied to or forwarded; the attempt is logged and dropped (`src/qb/core/Actor.cpp:776-779`, `src/qb/core/Actor.cpp:787-790`).
- Both are `send`-family, eager and unordered: cross-core the answer is published at once and precedes the handler's earlier pushes to the same actor. `push_reply(event)` / `push_forward(dest, event)` are the same answers through `push` — FIFO with everything this actor pushed to that destination, delivered at the end of the pass — with the same reuse, guard and consumption rules.

### Events must be trivially relocatable

The runtime relocates events with raw `memcpy`: it copies the bytes to a new address and abandons the source without running a destructor there. Events must therefore be **trivially relocatable**: no self-pointers, and no member that registers itself with an external registry from its constructor or destructor. PODs, `qb::string<N>`, `std::vector`, `std::unique_ptr` and `std::shared_ptr` satisfy this. A **by-value `std::string` does not**: a short one is self-referential on libstdc++ (`_M_p` addresses its own `_M_local_buf`), which corrupts on Linux and is structurally invisible under libc++/macOS. If an event must hold a self-reference, redesign it to keep the indirection elsewhere.

The requirement is **not** scoped to cross-core delivery. Two independent relocations exist:

- `reply`/`forward` byte-copy the received event into the self pipe, or into the remote producer ring with a buffered fallback when full (`src/qb/core/VirtualCore.cpp:1498-1630`; `src/qb/core/Main.cpp:343-419`). The delivered copy carries `alive == 0`, the original is raised after copying, and only the delivered owner is destroyed;
- the cross-core hop copies the event twice more (sender pipe → peer mailbox ring → receive buffer).

(Until 3.2 there was a third — the contiguous pipe `memcpy`d what it held on growth and `memmove`d it on compaction, on the sending core's own events too. The segmented pipe never moves an event, but the two above keep the requirement exactly as it was.)

The debug-only scan in `SharedCoreCommunication::send` (`src/qb/core/Main.cpp:257-299`) catches the common case but is not a proof of absence: it runs **only** on the cross-core hop, and it searches for a word addressing the event's *current* bytes, so a self-pointer that an earlier pipe growth already left dangling now points at the old buffer and is outside the scanned range. It is compiled out under `NDEBUG`.

### noexcept on the message path

`push()`, `send()`, `broadcast()`, and `Pipe::push()` / `Pipe::allocated_push()` are all `noexcept`, yet they may grow the pipe buffer (which can throw `std::bad_alloc`) or run an event constructor that throws. Because a throw cannot escape a `noexcept` function, **any such failure calls `std::terminate()` and aborts the process** (`src/qb/core/Actor.h:1123-1127`, `src/qb/core/Pipe.h:148-163`). This is intentional, not a recoverable error. Keep events small and their constructors allocation-light.

### Bounded inter-core flush (no cross-core deadlock)

`VirtualCore::__flush_all__` scans the peer pipes inline and enters `__flush_pipes__`, the drain of outbound pipes into peer mailboxes, once one of them holds events (`src/qb/core/VirtualCore.cpp:519-520`). A failed `try_send` falls into one of three cases, tested in this order:

- **Permanently unsendable — not backpressure.** An event wider than the peer's ring (`bucket_size > kMaxDeliverableBuckets`) can never be enqueued, because the ring enqueue is all-or-nothing: no amount of draining helps, and retrying would hold the whole FIFO pipe hostage behind it. The flush logs at `LOG_CRIT`, reports an `oversize` dead letter, **disposes** the event (its destructor runs) and skips it (`src/qb/core/VirtualCore.cpp:609-618`). A malformed `bucket_size == 0` — reachable only by overflowing that `uint16_t` field — cannot even be stepped over, so the rest of that pipe is discarded instead (`src/qb/core/VirtualCore.cpp:597-606`). Separating these two shapes from genuine backpressure is what keeps the flush terminating.
- **Best-effort events** (`event.state.bits.qos == 0`): a single `try_send` attempt, then dropped on failure — and dropped *without* being disposed, which is exactly why `routing_safe_type_id` `static_assert`s that an `EventQOS0`-derived event is trivially destructible.
- **QoS-guaranteed events**: a partial flush at once when the peer is stalled — its consumer index, as this core's producer ring last saw it, unmoved over `kFlushStallRefusals = 2` refusals in a row (`src/qb/core/VirtualCore.cpp:496-503`) — and the core has inbound work of its own — a peer's ring into it holds events, its self pipe holds the last pass's same-core pushes, or its io loop has something to deliver (`src/qb/core/VirtualCore.cpp:2114-2125`, Huly QB-979) — and otherwise a bounded spin-then-yield backoff before the partial flush, with tunables `kFlushSpinAttempts = 64` and `kFlushYieldAttempts = 512` (`src/qb/core/VirtualCore.cpp:486-487`):
  - attempts `[0, 64)` — `qb::spin_loop_pause()` (CPU hint, no scheduler involvement);
  - attempts `[64, 512)` — `std::this_thread::yield()` (give the peer consumer a slot);
  - at `512` — partial flush: keep the unsent tail in the pipe, notify the peer, and return to the workflow.

Termination is guaranteed in bounded time: after a partial flush the workflow drains its own mailbox, which frees space for peers, whose next `try_send` then succeeds. The design admits no cross-core deadlock and no starvation.

## Coroutines inside actors

- Two entry points run a C++20 coroutine inside an actor: `Actor::spawn(func)` (recommended — *scoped*, detailed below) and `Actor::spawn_detached(func)` (*detached*) (`src/qb/core/Actor.h:1531-1532` and `:1494-1495`). Both must be called from the actor's worker thread; a debug assertion checks that a thread-local coroutine scheduler exists on the calling thread (`src/qb/core/Actor.cpp:723-725`).
- A coroutine spawned this way runs in an **isolated context** and **must not access actor members after any `co_await`** — the actor may be destroyed while the coroutine is suspended, which is undefined behavior (`src/qb/core/Actor.h:1450-1452`). Capture everything you need by value before the first suspension point, and communicate back only through the `qb::CoroContext` handed to the lambda: `ctx.push<Event>(...)`, `ctx.id()`, `ctx.time()`. The context carries the actor's `ActorId` by value, never a raw `this`.
- The active-coroutine counter (`active_coroutines_`, `src/qb/core/Actor.h:1663`) is a `detail::coro_census` cell — a plain `size_t` of live frames plus an intrusive `uint32_t` handle count, held through `detail::coro_census_ref` and NEITHER of them atomic — allocated on the FIRST `spawn`/`spawn_detached` (`__ensure_coro_counter__` at `src/qb/core/Actor.h:1669-1672`), so an actor that never spawns pays nothing and each spawn is a predicted-not-taken null check, one plain increment and the spawn. It was eager until 3.2: measured on savina/fib, that one `make_shared` and its release were ~30 % of an actor's lifetime cost. It was a `shared_ptr<atomic<size_t>>` until 3.2 as well, and that cost too: the actor, its wrapper frames and the core that destroys it all live on one thread, so the atomic bought no safety and each of its `lock`-prefixed RMWs was a full fence on x86 — measured on savina/bank-transaction (one `qb::ask` per transfer, one core), atomic refcounting on the coroutine path was 29 % of the core's time, and the twin `shared_ptr` behind `cancellation_token` — copied into every `qb::ask` frame — most of it. Both are intrusive, same-thread counts now. Query it with `has_active_coroutines()` (`src/qb/core/Actor.h:1570-1571`) or `active_coroutine_count()` (`src/qb/core/Actor.h:1613-1614`).
- `Actor::spawn(func)` is the **recommended** variant: the coroutine is bound to a per-actor cancellation scope (`src/qb/core/Actor.h`). When the actor is killed/destroyed, the scope is cancelled (cooperatively) by `kill()` and, as a catch-all, by `VirtualCore::removeActor`. The lambda receives a `qb::ScopedCoroContext` (a superset of `CoroContext`) whose `ctx.sleep(...)`, `ctx.until_cancelled()`, `ctx.cancellation_point()` and `ctx.cancellable(task)` helpers are cancellation-aware: a scoped coroutine awaiting one of them **wakes within the next loop iteration, throws `qb::io::async::cancelled_error`, and unwinds cleanly** (RAII + `catch` run) instead of blocking on a long timeout/I/O. This makes actor coroutines safe and bounded by construction.
- Cancellation is **cooperative, not forced**: a scoped coroutine awaiting a *non*-cancellation-aware operation (a bare `qb::io::async::sleep`, a raw socket await) is not interrupted. Like `spawn_detached`, it may finish after its actor dies only while another actor keeps the same VirtualCore running; the last actor's removal exits that loop. The capture rule still applies to both — **never capture `this`**; the scope bounds cancellation, not member access after a `co_await`. <!-- src: qb/src/qb/core/VirtualCore.cpp:1064-1066; qb/src/qb/core/VirtualCore.h:1691-1702 -->
- **`ask` (request/response).** Inside a scoped coroutine, `co_await qb::ask(ctx, target, req, timeout)` (a free function from `qb/patterns.h`) sends a request and resolves to the reply, with correlation, timeout (`timeout_error`) and cancel-on-kill (`cancelled_error`) handled by a single awaiter — **no detached helper task**. `E` derives from `qb::AskEvent` and round-trips through one event type: the responder fills the response fields and `reply()`s it back (preserving the correlation id), and the asker routes replies by calling `resolve_ask(e)` first in its own `on(E&)` handler. The correlation registry is a per-worker-thread indexed table (mono-thread, no locks). Awaiter pointers are unregistered on resume/timeout/cancel/destroy; entries retain numeric ownership and generations afterward, so late responses remain recognisable after reuse. `resolve_ask` consumes those obsolete own responses without resumption. Per-owner queues and retained high-water capacity cost memory; generations retire instead of wrapping. An actor may ask itself (Huly QB-1100): its own handler answers; before its `onInit()` completes, a self-ask throws `std::logic_error` at once.
- The strict mono-thread rule for the coroutine layer is documented in [qb-io invariants](./io_invariants.md).

## CPU affinity and shutdown

- `qb::CoreInitializer::setAffinity(CoreIdSet)` pins a worker's thread, best-effort: a refused `pthread_setaffinity_np` / `SetThreadAffinityMask` only warns and never fails `VirtualCore` init (`src/qb/core/VirtualCore.cpp:656-662`). Core ids `>= qb::MaxCores`, including the `qb::NoAffinity` sentinel, are filtered out before pinning — pass a set containing only `qb::NoAffinity` to opt out of pinning explicitly. A core never given a set is pinned to the CPU at its position in the mask its thread inherits, and an explicit CPU outside that mask is left out: a placement never leaves the CPUs the process was given — `qb::detail::place_thread` (`src/qb/system/cpu.h:271`, Huly QB-1081).
- Shutdown has three triggers wired to the same plumbing (`src/qb/core/VirtualCore.cpp:1003-1004`, `:1681-1714`; `Main::stop()` at `src/qb/core/Main.cpp:901-908`): a POSIX signal (`SIGINT` / `SIGTERM` via `sigaction`), `Main::stop()` raising SIGINT's generation (one `std::atomic<unsigned int>` per signal number, plus the global generation the workflow polls), and the C++20 `qb::stop_source` (`request_stop()` on `~Main`, which then advances the same generation -- the pass reads that one value and never polls the token). A worker that observes any of them broadcasts one `SignalEvent` per changed signum in each scan -- repeated raises of that signum coalesce, and cooperative stop adds a virtual `SIGINT` -- so a later different signal never hides an earlier one, and existing shutdown handlers keep working on platforms with or without POSIX signals.

## Memory ordering cheat-sheet

| Data | Access pattern | Why it is safe |
|---|---|---|
| `Actor::_alive` | plain read/write | single-thread owner (`src/qb/core/Actor.h:345`) |
| `Actor` member fields | plain read/write | same |
| `VirtualCore::_actors` / `_actor_callbacks` (actor maps / callback list) | plain read/write | mutated only on the owning worker thread (`src/qb/core/VirtualCore.h:397-401`) |
| `Main` signal generations | `std::atomic<unsigned int>` per signal number, plus one global | async-signal-safe; the worker loop polls the global one |
| `Main::_stop_source` / `_stop_token` | `qb::stop_*` | standard library ordering when backed by `std::stop_*`; acquire/release atomic ordering in qb's fallback |
| Service-id registration | `ServiceIndex` once initialised, else a per-tag atomic slot (release/acquire) + registry mutex | one-time cross-thread publish; refused while a core is alive (`src/qb/core/VirtualCore.h:1540-1550`) |
| Inter-core mailbox | lock-free MPSC ring buffer | own internal acquire/release (see [concurrency primitives](../0_foundations/concurrency_primitives.md)) |

There is no `std::mutex` on the message path. The only locks in `qb-core` guard the one-time service-id map (static-init only) and the mailbox condition variable used when a core parks while truly idle at non-zero latency.

## Pitfalls

- **Touching another actor's state directly.** Calling a public method via `handle->` bypasses the mailbox and the actor model's guarantees. It is safe only when `handle.ready()` (the actor is active); on a non-ready handle `get()` returns `nullptr`, while `operator->` asserts in debug (`Actor.h:2568-2573`). Prefer sending an event to `handle.id()`.
- **Reading `_alive` from outside the owning thread.** It is not atomic; a cross-thread read is a data race (`src/qb/core/Actor.h:345`). Use `qb::ActorHandle<T>` (alias `RefActorHandle<T>`).
- **Capturing `this` (or any member by reference) into a `spawn_detached` coroutine.** The actor can die while the coroutine is suspended (`src/qb/core/Actor.h:1450-1456`). Capture by value; reach the actor only through `CoroContext`.
- **Blocking the worker thread.** `std::this_thread::sleep_for`, a heavy syscall, or a long computation in a handler or `on(qb::LoopEvent const&)` stalls every actor on the core (`src/qb/core/ICallback.h:16-19`). Offload to a coroutine or a dedicated I/O actor.
- **Letting an event constructor throw under load.** The `noexcept` message path terminates the process on a thrown exception (`src/qb/core/Pipe.h:148-160`). Keep events small and allocation-light.
- **Using `send()` for ordered work.** `send()` is unordered (`src/qb/core/VirtualCore.h:1271-1298`). The EventQOS0 path requires trivial destructibility at compile time (`src/qb/core/VirtualCore.h:1256-1259`). When ordering matters, use `push()`.
- **Persisting `type_id<T>()`.** It is process-local and not stable across runs.

## See also

- [Threading model](../2_core_concepts/threading_model.md) — the one-core-one-thread design in narrative form.
- [Event system](../2_core_concepts/event_system.md) — event types, QoS levels, and routing.
- [qb-io invariants](./io_invariants.md) — the matching contract for the async I/O layer and coroutine scheduler.
- [API overview](./api_overview.md) · [FAQ](./faq.md) · [Glossary](./glossary.md)
