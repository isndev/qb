# Inter-actor messaging: the address is the route

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported) — 60487ee7

An `ActorId` is `{ServiceId, CoreId}` packed into 32 bits, and the core half is not metadata — it *is* the routing decision. Every rule on this page (`push` versus `send`, the lifetime of the reference `push` hands back, why a payload must be relocatable, why one event can be silently dropped) is a consequence of that one packing and of the buffer it selects.

**Prerequisites:** [Writing actors](./actor.md) · [The event system](../2_core_concepts/event_system.md) — **See also:** [The pipe: qb's one buffer](../0_foundations/buffers.md) · [The engine](./engine.md) · [Core invariants](../7_reference/core_invariants.md) · [Concurrency primitives](../0_foundations/concurrency_primitives.md)

## The address is the route

```cpp
class ActorId {
    ServiceId _service_id;
    CoreId    _core_id;
```
<!-- src: qb/src/qb/core/ActorId.h:383-395 -->

Two `uint16_t`s (`src/qb/core/ActorId.h:51`, `:59`), no padding, converted to and from a `uint32_t` with `std::bit_cast` in both directions (`src/qb/core/ActorId.cpp:40`, `:46`). `sid()` is the actor's slot inside its core; `index()` names the core. Three values are reserved: `NotFound == 0` is the default-constructed, invalid id, `BroadcastSid` (`ServiceId` max) marks a `qb::BroadcastId`, and `qb::MaxCores == 256` bounds the core half (`src/qb/core/ActorId.h:401-402`, `:80`).

Every send begins by reading the core half and ends by writing bytes into a buffer chosen by it:

```mermaid
flowchart LR
    A["push&lt;E&gt;(dest, …)"] --> B["dest._core_id"]
    B --> C["__getPipe__(core)<br/>_pipes[core_set.resolve(core)]<br/>one VirtualPipe per destination core"]
    C --> D{"same core?"}
    D -->|yes| E["stays in the pipe<br/>drained by this core's own __receive__"]
    D -->|no| F["__flush_all__ → SharedCoreCommunication::send<br/>MPSC ring enqueue into the peer's Mailbox"]
    F --> G["peer's __receive__ copies the buckets out<br/>and routes them to on(E&amp;)"]
```
<!-- src: qb/src/qb/core/VirtualCore.h:842 (dest._core_id selects the pipe), qb/src/qb/core/VirtualCore.cpp:135-138 (__getPipe__), qb/src/qb/core/Main.cpp:213-218 (the ring enqueue) -->

Nothing in that path looks an actor up by identity across a thread boundary. The sender resolves a **core**, appends bytes to a buffer it owns exclusively, and the destination core turns those bytes back into an event on its own thread. The one cross-thread structure is the mailbox ring, and it is reached only from the flush.

Both buffers store the same unit. A `qb::VirtualPipe` is `qb::allocator::pipe<EventBucket>` (`src/qb/core/Event.h:689`), and an `EventBucket` is one cache line wide — `QB_LOCKFREE_EVENT_BUCKET_BYTES`, which is `QB_LOCKFREE_CACHELINE_BYTES`, 64 bytes on common targets (`src/qb/utility/prefix.h:66-68`, `:138-140`). An event occupies a whole number of contiguous buckets and records that count in its own 16-bit header. The destination side is a `SharedCoreCommunication::Mailbox`, one per core, reached through `getMailBox(CoreId)`; it derives from `qb::lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>` — many producers, one consumer, no lock (`src/qb/core/Main.h:328`, `:382-383`, `:454`). Its idle-wait policy is a `qb::duration` set per core; [the engine page](./engine.md#latency-what-a-core-does-when-it-has-nothing-to-do) owns that.

That is also why the send API needs no handle to the runtime. `VirtualCore::_handler` is a `thread_local` pointer to "the core running on this thread" (`src/qb/core/VirtualCore.h:85`), and because actors are thread-affine it is always the right core — so `Actor::push` is one forward through it:

```cpp
template <typename _Event, typename... _Args>
_Event &
Actor::push(ActorId const &dest, _Args &&...args) const noexcept {
    return VirtualCore::_handler->template push<_Event>(dest, id(), std::forward<_Args>(args)...);
}
```
<!-- src: qb/src/qb/core/VirtualCore.h:970-974 -->

`Actor::send` is the same one-line shape (`src/qb/core/VirtualCore.h:976-980`), and so is `Actor::broadcast` (`src/qb/core/VirtualCore.h:1003-1007`). So are the non-template members: `getPipe` (`src/qb/core/Actor.cpp:215-218`), `reply` and `forward` (`src/qb/core/Actor.cpp:300-318`), `time` (`src/qb/core/Actor.cpp:195-198`). No lock, no atomic, no fence appears anywhere on that path.

### A `Pipe` is per destination **core**, not per destination actor

`getPipe(dest)` returns a `qb::Pipe`, which is a `VirtualPipe *` plus the two endpoint ids (`src/qb/core/Pipe.h:94-101`), and the `VirtualPipe` it points at is selected by `dest._core_id` alone:

```cpp
Pipe
VirtualCore::getProxyPipe(ActorId const dest, ActorId const source) noexcept {
    return {__getPipe__(dest._core_id), dest, source};
}
```
<!-- src: qb/src/qb/core/VirtualCore.cpp:961-964 -->

Two actors on the same core therefore share one outbound buffer, which is what makes the invalidation rule below say *core* rather than *actor*. What `getPipe`/`to` actually save is two array indexings, not a hash lookup: `_pipes` is a `PipeMap`, which is `std::vector<VirtualPipe>` (`src/qb/core/VirtualCore.h:160`, `:270`), indexed by `CoreSet::resolve`, which reads a `std::array<uint8_t, MaxCores>` (`src/qb/core/CoreSet.cpp:61`). The saving is real but small; reach for `to(dest)` for readability, not for throughput.

## One event, core A to core B

The journey of a single `push` across a core boundary, in the order it happens.

**On the sending core, inside the handler.**

1. `Actor::push<E>(dest, args...)` forwards to `VirtualCore::push<E>(dest, id(), args...)`.
2. `router::ensure_disposer<Event, E>()` registers a type-erased destructor for `E` if it is not trivially destructible. This runs at the *enqueue* funnel because that is the one place which statically knows the type; without it every drop path later would free bytes without running `~E()` (`src/qb/core/VirtualCore.h:841`; `src/qb/system/event/router.h:999-1006`).
3. `__getPipe__(dest._core_id)` selects the outbound buffer for the destination core.
4. `pipe.allocate_back(BUCKET_SIZE)` reserves `ceil(sizeof(E) / 64)` buckets at the tail. `BUCKET_SIZE` is `allocator::getItemSize<E, EventBucket>()` (`src/qb/system/allocator/pipe.h:40-42`).
5. `detail::prepare_event_storage` zeroes that whole bucket range **in debug builds only** — the relocation guard in step 9 scans it, and an event's range is never fully written by its payload (`src/qb/core/Event.h:280-285`).
6. The event is placement-newed into the reservation, then `fill_event` stamps `id`, `dest`, `source` and `bucket_size` into the header (`src/qb/core/VirtualCore.h:847-851`, `:786-804`).
7. `push` returns a reference into the pipe. It is live until the next allocation on that same pipe — see [below](#the-reference-push-returns-dies-at-the-next-push-to-that-core).

**Later in the same loop pass, in `__flush_all__`.**

8. The flush walks each non-empty outbound pipe as a byte stream, stepping `cur += event.bucket_size` (`src/qb/core/VirtualCore.cpp:297-307`).
9. `try_send` → `SharedCoreCommunication::send(_resolved_index, event)`. In a debug build this first scans the event's whole bucket range for a pointer-sized word addressing that same range and aborts if it finds one (`src/qb/core/Main.cpp:193-206`).
10. `enqueue(source_index, buckets, bucket_size)` copies the buckets into the destination mailbox's SPSC ring for *this* producer core, then `notify()` wakes a parked consumer (`src/qb/core/Main.cpp:215-217`). The producer slot is **this core's** resolved index, never `event.source` — `forward()` preserves the original sender, so deriving the slot from it would let two threads write one single-producer ring (`src/qb/core/Main.cpp:208-212`).
11. On success the pipe cursor advances past the event. Its destructor is **not** run here: the bytes now live in the ring, and the receiver owns them.

**On the destination core, on its next pass.**

12. `__receive__` calls `_mail_box.dequeue(fn, _event_buffer->data(), MaxRingEvents)`, which copies a contiguous batch out of the ring into the core's own `EventBuffer` (`src/qb/core/VirtualCore.cpp:224-226`).
13. `__receive_events__` walks that batch bucket-by-bucket, `reinterpret_cast`ing each offset to an `Event *` and trusting `bucket_size` to find the next one. A `bucket_size == 0` would make the walk stand still, so it is checked and the batch abandoned (`src/qb/core/VirtualCore.cpp:147-158`).
14. `event->state.bits.alive = 0`, then `_router.route(*event, onError)`.
15. The router resolves `event.getID()` to the per-type resolver, which looks the destination up in `_subscribed_handlers.find(event.getDestination())` (`src/qb/system/event/router.h:348-354`) and calls `dispatch_trampoline` — a per-handler-type static function that recasts a `void *` and calls `handler.on(event)` (`src/qb/system/event/router.h:280-290`).
16. After the handler returns, the same call disposes the event: `~E()` runs exactly once, on the receiving core, at an address the event was never constructed at (`src/qb/system/event/router.h:356-357`).

Two things in that sequence are worth pinning down, because they are where the surprises live: step 11 (nobody destroys the source copy) and step 16 (somebody destroys the *relocated* copy).

### `is_alive()` is checked at dispatch, not at enqueue

Nothing filters an event addressed to a dead actor on the way in. The actor stays in the router's handler map until `VirtualCore::removeActor` reaches `unregisterEvents(id)` at the end of the pass (`src/qb/core/VirtualCore.cpp:896-897`), so between `kill()` and the reap it is still a routing target. What stops it receiving is the trampoline:

```cpp
auto &handler = *static_cast<_Handler *>(opaque_handler);
if constexpr (qb::has_is_alive<_RawEvent>) {
    if (handler.is_alive())
        handler.on(event);
}
```
<!-- src: qb/src/qb/system/event/router.h:282-289 -->

So "events to a dead actor are dropped" is precise, and it is a *dispatch-time* check on the destination core. The event is still copied, still flushed, still routed, and still disposed — only the handler call is skipped.

### An event nobody subscribed to

If no actor on the destination core registered *that event type at all*, `memh::route` takes its `onError` branch. `VirtualCore` passes a lambda that logs a warning for a unicast destination and stays silent for a broadcast, since a broadcast reaching a core with no subscriber is normal (`src/qb/core/VirtualCore.cpp:201-210`). The router then disposes the event itself through the disposer registry that step 2 populated (`src/qb/system/event/router.h:880-902`). That is why `ensure_disposer` sits at the enqueue funnel and not at `subscribe`: a type that is pushed but subscribed nowhere would otherwise have no disposer, and every drop path would leak its heap members.

## The primitives at a glance

Every one of these is a member of `qb::Actor`, `const` and `noexcept`, and callable from any handler, from `onInit()`, and from an `on(qb::LoopEvent const&)` tick. Each sets this actor as the event's source.

| Primitive | Ordering | Event object | Trivially destructible? | Handler takes | Destination |
|---|---|---|---|---|---|
| `push<E>(dest, …)` | FIFO per source → dest | new, at the pipe **tail** | no — the receiver runs `~E()` | — | one `ActorId`, or `BroadcastId(core)` |
| `to(dest).push<E>(…)` | same as `push` | same | no | — | one `ActorId` |
| `getPipe(dest).allocated_push<E>(n, …)` | same as `push` | same, plus an `n`-byte tail | no | — | one `ActorId` |
| `send<E>(dest, …)` | **none** | new, at the pipe **front**, retracted on immediate delivery | **enforced for `EventQOS0`** — those may be dropped without disposal | — | one `ActorId` |
| `broadcast<E>(…)` | **none** (one `send` per core) | one per core | same as `send`, on every remote core | — | every actor on every core |
| `reply(event)` | none (goes through `send`) | **reuses** the received event | n/a | `on(E&)` non-const | back to `event.source` |
| `forward(dest, event)` | none (goes through `send`) | **reuses** the received event | n/a | `on(E&)` non-const | new `ActorId`, `source` preserved |

When in doubt the answer is `push`.

### `to(dest)` — chaining over one pipe

`to(dest)` returns an `Actor::EventBuilder`, which holds a `qb::Pipe` and whose `push` forwards to `Pipe::push` and returns the builder for chaining (`src/qb/core/Actor.h:492-530`; `src/qb/core/VirtualCore.h:1026-1031`):

```cpp
// src: derived from qb/tests/core/system/messaging/messaging-api.cpp (EventBuilderPushActor)
to(stats_id)
    .push<CounterIncrement>("login_attempts")
    .push<TimerStart>("session");
```

`EventBuilder::push` discards the event reference rather than returning it, which is exactly right for the chained form: each call is the "next event queued to that core" that would have killed the previous reference anyway.

## `push` and `send` are the same allocation, from two ends

The two primitives differ by one call: which end of the pipe they carve from. Everything else in their contract follows from that.

```cpp
// push
auto *const raw = pipe.allocate_back(BUCKET_SIZE);
```
<!-- src: qb/src/qb/core/VirtualCore.h:847 -->

```cpp
// send
auto *const raw = pipe.allocate(BUCKET_SIZE);
…
if (dest._core_id != _index && try_send(data))
    pipe.free(data.bucket_size);
```
<!-- src: qb/src/qb/core/VirtualCore.h:814-821 -->

`allocate_back` appends at `_end`: the event joins the FIFO stream and is delivered by the next flush, in order with everything already queued to that core. `allocate` tries the **front** first — the retired region below `_begin`, which the pipe already owns and no reader will walk — and only falls back to the tail if there is no room there ([the pipe's four operations](../0_foundations/buffers.md#the-four-operations)).

Then `send` does the thing `push` cannot: it attempts an immediate cross-core delivery and, on success, **retracts the allocation**. `pipe.free` consults the `_flag_front` the front-allocation set and moves the cursor back. The event never enters the outbound stream, so it can arrive *before* events queued earlier by `push`. That is the unordered contract, stated as a mechanism rather than as a rule.

Three consequences fall out of the retraction, and only the third is a compile error:

- **If the immediate attempt fails, or the destination is this same core, the retraction does not happen** and the event stays in the pipe to be flushed normally. `send` is not "no buffering"; it is "buffering it can undo".
- **The retraction is a cursor move, not a destructor call.** Nothing runs `~E()` on that storage.
- Hence the `static_assert`, and hence its exact scope:

```cpp
if constexpr (event_qos0_type<T>) {
    static_assert(std::is_trivially_destructible_v<T>, "EventQOS < 2 require to be trivially destructible");
}
```
<!-- src: qb/src/qb/core/VirtualCore.h:794-796 -->

It fires only for types deriving from `qb::EventQOS0`. A plain `qb::Event` subclass holding a `std::vector` compiles through `send` — and, on every path where it is actually *delivered*, is destroyed exactly once by the receiver, which `SendNonTrivialPayload.{SameCore,CrossCore}DestroysEveryPayloadExactlyOnce` pins to a zero live-object balance (`qb/tests/core/system/messaging/send-nontrivial-payload.cpp`). What the requirement protects is the **drop** path, and only `EventQOS0` events have one: a best-effort event that fails its single `try_send` during the flush is skipped without being disposed, on the strength of one `qos` test (`src/qb/core/VirtualCore.cpp:348-357`). Derive fire-and-forget events from `qb::EventQOS0` so the compiler holds you to it.

> **QoS is a binary backpressure policy, not a priority order.** `qb::EventQOS2` and `qb::EventQOS1` are both `using … = Event` (`src/qb/core/Event.h:499`, `:509`); the base `Event` header encodes `qos = 2` and `EventQOS0`'s constructor sets it to `0` (`src/qb/core/Event.h:414`, `:516-520`). The only read of that field is the flush's `if (!event.state.bits.qos)` gate (`src/qb/core/VirtualCore.cpp:348`). Events drain in FIFO order whatever their QoS.

**Prefer `push`.** `send` buys the possibility of skipping one flush cycle on a cross-core hop and costs the ordering guarantee outright. Reach for it when ordering is genuinely irrelevant *and* you have measured that it matters.

## The reference `push` returns dies at the next push to that core

`push<E>(dest, …)` returns `E&` so you can finish populating the event after construction:

```cpp
// src: derived from qb/tests/core/system/messaging/messaging-api.cpp:145-147
auto &e          = getPipe(_to).allocated_push<TestEvent>(32);
e.has_extra_data = true;
copyAllocatedPayload(e);
```

That reference **dies at the very next event queued to the same destination core** — not at the end of the enclosing scope, and not at the flush. The header says so itself:

> The returned reference dies at the very next event queued to the same destination core — not merely at the end of the enclosing scope.
> — the `@attention` on `Actor::push` (`src/qb/core/Actor.h:861-870`)

The pipe is one growable allocation. `allocate_back` has three branches, and two of them move everything already queued ([`allocate_back`](../0_foundations/buffers.md#what-allocate_back-actually-does)):

- **Fast path** — the reservation fits between `_end` and `_capacity`. Nothing moves; every reference stays valid. This is the common case, which is exactly what makes the rule easy to violate for a long time without noticing.
- **Reallocation** — a new block, a `memcpy`, the old block handed back to `std::allocator`. A stale reference now points at freed memory, which ASan, Valgrind and a hardened allocator all catch on first touch.
- **Compaction** — `reorder()` `memmove`s the live range down to offset 0 *inside the same allocation*. The stale reference still addresses valid, mapped, in-use memory; it simply refers to a **different event**. Nothing at the allocator level went wrong, so no allocator debugger and no sanitizer can see it. And a busy pipe compacts far more often than it grows.

```cpp
auto &evt = push<UpdateValue>(target_id, /*key=*/7, /*value=*/0.0);
evt.value = 42.5;              // OK — nothing queued in between

push<Other>(target_id);        // `evt` is dead from here on
// evt.value = 1.0;            // may write into `Other`, silently
```

Because a `Pipe` is per destination *core*, "the same destination" is wider than it looks: a `push` to a **different actor that happens to live on the same core** invalidates the reference just as thoroughly. So does a helper call that sends, a `broadcast` (which sends to every core, this one included), and the next iteration of a loop. Populate the event fully before queueing anything else. Pinned by `PipeAllocatorContract.*` in `qb/tests/io/unit/core/pipe-allocator.cpp`.

## Payloads must be trivially **relocatable**, not merely copyable

An event is moved by raw `memcpy`: the bytes are copied to a new address and the source is abandoned **without running a destructor there**. A member holding a pointer into its own storage therefore keeps addressing the old bytes after the move.

C++20 has no `is_trivially_relocatable` trait, clang's builtin rejects `std::vector` (which is safe here), and without reflection a `static_assert` cannot inspect an event's members. So there is no compile-time check, and there cannot be one. `qb::string<N>`, plain data, `std::unique_ptr`, `std::shared_ptr` and `std::vector` are all fine. A **by-value `std::string` is not**: on libstdc++ a short string's `_M_p` addresses its own inline `_M_local_buf`, so the receiver reads reused memory and `~basic_string()` calls `operator delete` on a pointer that never came from the heap. libc++ recomputes `data()` from `this`, so the defect is structurally invisible on macOS and corrupts only on Linux.

**The rule is not scoped to cross-core delivery.** An event is relocated at three independent points, and only the last one crosses a core:

| Where | What moves it | Applies to a same-core `push`? |
|---|---|---|
| Pipe growth or compaction | `allocate_back`'s reallocation `memcpy`s the live range into a new block; `reorder()` `memmove`s it down in place (`src/qb/system/allocator/pipe.h:356-392`, `:520-528`) | **yes** — `push` places same-core events in a pipe too |
| `reply` / `forward` | both route through `VirtualCore::send(Event const&)`, which byte-recycles the event into a pipe with `VirtualPipe::recycle` — a `memcpy` into a fresh reservation (`src/qb/core/VirtualCore.cpp:975-981`; `src/qb/system/allocator/pipe.h:509-513`) | **yes** |
| The cross-core hop | sender pipe → mailbox ring → receive buffer: two more `memcpy`s | no |

### What the debug guard does, and the two things it cannot see

Before handing an event to a peer's ring, a debug build scans the event's whole bucket range for a pointer-sized word addressing that same range, and aborts with a diagnostic rather than corrupting the receiver:

```cpp
for (std::size_t off = 0; off + sizeof(std::uintptr_t) <= bytes; off += alignof(std::uintptr_t)) {
    std::uintptr_t word = 0;
    std::memcpy(&word, base + off, sizeof(word));
    if (word >= lo && word < hi)
        return true;
}
```
<!-- src: qb/src/qb/core/Main.cpp:180-186 -->

It is `#ifndef NDEBUG`, so release pays nothing (`src/qb/core/Main.cpp:193`). Two gaps follow from where it sits:

- **It never runs for same-core delivery**, which does not go through the mailbox layer at all — the exact path the table above says is *not* exempt.
- **It looks for a word addressing the event's current bytes.** A self-pointer that an earlier pipe growth already left dangling now points at the old buffer, outside the scanned range, and passes.

It is also only sound because every construction site zeroes the bucket range first. An event's range is not fully written by its payload — dead bytes inside `sizeof(E)`, tail padding, an `allocated_push` tail — and those bytes come out of a recycled buffer, so a stale value that happens to address the range makes the guard fire on a perfectly relocatable payload. That was measured at 2 runs in 30 on `qb-core-test-system-shutdown-saturation` before `prepare_event_storage` was introduced, with the offending words at offsets 40 and 56 of a 64-byte event whose live members end at 52 (`src/qb/core/Main.cpp:161-171`). Treat a clean debug run as evidence, not proof. Pinned by `RelocatablePayload.*` / `RelocatablePayloadDeathTest.*` in `qb/tests/core/system/messaging/relocatable-payload.cpp`.

## `reply` and `forward` reuse the event in place

```cpp
void
VirtualCore::reply(Event &event) noexcept {
    std::swap(event.dest, event.source);
    event.state.bits.alive = 1;
    send(event);
}
```
<!-- src: qb/src/qb/core/VirtualCore.cpp:989-994 -->

`forward` is the same three lines with `event.dest = dest;` in place of the swap, and **deliberately leaves `event.source` untouched** so a downstream `reply` returns to the true client rather than to the forwarder (`src/qb/core/VirtualCore.cpp:996-1001`). In one line: *`reply` swaps `dest` and `source`; `forward` sets a new `dest` and keeps `source`.*

Three consequences:

- **The handler must take the event by non-const reference.** Both mutate it in place; `void on(E const&)` will not compile against `reply(event)`.
- **Both route through `send`, not `push`,** so a replied or forwarded event carries no ordering guarantee relative to your pushes to the same destination.
- **Both byte-recycle the received event** into a pipe with `recycle`, so the relocation rule applies to them unconditionally, same core or not.

A broadcast event can be neither replied to nor forwarded: `Actor::reply` and `Actor::forward` test `event.dest.is_broadcast()`, log a warning and return without sending (`src/qb/core/Actor.cpp:301-307`, `:309-318`). After either call the event is consumed — do not read or modify it.

## `broadcast`, and the two ways to fan out

```cpp
for (const auto it : _engine._core_set.raw())
    send<T>(BroadcastId(it), source, init...);
```
<!-- src: qb/src/qb/core/VirtualCore.h:832-833 -->

One `send` per registered core, addressed to `BroadcastId(core)`. Note `init...` rather than `std::forward<_Init>(init)...`: forwarding an rvalue would move it into the first core's event and leave every later core constructing from moved-from arguments — an empty string on every core but one (`src/qb/core/VirtualCore.h:827-831`).

On the receiving side, a broadcast destination makes the router snapshot every subscribed handler for that type into a `thread_local` buffer and then dispatch from the snapshot, because a handler may subscribe or unsubscribe during the walk — spawning an actor registers `KillEvent`, and that insert can rehash and reallocate the entry array under a live iterator (`src/qb/system/event/router.h:310-345`).

To reach every actor on **one** core, push to a `qb::BroadcastId` instead. That keeps `push`'s ordering, because it goes through the ordinary tail allocation:

```cpp
broadcast<SystemNotice>("shutting down");          // every actor, every core — via send
push<CacheFlush>(qb::BroadcastId(1), "users");     // every actor on core 1 — via push, ordered
```

Because `broadcast` fans out through `send`, it inherits `send`'s drop path on every remote core. Broadcast a trivially destructible event, ideally one deriving from `qb::EventQOS0` so the `static_assert` holds you to it, and keep bulk data behind a `std::shared_ptr`.

## The size ceiling, and what happens past it

An event's in-pipe footprint must fit the destination mailbox ring. The two constants are close together and easy to conflate:

| Constant | Value at a 64-byte bucket | What it sizes |
|---|---|---|
| `SharedCoreCommunication::MaxRingEvents` | `65535 / 64` = **1023** buckets (≈ 64 KiB) | each per-producer SPSC ring inside a mailbox (`src/qb/core/Main.h:326`) |
| `VirtualCore::MaxRingEvents` | `65536 / 64` = **1024** buckets | the per-core `EventBuffer` the receive path copies into (`src/qb/core/VirtualCore.h:152`) |
| `VirtualCore::kMaxDeliverableBuckets` | = the first of the two, **1023** | the widest event `__flush_all__` will even attempt (`src/qb/core/VirtualCore.h:154`) |

The ring enqueue is all-or-nothing, so an event wider than 1023 buckets is not *backpressured* — it is permanently undeliverable, however much the consumer drains. Retrying it would hold the whole FIFO pipe to that core hostage behind it, and `Main::join()` would never return. The flush therefore separates the two cases before it retries anything:

```cpp
if (unlikely(event.bucket_size > kMaxDeliverableBuckets)) {
    QB_LOG_CRIT(…);
    _router.dispose(event);
```
<!-- src: qb/src/qb/core/VirtualCore.cpp:335-341 -->

The event is disposed (its destructor *does* run), the pipe advances past it, and the rest of the pipe keeps flowing. **The message is lost and nothing at the call site says so** — the only trace is a `LOG_CRIT` naming source, destination and bucket count. A malformed `bucket_size == 0`, reachable only by overflowing that `uint16_t` with a ≥ 65536-bucket `allocated_push`, cannot even be stepped over, so the rest of that pipe is discarded instead (`src/qb/core/VirtualCore.cpp:325-333`). Pinned by `OversizeEvent.OversizedEventDoesNotWedgeTheEngine` in `qb/tests/core/system/messaging/oversize-event-probe.cpp`.

### `allocated_push` sizes the **tail**, not the event

```cpp
size += sizeof(T);
size = size / sizeof(EventBucket) + static_cast<bool>(size % sizeof(EventBucket));
```
<!-- src: qb/src/qb/core/Pipe.h:324-325 -->

The `size` argument is the **extra bytes after the event object**; `sizeof(_Event)` is added internally and the sum is rounded up to whole buckets. So `allocated_push<E>(0)` reserves exactly one event's worth — the event is never under-allocated — and a non-zero `size` is correct only when you intend to write raw bytes into the region immediately following the event, the way `AllocatedPipePushActor` does with `allocated_push<TestEvent>(32)` plus a 32-byte tail (`qb/tests/core/system/messaging/messaging-api.cpp:145-148`).

Passing `sizeof(E) + n` double-counts one event and halves the usable ceiling. Passing the payload size when the payload is heap-owned is worse:

```cpp
auto blob = std::make_shared<std::vector<char>>(256 * 1024);
qb::Pipe pipe = getPipe(processor_id);
// 0, not blob->size(): the blob lives on the heap, so the event's in-pipe
// footprint is just the event. blob->size() would reserve 256 KiB and put the
// event 4x past the 1023-bucket cross-core ceiling — dropped, with a LOG_CRIT.
pipe.allocated_push<BlobEvent>(0, blob);
```

**Size the event small, not the payload.** The hint only helps the source pipe reserve space; the ceiling that matters is the destination ring, and no hint moves it.

## Ordering, stated exactly

The runtime makes one ordering promise:

> Events delivered with `push` (or `Pipe::push`, or `to(dest).push`) from a **single source actor** to a **single destination actor** are processed by the destination in the exact order they were pushed.

It holds because `push` appends to the tail of one buffer and the destination drains that buffer front to back, and because a `VirtualCore` is single-threaded and an actor never migrates, so no handler ever runs concurrently with another handler of the same actor.

What it does **not** cover:

- **No cross-source ordering.** If A and B both push to C, each source's own subsequence is preserved; the merge is unspecified.
- **No cross-destination ordering.** Two pushes from A to different actors have no relationship — even when both destinations are on the same core, because the flush and the drain interleave with the peers' own work.
- **`send` is unordered, period** — see the retraction above.
- **`reply` and `forward` are `send`**, so they are unordered relative to your pushes.
- **`broadcast` fans out as independent sends.** Per-recipient there is no ordering at all; across recipients there is no global order.

## The event header

`qb::Event` is cache-line aligned and carries 12 bytes of routing metadata before your first member (`src/qb/core/Event.h:336`):

```cpp
    union Header {
        struct {
            uint32_t : 16, : 8, alive : 1, qos : 2, factor : 5;
        } bits;
        uint8_t prot[4] = {'q', 'b', '\0', 4 | ((QB_LOCKFREE_EVENT_BUCKET_BYTES / 16) << 3)};
    } state;
    uint16_t bucket_size;
    id_type  id;
    // for users
    id_handler_type dest;
    id_handler_type source;
```
<!-- src: qb/src/qb/core/Event.h:395-420 -->

Three details are load-bearing:

- **The bit-fields live in a named struct, never as bare union members.** In a union every member sits at offset 0 and each bit-field declarator is its own member, so `alive`, `qos` and `factor` would all alias one another *and* `prot[0]`: writing `alive` would rewrite `qos`, and `reply()` would mutate the `'q'` of the magic on every call. Inside a struct the `: 16, : 8` padding declarators do their job and place `alive` at bit 24 — `prot[3]`, the one byte the default initialiser encodes.
- **`id_type` is `EventId` (a `uint16_t`) in every build mode.** It used to be `const char *` under `!NDEBUG`, which moved `dest` from offset 8 to offset 16. Cross-core events are memcpy-relocated and `libqb-core` is installable, so a consumer built with the other `NDEBUG` read `dest` at the wrong offset and routed to a garbage `ActorId`, silently. The human-readable name moved to a side registry — `qb::event_type_name(id)`, diagnostics only (`src/qb/core/Event.h:348-361`, `:486-489`).
- **`bucket_size` is 16 bits**, which is where the 65536-bucket wrap in the previous section comes from, and it is what keeps the whole header inside one cache line. `getSize()` multiplies it back out by the bucket size (`src/qb/core/Event.h:471-474`).

Type ids are dense and assigned once per type through a magic static, then recorded in a process-wide registry keyed by `typeid(T).name()` so that a second image whose own magic static failed to coalesce recovers the id `T` already has instead of minting a colliding one (`src/qb/core/Event.h:236-242`). A `type_id<T>()` value is stable for the life of the process and **not** stable across runs; do not persist it.

## `noexcept` on the message path

`push`, `send`, `broadcast`, `reply`, `forward`, `Pipe::push` and `Pipe::allocated_push` are all `noexcept`, yet they grow a pipe buffer — which can throw `std::bad_alloc` — and run your event's constructor in place, which can throw anything. A throw cannot cross a `noexcept` boundary, so **any such failure calls `std::terminate()` and aborts the process** (`src/qb/core/Actor.h:871-875`; `src/qb/core/Pipe.h:135-147`).

This is a design position, not an oversight: events are expected to be small, allocation-light messages on an adequately provisioned system, and an allocation failure in the messaging hot path is treated as fatal. Keep event constructors cheap, and move heap data in through an already-allocated `std::shared_ptr` rather than allocating inside the constructor.

## Pitfalls

- **Holding a `push` reference across another send.** It dies at the next event queued to the same destination *core* — including one addressed to a different actor on that core. Compaction makes the failure invisible to every sanitizer. Populate the event, then queue.
- **A by-value `std::string` in an event.** Not valid on any path, not just cross-core. Use `qb::string<N>` for inline text, or box it behind a `std::shared_ptr`. The debug guard catches the common case on the cross-core hop only.
- **Using `send` because it "looks faster".** It costs the ordering guarantee outright and only sometimes skips a flush cycle. The default answer is `push`.
- **A non-trivially-destructible `send` payload.** Delivery is safe — the receiver destroys it exactly once. The drop path is not, and only `EventQOS0` events have one, which is exactly where the `static_assert` fires. Derive fire-and-forget events from `qb::EventQOS0`.
- **Sizing `allocated_push` to the payload.** The argument is the trailing bytes after the event; `sizeof(E)` is added for you. An oversized event is dropped cross-core with a `LOG_CRIT` and nothing at the call site fails.
- **Replying to or forwarding a broadcast.** Logged and dropped. Build a fresh unicast event instead.
- **Taking the event by `const&` and then calling `reply`/`forward`.** Both mutate in place; declare `void on(E &event)`.
- **Assuming `reply` keeps its place in the `push` order.** It goes through `send`. If a response must follow earlier pushes in order, `push` a new event.
- **Allocating inside an event constructor under memory pressure.** The `noexcept` path terminates the process.

## See also

- [The pipe: qb's one buffer](../0_foundations/buffers.md) — the cursor model, the three branches of `allocate_back`, and why memory only grows. This page is that mechanism applied to events.
- [Writing actors](./actor.md) — the send API in the context of an actor's life, and what happens to a coroutine when the actor dies.
- [The engine](./engine.md) — the loop pass that runs the flush and the receive, and the backpressure policy behind a failed `try_send`.
- [The event system](../2_core_concepts/event_system.md) — defining event types, `qb::string<N>` versus `std::string`, and handler registration.
- [Concurrency primitives](../0_foundations/concurrency_primitives.md) — the MPSC ring the flush enqueues into.
- [Core invariants](../7_reference/core_invariants.md) — the same guarantees in reference form.
