# Event messaging between actors

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

A deep dive into the qb messaging layer: how `push`, `send`, `reply`, `forward`, and `broadcast` differ in delivery semantics, what ordering the runtime guarantees, and how events move through the per-actor pipe and the per-core mailbox both within a core and across cores.

**Prerequisites:** [Writing actors with qb::Actor](./actor.md), [The event system](../2_core_concepts/event_system.md) — **See also:** [The engine: Main and VirtualCore](./engine.md), [Actor patterns](./patterns.md), [Concurrency model](../2_core_concepts/concurrency.md)

## Summary

Actors in qb communicate only by exchanging typed events. An event is a C++ object that derives from `qb::Event`; the runtime copies it into a buffer, routes it to the destination actor's owning `VirtualCore`, and invokes that actor's handler for the event's exact type. This page covers the send side in detail — the five primitives an actor uses, their ordering and lifetime contracts, and the two-stage transport (source-side *pipe*, destination-side *mailbox*) that carries an event from one core to another.

The event *type* model — how to define events, when to use `qb::string<N>` versus `std::string`, and how handlers are registered and dispatched — is owned by [The event system](../2_core_concepts/event_system.md). This page does not repeat those definitions; it focuses on delivery.

Every signature here is grounded in three headers: `qb/core/Event.h` (the event base class), `qb/core/Actor.h` (the send API), and `qb/core/Pipe.h` (the low-level channel). Routing internals come from `qb/core/VirtualCore.tpp` and `qb/core/Main.h`.

## Concepts

### The transport: pipe out, mailbox in

Every event crosses two structures on its way from sender to receiver.

- **The pipe (`qb::Pipe`, wrapping a `qb::VirtualPipe`)** is the source side. A `VirtualPipe` is `qb::allocator::pipe<EventBucket>` (`qb/core/Event.h`) — a growable buffer owned by the *sending* `VirtualCore`, keyed by the destination core. The sender constructs the event in place inside this buffer.
- **The mailbox** is the destination side. The engine holds one `SharedCoreCommunication::Mailbox` per core (in `SharedCoreCommunication::_mail_boxes`, reached via `getMailBox(CoreId)`, `qb/core/Main.h`). A `Mailbox` derives from `qb::lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>` — a multi-producer, single-consumer lock-free ring. Every other core enqueues into a core's mailbox; only that core dequeues from it.

A `VirtualPipe` and the mailbox both store **`EventBucket`** slots. An `EventBucket` is one cache line wide — `QB_LOCKFREE_EVENT_BUCKET_BYTES`, which equals `QB_LOCKFREE_CACHELINE_BYTES` (`qb/utility/prefix.h`), 64 bytes on common targets. An event occupies a whole number of contiguous buckets, recorded in the 16-bit `bucket_size` field of its header.

The mailbox ring holds `MaxRingEvents = std::numeric_limits<uint16_t>::max() / QB_LOCKFREE_EVENT_BUCKET_BYTES` buckets — roughly **1023 buckets (≈64 KiB)** with the default 64-byte bucket (`qb/core/Main.h`).

### Two delivery paths: same core and cross core

The destination actor's `ActorId` carries the `CoreId` of the core that owns it. Routing branches on whether that core is the sender's own core.

- **Same core.** The event is appended to the local pipe and consumed during the same core's event-loop iteration, after the current handler returns. No mailbox, no inter-thread synchronization.
- **Cross core.** The event is published into the destination core's MPSC mailbox. The destination core dequeues it on its next loop iteration. This is the only path that touches the lock-free ring.

`send()` and `push()` resolve this branch differently — see [Ordering guarantees](#ordering-guarantees) below.

### Mailbox latency and wake-up

A mailbox is constructed with a `qb::duration` latency (`qb/core/Main.h`). It governs how the consuming core waits when its mailbox is empty.

- **Latency `0` (default, busy-spin).** `Mailbox::wait()` returns immediately; the core stays on the lock-free fast path and polls. Lowest latency, one core fully occupied.
- **Latency `> 0`.** `Mailbox::wait()` parks on a `std::condition_variable` for up to the configured span. A producer that enqueues an event calls `Mailbox::notify()`, which signals the condition variable and wakes the consumer. Lower CPU use at idle, at the cost of up to one latency span of wake-up delay.

Latency is set per core through `qb::CoreInitializer::setLatency` or globally through `qb::Main::setLatency`; see [The engine](./engine.md).

## The five send primitives

All send methods are inherited from `qb::Actor` and are callable from inside any handler, `onInit`, or `on(qb::LoopEvent const&)`. Every one is `const noexcept`.

> **`noexcept` is load-bearing.** `push`, `send`, `broadcast`, `reply`, and `forward` are all `noexcept`, yet they grow the pipe buffer (which can throw `std::bad_alloc`) and run the event constructor in place (which can throw). A throw cannot cross a `noexcept` boundary, so any such failure calls `std::terminate()` and aborts the process — it is not recoverable. This is by design: events are expected to be small, allocation-light messages on an adequately provisioned system. Keep event constructors cheap, and move large heap data in through an already-allocated `std::shared_ptr` rather than allocating inside the constructor. (`qb/core/Pipe.h`, `qb/core/Actor.h`.)

At a glance, the five primitives differ along five axes:

| Primitive | Ordering | Event object | Trivially-destructible required? | Handler must take | Destination |
|---|---|---|---|---|---|
| `push` | FIFO per source→dest | new, at pipe **back** | no — dtor runs on the receiving side | — | one `ActorId` (or `BroadcastId(core)`) |
| `send` | **none** | new, at pipe **front** | **yes for `EventQOS0`** — the engine may DROP those on backpressure without destroying them | — | one `ActorId` |
| `reply` | n/a (redirects one event) | **reuses** the received event | n/a | `on(E&)` non-const | back to `event.source` |
| `forward` | n/a (redirects one event) | **reuses** the received event | n/a | `on(E&)` non-const | new `ActorId`; `source` preserved |
| `broadcast` | FIFO per recipient, no global order | new per core (fans out via `send`) | **yes for `EventQOS0`** — same drop risk per remote core | — | every actor on every core |

The subsections below detail each. When unsure, the answer is `push`.

### `push` — ordered, the default

```cpp
template <typename _Event, typename... _Args>
_Event &push(ActorId const &dest, _Args &&...args) const noexcept;
```
<!-- src: qb/include/qb/core/Actor.h -->

`push` is the primary and recommended primitive. It constructs `_Event` in place at the **back** of the destination pipe (`allocate_back`, `qb/core/VirtualCore.tpp`) and returns a mutable reference to it, so the sender can finish populating the event after construction:

```cpp
// derived from: qb/source/core/tests/system/messaging/messaging-api.cpp (BasicPushActor)
#include <qb/actor.h>
#include <qb/event.h>

struct UpdateConfig : qb::Event {
    qb::string<64>  key;
    qb::string<256> value;
    int             priority;
    UpdateConfig(const char *k, const char *v, int p)
        : key(k), value(v), priority(p) {}
};

void Producer::on(const TickEvent &) {
    auto &cmd = push<UpdateConfig>(_target, "timeout_ms", "500", 1);
    cmd.priority = 2; // valid: the event has not been sent yet
}
```

`push` accepts events with non-trivial members and destructors (`std::vector`, smart pointers, `qb::string<N>`). The framework runs the event's destructor on the receiving side after the handler returns.

> **Members must be trivially relocatable — no pointer into themselves.** The runtime moves an event by raw `memcpy`: it copies the bytes to a new address and abandons the source without running a destructor there. A member that points at its own storage therefore keeps addressing the *old* bytes after the move. A **short `std::string` by value** is exactly that on libstdc++, whose small-string buffer is referenced by an internal pointer: the handler reads reused memory and `~basic_string()` frees an address that never came from the heap. libc++ recomputes `data()` from `this`, so the defect is invisible on macOS and corrupts only on Linux. Use `qb::string<N>` for inline text, or hold the data on the heap behind a `std::shared_ptr` / `std::unique_ptr`. `std::vector`, smart pointers and long (heap-backed) strings are all safe.
>
> **This is not a cross-core-only rule.** An event is relocated at three distinct points, and only the last one crosses a core boundary:
>
> 1. **Pipe growth and compaction.** The source pipe is a growable buffer: when `allocate_back` outgrows the current capacity it `memcpy`s everything already queued into the new allocation, and `reorder()` can compact a pipe in place with `memmove` once its freed prefix has grown past half the capacity (`qb/system/allocator/pipe.h`). Both move events that are still waiting to be drained — including events whose destination is on the **sending** core, since `push` places those in a pipe too and the core drains that pipe on its next `__receive__` (`qb/source/core/src/VirtualCore.cpp`).
> 2. **`reply` and `forward`.** Both byte-recycle the received event into a pipe with `memcpy` (`VirtualPipe::recycle`, `qb/system/allocator/pipe.h`), same core or not.
> 3. **The cross-core hop.** Sender pipe → peer mailbox ring → receive buffer: two more `memcpy`s, after which the event is destroyed at an address it was never constructed at.
>
> A same-core `push` is therefore *less likely* to expose the bug — the pipe starts at 4096 buckets (256 KiB at the default bucket size) and relocates only when it must grow — but it is not exempt. Design the event type to be relocatable; do not reason about which core the destination happens to be on.
>
> Debug builds help, but only partly. Before handing an event to a peer's mailbox ring the engine scans it for a pointer into its own storage and aborts with a diagnostic rather than corrupting the receiver (`SharedCoreCommunication::send`, `qb/source/core/src/Main.cpp`). Two gaps follow from where that check sits: it **never runs for same-core delivery** (which does not go through the mailbox layer at all), and it looks for a word addressing the event's *current* bytes, so a self-pointer that an earlier pipe growth already left dangling now points at the old buffer and falls outside the scanned range. The scan is compiled out under `NDEBUG`, so release pays nothing. Treat a clean debug run as evidence, not proof.

> Do not retain the returned reference past the current scope. The event's lifetime is owned by the framework once control leaves the handler.

### `send` — unordered, trivially destructible only

```cpp
template <typename _Event, typename... _Args>
void send(ActorId const &dest, _Args &&...args) const noexcept;
```
<!-- src: qb/include/qb/core/Actor.h -->

`send` does **not** guarantee ordering relative to other sends or pushes from the same source to the same destination. It constructs the event at the **front** of the pipe (`allocate`, `qb/core/VirtualCore.tpp`); for a cross-core destination it attempts to publish the event into the destination mailbox immediately and, on success, frees the pipe slot with `pipe.free(...)` (`qb/core/VirtualCore.tpp`). That early-publish path is what breaks the FIFO ordering that `push` preserves.

`_Event` **must be trivially destructible**. This is a hard contract, but the enforcement is not uniform: `fill_event` `static_assert`s `std::is_trivially_destructible_v<T>` only for events deriving from `EventQOS0` (`if constexpr (event_qos0_type<T>)`, `qb/core/VirtualCore.tpp`). A plain `qb::Event`-derived type with a `std::vector` or smart-pointer member therefore *compiles* through `send`, and — contrary to a long-standing note here — **does not leak**: on the cross-core publish path the pipe slot is reclaimed by pointer arithmetic without running the destructor (`pipe.free` advances `_begin`/`_end` only, `qb/system/allocator/pipe.h`), but the bytes were already relocated into the destination ring and the *receiver* destroys them exactly once. `SendNonTrivialPayload.{SameCore,CrossCore}DestroysEveryPayloadExactlyOnce` pins a zero live-object balance on every placement path.

What the requirement really protects is the **drop** path: a `qb::EventQOS0` event is best-effort, so `__flush_all__` discards it on backpressure *without* disposing it — hence the `static_assert`, and hence the rule that fire-and-forget events derive from `qb::EventQOS0` and stay trivially destructible. Separately, and for `push` as much as `send`, every event payload must be trivially **relocatable** (no pointer into itself) whatever core it is bound for — see the note under [`push`](#push--ordered-the-default).

```cpp
// derived from: qb/source/core/tests/system/messaging/messaging-api.cpp (BasicSendActor)
#include <qb/actor.h>
#include <qb/event.h>

// Derive from EventQOS0 so the trivially-destructible requirement is enforced
// at compile time (see note above). A plain qb::Event would compile silently.
struct FireForgetSignal : qb::EventQOS0 {};

void Monitor::ping(qb::ActorId monitor_id) {
    send<FireForgetSignal>(monitor_id);
}
```

Prefer `push` unless you have measured a need and ordering genuinely does not matter. Misuse is a frequent source of order-dependent bugs.

### `reply` — return an event to its sender

```cpp
void reply(Event &event) const noexcept;
```
<!-- src: qb/include/qb/core/Actor.h -->

`reply` reuses the received event object instead of allocating a new one. The runtime swaps the event's `dest` and `source`, re-marks it alive, and sends it back (`std::swap(event.dest, event.source)`, `qb/source/core/src/VirtualCore.cpp`). The handler must therefore take its event **by non-const reference**, because the object is mutated in place:

```cpp
// derived from: qb/source/core/tests/system/messaging/messaging-reply-forward.cpp (reply handler)
void Responder::on(MyRequest &request) {     // non-const reference
    request.response = compute(request.query);
    reply(request);                          // sent back to request's source
    // request is consumed here; do not touch it again
}
```

A broadcast event cannot be replied to: if `event.dest.is_broadcast()` is true, `reply` logs a warning and drops the call (`qb/source/core/src/Actor.cpp`). After `reply` returns, the event is consumed — do not read or modify it.

### `forward` — redirect an event, preserving its origin

```cpp
void forward(ActorId dest, Event &event) const noexcept;
```
<!-- src: qb/include/qb/core/Actor.h -->

`forward` re-routes a received event to a new destination without allocating. It overwrites `event.dest` with the new target but **deliberately leaves `event.source` untouched**, so the original sender remains the logical origin and a downstream `reply` returns to the true client rather than to the forwarding actor (`qb/source/core/src/Actor.cpp`, `qb/source/core/src/VirtualCore.cpp`). As with `reply`, the handler must take a non-const reference, broadcast events cannot be forwarded, and the event is consumed after the call.

```cpp
// derived from: qb/source/core/tests/system/messaging/messaging-reply-forward.cpp (forward handler)
void Router::on(WorkItem &item) {            // non-const reference
    forward(pick_worker(item), item);        // worker sees the original source
}
```

The distinction in one line: `reply` swaps `dest` and `source`; `forward` sets a new `dest` and keeps `source`.

### `broadcast` — fan out to every actor

```cpp
template <typename _Event, typename... _Args>
void broadcast(_Args &&...args) const noexcept;
```
<!-- src: qb/include/qb/core/Actor.h -->

`broadcast` delivers a copy of the event to every actor on every active core. Internally it iterates the engine's core set and issues one `send` per core with a `BroadcastId` destination (`qb/core/VirtualCore.tpp`):

```cpp
broadcast<SystemShutdownNotice>();
```

> Because `broadcast` fans out via `send`, it inherits `send`'s contract on every remote core: the event **should be trivially destructible**, because a `qb::EventQOS0` broadcast is best-effort and may be dropped on backpressure without being disposed. A non-trivially-destructible payload is not leaked on the ordinary delivery path — the receiver destroys it — but it is on a drop, and there is no compile-time guard on `broadcast` (the `fill_event` `static_assert` fires only for `EventQOS0`-derived events). Broadcast a trivially-destructible event (ideally derive from `qb::EventQOS0`) and keep bulk data behind a `std::shared_ptr`.

To target a single core instead of the whole system, push to a `qb::BroadcastId`:

```cpp
// Deliver to every actor on core 1, ordered relative to this sender's pushes.
push<CacheFlushCommand>(qb::BroadcastId(1), "users_cache");
```

`qb::BroadcastId(core_id)` is an `ActorId` whose service id is the reserved `BroadcastSid` (`qb/core/ActorId.h`). An actor receiving a broadcast cannot `reply` to it or `forward` it, since the destination id is a broadcast id.

## Ordering guarantees

The runtime makes one ordering promise, and it is narrow but precise:

> Events delivered with `push` (or `Pipe::push` / `to(dest).push`) from a **single source** actor to a **single destination** actor are processed by the destination in the exact order they were pushed. (`qb/core/Pipe.h`, `qb/core/Actor.h`.)

What this promise does *not* cover:

- **No cross-source ordering.** If actors A and B both push to C, the interleaving of A's and B's events at C is unspecified. Each source's own subsequence is preserved; the merge is not.
- **No cross-destination ordering.** Two pushes from A to different destinations have no ordering relationship.
- **`send` is unordered, period.** It carries no ordering guarantee relative to any other `send` or `push`, even same-source same-destination, because of the early cross-core publish path described above.
- **`broadcast` fans out as independent sends**, so per-recipient ordering follows the same single-source rule, but there is no global ordering across recipients.

Underlying these rules: `push` appends to the back of the per-source-core pipe and the pipe drains in FIFO order, so a single source draining into a single destination preserves insertion order. Because each `VirtualCore` is strictly single-threaded and an actor never migrates cores, a handler never runs concurrently with another handler on the same actor — ordering is observed exactly as the receiving core dequeues.

## Bulk and large-payload sending

For sending several events to one destination, or for events with large dynamic payloads, the framework exposes the underlying channel directly.

### `to(dest)` — fluent chained pushes

```cpp
[[nodiscard]] EventBuilder to(ActorId dest) const noexcept;
```
<!-- src: qb/include/qb/core/Actor.h -->

`to(dest)` returns an `Actor::EventBuilder` bound to the destination's pipe; each `EventBuilder::push` forwards to `Pipe::push` and returns the builder for chaining (`qb/core/Actor.tpp`). The pipe is resolved once, so repeated sends to the same destination skip the per-call lookup. Ordering matches plain `push`.

```cpp
// derived from: qb/source/core/tests/system/messaging/messaging-api.cpp (EventBuilderPushActor)
to(stats_id)
    .push<CounterIncrement>("login_attempts")
    .push<TimerStart>("session");
```

### `getPipe(dest)` and `allocated_push`

```cpp
[[nodiscard]] Pipe getPipe(ActorId dest) const noexcept;
```
<!-- src: qb/include/qb/core/Actor.h -->

`getPipe` hands back the `qb::Pipe` to a destination. `Pipe` exposes `push` (identical semantics to `Actor::push`) and `allocated_push`, which takes a byte-size hint so the framework can reserve the right amount of pipe buffer up front and avoid reallocation while constructing a large event:

```cpp
// src: qb/include/qb/core/Pipe.h
template <typename _Event, typename... _Args>
[[nodiscard]] _Event &allocated_push(std::size_t size, _Args &&...args) const noexcept;
```

```cpp
// derived from: qb/source/core/tests/system/messaging/messaging-api.cpp (AllocatedPipePushActor); pattern from qb/include/qb/core/Pipe.h
qb::Pipe pipe = getPipe(processor_id);
auto blob = std::make_shared<std::vector<std::byte>>(1024 * 1024); // 1 MB, on the heap
// ... fill blob ...
// The blob is owned by the shared_ptr, so the event's in-pipe footprint is just the
// event object: NO trailing bytes are needed. Passing blob->size() here would reserve
// 1 MB of pipe space and push the event past the cross-core ceiling — see the note below.
auto &ev = pipe.allocated_push<BlobEvent>(0, blob);
```

The `size` argument is the **extra payload bytes beyond the event itself** — `allocated_push` adds `sizeof(_Event)` to it internally (`size += sizeof(T)`, `qb/core/Pipe.tpp`), it does not clamp `size` up to `sizeof(_Event)`. A hint of `0` still allocates at least one event's worth (the event always fits), and `0` is the right answer whenever the bulk data lives on the heap behind a pointer member.

Pass a non-zero `size` only when you deliberately write raw bytes into the region **immediately following** the event object — the way `AllocatedPipePushActor` in `messaging-api.cpp` does with `allocated_push<TestEvent>(32)` plus a 32-byte tail. Never pass `sizeof(BlobEvent) + blob->size()`: that double-counts one event's worth of space on top of a payload that is already too large.

> **Size the event small, not the payload.** The hint helps the *source pipe* reserve space, but the real ceiling is the destination mailbox. An event's total in-pipe footprint must fit the mailbox ring — about 1023 buckets (≈64 KiB) with the default bucket size. A larger event can never be enqueued into a peer's mailbox, however much that peer drains, so `__flush_all__` treats it as permanently unsendable rather than backpressured: it logs at `LOG_CRIT`, disposes the event (its destructor *does* run) and drops it, then keeps flushing the rest of the pipe (`qb/source/core/src/VirtualCore.cpp`). The message is lost, and nothing at the call site says so. Put bulk data on the heap behind a `std::shared_ptr` member and keep the event struct itself small; do not size `allocated_push` to the payload bytes. (`qb/core/Pipe.h`.)

## Pitfalls

- **`send` ordering and lifetime.** `send` does not preserve order, even same-source to same-destination. Reach for it only when ordering is irrelevant *and* the event is trivially destructible. The compile-time check fires only for `EventQOS0`-derived events, so derive fire-and-forget events from `qb::EventQOS0` — those are the ones the engine may drop on backpressure without disposing. A non-trivial plain-`Event` type compiles and is destroyed correctly by the receiver, but it forfeits the drop guarantee. The default answer is `push`.
- **Non-const handler for `reply`/`forward`.** Both reuse the event object in place, so the handler must declare `void on(MyEvent &event)`. A `const` reference will not compile against `reply(event)` / `forward(dest, event)`.
- **Using an event after `reply`/`forward`.** The event is consumed once handed back to the runtime. Reading or mutating it afterward is a use-after-consume bug.
- **Replying to or forwarding a broadcast.** Both are silently dropped (logged at warn) when the destination is a broadcast id. Design request/response flows around unicast events.
- **Oversized events are dropped cross-core.** An event whose in-pipe footprint exceeds the mailbox ring capacity can never be enqueued into a peer's mailbox. The flush recognises that as permanent, not as backpressure: it logs at `LOG_CRIT`, disposes the event and moves on, so the message is silently lost while the rest of the pipe keeps flowing. Keep events small; move bulk data to the heap.
- **Allocation in an event constructor under OOM aborts the process.** Because the send path is `noexcept`, a throwing event constructor or a failed buffer growth calls `std::terminate()`. Construct events from already-allocated resources.
- **Returned references are scope-bound.** The reference from `push` / `allocated_push` is valid only until control leaves the current handler. Do not store it.

## See also

- [The event system](../2_core_concepts/event_system.md) — defining events, `qb::string<N>` versus `std::string`, handler registration and dispatch.
- [Writing actors with qb::Actor](./actor.md) — the full `Actor` API, lifecycle, and services.
- [The engine: Main and VirtualCore](./engine.md) — cores, mailboxes, latency configuration, and scheduling.
- [Actor patterns](./patterns.md) — request/response, routing, and fan-out built on these primitives.
- [Concurrency model](../2_core_concepts/concurrency.md) — the single-threaded-per-core execution guarantees behind the ordering rules.
