@page reference_core_invariants QB-Core: Thread-Safety & Lifecycle Invariants
@brief Consolidated reference of the thread-ownership, memory-ordering and lifecycle invariants upheld by `qb-core`. Read this once before you write actor, coroutine or event-router code.

# QB-Core — Thread-Safety & Lifecycle Invariants

This page is the **single source of truth** for the invariants the runtime assumes. Every header and `.cpp` in `qb/include/qb/core/**` + `qb/source/core/src/**` is written with these rules in mind. If a change breaks an invariant, the whole actor model breaks silently.

The rules are grouped by subsystem. Each one links back to the finding number in `qb/QB_CORE_PLAN.md` where the invariant was formalised or hardened.

---

## 1. Thread model — one VirtualCore, one worker thread

- Every `qb::VirtualCore` runs on **exactly one** `std::jthread` for the entire life of the engine (`qb::Main::start()` → `~Main()`).
- The worker is identified by a `thread_local` pointer: `VirtualCore::_handler`. This pointer is installed by `Main::start_thread` before any actor is instantiated on that worker, and it is never reassigned for the lifetime of the worker.
- **Actors never migrate** between cores. An actor created on core `N` lives, receives events, and is destroyed on core `N`. Its `this` pointer is therefore only dereferenceable on that one thread.
- Any API that synthesises "`thread-free`" behaviour (e.g. `qb::Pipe`, `to()`, `push<>()`, `broadcast<>()`) is implemented in terms of enqueuing events into the destination core's mailbox; the destination core's worker then dequeues and dispatches **on its own thread**.

Consequence: almost nothing in qb-core needs to be atomic. The few atomics that exist are there for **cross-thread init** (2.3 — `_nb_service`, `detail::_type_id_counter`) or **engine-wide shutdown** (2.17 — `std::stop_source` / `std::stop_token`).

---

## 2. Actor lifecycle

### 2.1 Construction

- `Actor::Actor()` / `Actor::Actor(ActorId)` / `Actor::Actor(qb::no_default_events_t)` all assert `VirtualCore::_handler != nullptr` (finding 2.8). Instantiating an actor outside a worker thread is a programming error and will trip an assertion in debug builds.
- `TActorFactory<_Actor>::create_impl` and `VirtualCore::addReferencedActor` both route through the `qb::allocate_actor<_Actor>(args...)` customization point (finding 2.13). Users who want PMR / pool-backed allocation specialise that template for their actor type.
- Default event registrations (`KillEvent`, `SignalEvent`, etc.) can be opted out by passing `qb::no_default_events` to the ctor (finding 2.16) — use this for ephemeral, pool-reused actors.

### 2.2 Init

- `Actor::onInit()` is invoked exactly once by the owning `VirtualCore`, on that core's worker thread, before the actor enters the event loop. Returning `false` cancels the registration; the actor is destroyed immediately.

### 2.3 Steady state

- Event handlers, `onCallback()`, `onInit()`, `onTerminate()`, and all coroutines awaited inside an actor **execute on the owning core's worker thread**. You can freely access `this` members without synchronisation.
- `Actor::_alive` is a plain `bool` (not atomic) intentionally: reads and writes are strictly single-threaded on the owning core (finding 2.9). External code must **not** observe `_alive` directly — it MUST go through `qb::RefActorHandle<T>::get()` (finding 2.9), which resolves liveness through `VirtualCore::findActor<T>()` on the owning core.

### 2.4 Destruction

- `Actor::kill()` flips `_alive = false` on the owning core. The actual destruction happens at the **end of the current workflow iteration**, in `VirtualCore::removeActor()`.
- An actor with in-flight coroutines (`active_coroutine_count() > 0`) will log a warning when destroyed: coroutines must never capture raw actor pointers (use `qb::RefActorHandle<T>` or coroutine scopes that propagate cancellation — finding 2.12 / coroutine skill).
- Service actors reserve their `ServiceId` for the entire life of the process. Only non-service ids are recycled into `VirtualCore::_ids` (finding 2.14 / 2.3).

---

## 3. Event system

### 3.1 Identity

- Each event / service type has a dense, collision-free 16-bit `TypeId` assigned at first use through a magic-static counter in `qb::detail::type_id_for<T>()` (finding 2.1). The counter is atomic so concurrent first-instantiations from different TUs are race-free.
- The legacy address-narrowing path has been removed. Do not write code that assumes `type_id<T>()` is `constexpr` or derivable from an address.
- `type_id<T>()` is **stable within a process run** but is not guaranteed to be stable **across runs** — do not persist it.

### 3.2 Push vs send

- `push<Event>()` uses `allocate_back` on the destination pipe → FIFO, ordered between the same (source, dest) pair, works with any `Event` type.
- `send<Event>()` uses `allocate` on the destination pipe → may reuse holes, faster, but ordering is not preserved. Requires `event_qos0_type` (trivially destructible events).
- `broadcast<Event>()` fan-outs to every active core; each core processes the event independently on its worker thread.

### 3.3 Event bucket layout

- An `Event` is cacheline-aligned (`QB_LOCKFREE_CACHELINE_ALIGNMENT`) and crafted so its metadata (`state`, `bucket_size`, `id`, `dest`, `source`) fits in a single cacheline. Keep `EventId` at 16 bits to preserve that property (finding 2.1 — acknowledged trade-off).
- `base_pipe::allocate_back` relocates existing buckets via `memcpy` when the pipe grows. **Events MUST be trivially relocatable**: this is a documented precondition. Concretely: no self-pointers, no members that register themselves with an external registry in their ctor/dtor. Most payloads satisfy this (PODs, `std::string` with SSO, `std::unique_ptr`, etc.). If you have an event with a self-pointer, redesign it to hold the indirection elsewhere (finding 2.2 — kept by design).

### 3.4 Deadlock recovery between cores

`VirtualCore::__flush_all__` drains outbound pipes to peers via their mailboxes. If a peer's mailbox is full:

- **Non-QoS events** (`qos == 0`): single `try_send` attempt, dropped on failure (best-effort semantics).
- **QoS events**: bounded three-stage backoff (finding 2.4):
  - `[0, 64)` attempts → `qb::spin_loop_pause()` hint (CPU-friendly, no scheduler involvement)
  - `[64, 512)` attempts → `std::this_thread::yield()` (peer consumer gets a slot)
  - `>= 512` → partial flush (keep the unsent tail in the pipe), `mailbox.notify()` the peer, return to workflow.

**Termination is guaranteed in bounded time**: after a partial flush, the workflow calls `__receive__`, which drains the local mailbox → frees space for peers → their next `try_send` succeeds. Formally no deadlock, no starvation.

---

## 4. Memory ordering cheat-sheet

| Data                                   | Access pattern             | Why safe                                                                              |
|----------------------------------------|----------------------------|---------------------------------------------------------------------------------------|
| `Actor::_alive`                        | plain read/write           | Single-thread owner (§ 2.3).                                                          |
| `Actor` member fields                  | plain read/write           | Idem.                                                                                 |
| `VirtualCore::_actors` / `_callback_list` | plain read/write        | Only mutated on the owning worker thread.                                             |
| `Main::_signal_pending`                | `volatile std::sig_atomic_t` | Async-signal-safe read in the worker loop.                                           |
| `Main::_stop_source` / `_stop_token`   | `std::stop_*`              | Standard library provides the ordering (`request_stop` acts as release / poll as acquire). |
| `VirtualCore::_nb_service`             | `atomic<ServiceId>` relaxed | Published through magic-static acquire edge (§ 2 / finding 2.3).                     |
| `detail::_type_id_counter`             | `atomic<TypeId>` relaxed   | Idem (finding 2.1).                                                                   |
| MPSC mailbox (`SharedCoreCommunication::Mailbox`) | lock-free ringbuffer | Own internal acquire/release (see `lockfree/mpsc/ringbuffer.h`).                      |

No `std::mutex` on the hot path. The only mutexes in qb-core protect:
- The `servicesMutex()` (mutation of the `type_id<Tag>() → ServiceId` map — rare, only at static init).
- `SharedCoreCommunication::Mailbox::_mtx` (condition variable parking when latency > 0 — only when truly idle).

---

## 5. Coroutines & async I/O

- `Actor::spawn_async` eagerly allocates a single `shared_ptr<atomic<size_t>>` active-coroutine counter in the `Actor` ctor (finding 2.12). Every `spawn_async` call is a single `fetch_add` + spawn on the hot path.
- Coroutine bodies run in the same event loop as the actor (`qb::io::async::listener::current`). You can freely `co_await` on `qb::io::async::*` awaitables; on resumption, you are back on the owning worker thread.
- **Do not** capture a raw actor pointer into a coroutine unless you can prove the coroutine completes before the actor dies. Use `qb::RefActorHandle<T>` or a `qb::coroutine::scope` with cancellation to propagate teardown.
- A dying actor with outstanding coroutines logs a warning; the coroutines are NOT forcibly cancelled — that is the caller's responsibility.

---

## 6. CPU affinity & shutdown

- `qb::CoreInitializer::setAffinity(CoreIdSet)` pins the VirtualCore's thread. Pass `qb::CoreIdSet{qb::NoAffinity}` (or any set with no real core id < `qb::MaxCores`) to explicitly opt out of pinning (finding 2.11). See `@ref NoAffinity`.
- `Main::stop()` is async-signal-safe: sets a `volatile sig_atomic_t` that `__workflow__` polls.
- `~Main()` additionally requests the `std::stop_source`, providing a signal-free shutdown path that works on platforms without POSIX signals (finding 2.17). Workers observe the token in `__workflow__` and synthesise a `SignalEvent` broadcast so existing shutdown handlers keep working.

---

## 7. Do-and-Don't summary

**Do**
- Keep all actor state accesses on the owning worker thread.
- Use `qb::RefActorHandle<T>` when referencing a same-core child actor.
- Use `allocated_push<Event>()` when you need fine-grained control of the bucket, otherwise `push<Event>()`.
- Write events that are trivially relocatable (no self-pointers).
- Opt out of default event registrations (`qb::no_default_events`) for short-lived pool-reused actors.

**Don't**
- Don't touch `Actor::_alive` from outside the owning thread.
- Don't persist `type_id<T>()` across runs.
- Don't block the worker thread (no `std::this_thread::sleep_for`, no heavy syscalls) — use a coroutine or a dedicated IO actor.
- Don't capture raw actor pointers into coroutines.
- Don't rely on any kind of lock-based synchronization to "share" actor state.

---

**(Cross-refs:** [API Overview](./api_overview.md) · [FAQ](./faq.md) · [QB_CORE_PLAN.md](../../QB_CORE_PLAN.md) — finding numbers referenced above.)**
