# Concurrency primitives

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 3.1.0 (C++20 default, C++23 supported) — ef7d3ea7

The lock-free building blocks under `qb/system/lockfree` — the SPSC ring buffer, the sharded MPSC ring built on it, an unbounded MPSC queue and a TTAS spinlock — plus the CPU facilities in `qb/system/cpu.h` they rest on, and the threading contract you must honour to use any of them directly.

**Prerequisites:** [Threading model](../2_core_concepts/threading_model.md), [Concurrency in qb](../2_core_concepts/concurrency.md) — **See also:** [Foundations overview](./README.md) · [Core invariants](../7_reference/core_invariants.md) · [The engine](../4_qb_core/engine.md) · [The pipe](./buffers.md)

## Summary

`qb` reaches for a lock only where it cannot avoid one. Every cross-core event travels through a lock-free multi-producer/single-consumer ring buffer, and there is no `std::mutex` anywhere on that path (`src/qb/core/Main.h:328`). This page documents the four primitives that make that possible — `qb::lockfree::spsc::ringbuffer`, the sharded `qb::lockfree::mpsc::ringbuffer` built on top of it, the unbounded `qb::lockfree::mpsc_unbounded_queue`, and `qb::lockfree::SpinLock` — and it is precise about which of them the engine actually uses, because two of the four it does not.

Application code rarely touches any of this. The actor model — `push`, `send`, `reply`, `broadcast` — is the supported interface for inter-actor and inter-core communication, and it abstracts every primitive below behind a safer, type-checked surface. Read this page when you are contributing to the engine, embedding a primitive in your own framework-adjacent code, or reasoning about why the message path has the performance and safety properties it does.

These primitives are correct only under tightly scoped threading contracts. Each one names exactly which thread may call which method. Violating that contract does not raise a compile error — it corrupts state silently. Treat the per-primitive contract sections below as load-bearing.

## The real-multithread contract

`qb-core` is a share-nothing runtime: each `VirtualCore` runs on one thread and never shares actor state, so almost nothing in the actor layer needs an atomic or a lock ([Core invariants](../7_reference/core_invariants.md)). The lock-free primitives are the narrow exception — they are the seam where genuinely concurrent threads meet. That seam is governed by one rule per primitive:

| Primitive | Concurrent producers | Concurrent consumers | Synchronization |
| --- | --- | --- | --- |
| `spsc::ringbuffer` | exactly one | exactly one | none (separate cache-line-isolated indices) |
| `mpsc::ringbuffer`, indexed enqueue | one per index | exactly one | none — caller-bound producer slots |
| `mpsc::ringbuffer`, round-robin enqueue | many | exactly one | per-producer `SpinLock` |
| `mpsc_unbounded_queue` | many | exactly one | lock-free (Michael-Scott) |

"Exactly one consumer" is not advisory. In the SPSC buffer, `write_index_` is written only from the enqueue side and `read_index_` only from the dequeue side; a second producer or a second consumer races those stores and the result is undefined (`src/qb/system/lockfree/spsc.h:151`). The MPSC `dequeue` and `consume_all` overloads are likewise single-consumer only (`src/qb/system/lockfree/mpsc.h:183-187`).

These types live in `qb` — the foundation shared by `qb-io` and `qb-core` — not in either library exclusively. They depend on [the time vocabulary](./time.md) (`qb::duration`, `qb::mono_time`) and on the cache-line constants from [`qb/utility/prefix.h`](./abi_and_build_fingerprint.md).

## `SpinLock`

- **Header:** `qb/system/lockfree/spinlock.h`
- **Type:** `qb::lockfree::SpinLock`

A test-and-test-and-set (TTAS) busy-wait mutex over a single `std::atomic<bool>` (`src/qb/system/lockfree/spinlock.h:205`). A contending thread loops ("spins") on a *relaxed load* — issuing `qb::spin_loop_pause()` between reads — and only retries the atomic exchange once the lock appears free (`src/qb/system/lockfree/spinlock.h:184-192`).

That shape is not a micro-optimisation. Retrying `exchange` back to back is a read-modify-write: it takes the cache line **exclusive** on every attempt, so N waiters ping-pong the line and actively starve the holder they are waiting on. Spinning on a relaxed load keeps the line shared. Measured over 20 000 acquisitions per thread, applying TTAS to the `trylock(int64_t)` wait loop — the one that previously had nothing between attempts at all — was 1.5× faster at 2 threads, ~1.8× at 4, and **4.3× at 8** with a longer critical section (`src/qb/system/lockfree/spinlock.h:107-114`).

`SpinLock` satisfies the C++ *BasicLockable* requirement (`lock()` / `unlock()`), so it composes directly with `std::lock_guard<SpinLock>` — which is exactly how `mpsc::ringbuffer` uses it (`src/qb/system/lockfree/mpsc.h:135`).

### Interface

```cpp
// src: src/qb/system/lockfree/spinlock.h
namespace qb::lockfree {
class SpinLock {
public:
    SpinLock() noexcept;                                       // starts unlocked
    SpinLock(const SpinLock &) = delete;                       // non-copyable
    SpinLock(SpinLock &&)      = delete;                       // non-movable

    [[nodiscard]] bool locked() const noexcept;                // is the lock held?
    [[nodiscard]] bool trylock() noexcept;                     // one attempt, no spin
    [[nodiscard]] bool trylock(int64_t spin) noexcept;         // up to `spin` retries
    [[nodiscard]] bool trylock_for(qb::duration timespan) noexcept;
    [[nodiscard]] bool trylock_until(qb::mono_time deadline) noexcept;
    void lock() noexcept;                                      // spin until acquired
    void unlock() noexcept;
};
}
```

### Contract and behavior

- **Non-copyable, non-movable.** All four copy/move special members are deleted (`src/qb/system/lockfree/spinlock.h:53,58,68,73`). A `SpinLock` must stay put in memory — embed it as a member, never pass it by value.
- **Timed methods use the monotonic clock.** `trylock_for(qb::duration)` computes its deadline with `qb::mono_now()` (steady clock), never wall time, so adjusting the system clock cannot extend or shorten the wait (`src/qb/system/lockfree/spinlock.h:135-138`). `trylock_until(qb::mono_time)` delegates to `trylock_for(deadline - qb::mono_now())` (`src/qb/system/lockfree/spinlock.h:171-174`). This is [the two-instant-types rule](./time.md#mono_time-and-wall_time-do-not-mix) paying for itself in the framework's own code.
- **A past deadline degrades to a single try.** Because `trylock_until` subtracts the current time from the deadline, a deadline already in the past yields a negative `qb::duration`; the `do … while` body still runs once, so the call performs exactly one non-blocking `trylock()` and returns its result rather than failing outright (`src/qb/system/lockfree/spinlock.h:141-147`).
- **Each load-spin iteration consumes one unit of the caller's budget** in `trylock(int64_t)`, so total work stays bounded by `spin`, and a free lock is still acquired on the first attempt (`src/qb/system/lockfree/spinlock.h:116-120`).
- **Try-acquire results are `[[nodiscard]]`.** `trylock`, its timed variants, and `locked()` are all marked; ignoring the return value is a logic error the compiler will warn about.

### When to reach for it

Use `SpinLock` only for critical sections that are a handful of instructions long and contended rarely, where the syscall overhead of a `std::mutex` would dominate. The cost model is the inverse of a sleeping mutex: a thread that cannot acquire the lock burns 100% of its core spinning. If the section can be held for more than a few instructions, or contention is high, a `std::mutex` is the correct choice.

**Inside the framework it is used in exactly one place** — the MPSC round-robin enqueue overloads — and the engine does not call those. See the MPSC section below.

## SPSC ring buffer

- **Header:** `qb/system/lockfree/spsc.h`
- **Types:** `qb::lockfree::spsc::ringbuffer<T, _MaxSize>` (fixed) and `qb::lockfree::spsc::ringbuffer<T, 0>` (runtime-sized)

A bounded, wait-free single-producer/single-consumer FIFO. The producer advances `write_index_`; the consumer advances `read_index_`. The two indices live on separate cache lines — `padding1` is sized `QB_LOCKFREE_CACHELINE_BYTES - sizeof(size_t)` — so the producer's and consumer's writes never trigger false sharing (`src/qb/system/lockfree/spsc.h:55-59`). Coordination uses acquire/release ordering on the indices; no lock is taken on either side.

### Type requirements and capacity

- **`T` must be trivially copyable.** A `static_assert` enforces this, because the bulk enqueue/dequeue paths move elements with `std::memcpy` (`src/qb/system/lockfree/spsc.h:52-53`). The single-element `enqueue(T const&)` placement-news a copy, but for the trivially-copyable `T` the type allows, that is byte-equivalent to the bulk `memcpy`; no per-element constructor or destructor runs on the bulk path.
- **One slot is reserved.** A buffer of requested capacity *N* allocates *N + 1* slots: the extra slot disambiguates full from empty (the buffer is full when advancing the write index would collide with the read index). Usable capacity equals the requested `_MaxSize` (fixed variant) or the constructor argument (runtime variant) (`src/qb/system/lockfree/spsc.h:370`).
- **Fixed vs. runtime size.** `ringbuffer<T, _MaxSize>` embeds a `std::array<T, _MaxSize + 1>` sized at compile time. `ringbuffer<T, 0>` takes the size as a constructor argument and allocates `new T[size + 1]` (`src/qb/system/lockfree/spsc.h:477-479`).

### Interface

The two specializations expose the same public surface (signatures shown for the fixed variant):

```cpp
// src: src/qb/system/lockfree/spsc.h
namespace qb::lockfree::spsc {
template <typename T, std::size_t _MaxSize>
class ringbuffer {
public:
    // --- producer side (one thread only) ---
    bool   enqueue(T const &t) noexcept;                       // one item; false if full
    template <bool _All = true>
    size_t enqueue(T const *t, size_t size) noexcept;          // bulk; returns count enqueued

    // --- consumer side (one thread only) ---
    bool   dequeue(T *ret) noexcept;                           // one item; false if empty
    size_t dequeue(T *ret, size_t size) noexcept;              // bulk; returns count dequeued
    template <typename Func>
    size_t dequeue(Func const &func, T *ret, size_t size) noexcept;   // dequeue then func(ret, n)
    template <typename Func>
    size_t consume_all(Func const &func) noexcept;             // func(segment, len) per segment

    // --- either side, but observe the contract below ---
    [[nodiscard]] bool empty() const noexcept;
};
}
```

- **Bulk enqueue is all-or-nothing by default.** The `_All` template parameter defaults to `true`: `enqueue<true>(t, size)` enqueues every element or none, returning `0` when there is insufficient room (`src/qb/system/lockfree/spsc.h:181-183`). Set `_All = false` for a partial enqueue that takes as many elements as fit.
- **`consume_all` avoids the intermediate copy** — and that is exactly why it is the dangerous overload. It invokes `func` directly over the buffer's internal storage, passing a `T *` to the start of one contiguous segment and that segment's element count. Because the ring wraps, a full traversal can call the `functor` **twice**, once per segment (`src/qb/system/lockfree/spsc.h:305-306`).

> **`consume_all` is only safe when one ring slot is one independent item.** The producer's bulk `enqueue` deliberately writes across the wrap with a two-section `memcpy`, so a logical item spanning several slots — a multi-bucket `qb::Event` is the obvious one — is split by that second call. A consumer that parses variable-length items by an embedded size field then reads a header whose item runs past the end of the segment. When items can span more than one slot, use the copy-out `dequeue(func, scratch, n)` overload instead: it reassembles the batch contiguously before calling the functor. This is not a hypothetical distinction — it is why the engine drains its mailbox through the copy-out overload (`mpsc::consume_all(func, scratch, chunk)`, which forwards to this one) rather than the in-place `consume_all(func)` (`src/qb/system/lockfree/spsc.h:275-283`).

### Where the engine uses it

The SPSC buffer is the building block of the MPSC mailbox, not a directly used type in the actor layer. Each producer slot of the inter-core mailbox is one SPSC ring buffer (next section).

## MPSC ring buffer

- **Header:** `qb/system/lockfree/mpsc.h`
- **Types:** `qb::lockfree::mpsc::ringbuffer<T, max_size, nb_producer>` (fixed producer count) and `qb::lockfree::mpsc::ringbuffer<T, max_size, 0>` (runtime producer count)

A bounded multi-producer/single-consumer ring buffer, built as an array (fixed) or vector (runtime) of per-producer SPSC rings. Each producer owns a dedicated SPSC ring, so producers that write to distinct rings never contend; the single consumer drains across all rings. Each `Producer` struct pads its `SpinLock` out to a full cache line to keep producers off each other's cache lines (`src/qb/system/lockfree/mpsc.h:58`).

This is the core data structure for inter-`VirtualCore` communication. Each destination core owns one mailbox; every other core writes into its own dedicated SPSC slot of that mailbox, and the destination core is the sole consumer that drains across all slots:

```mermaid
flowchart LR
    P0["VirtualCore 0<br/>(producer)"]
    P1["VirtualCore 1<br/>(producer)"]
    PN["VirtualCore N<br/>(producer)"]
    subgraph MB["Destination Mailbox — mpsc::ringbuffer&lt;EventBucket, MaxRingEvents, 0&gt;"]
        direction TB
        R0["Producer slot 0<br/>SPSC ring"]
        R1["Producer slot 1<br/>SPSC ring"]
        RN["Producer slot N<br/>SPSC ring"]
    end
    DC["Destination VirtualCore<br/>(single consumer)"]
    P0 -- "enqueue(0, bucket)" --> R0
    P1 -- "enqueue(1, bucket)" --> R1
    PN -- "enqueue(N, bucket)" --> RN
    R0 -- "consume_all(func, scratch, chunk) drains each ring" --> DC
    R1 --> DC
    RN --> DC
```

The producer index is the **sender's** resolved core id, so each producer is permanently bound to one slot and rides the lock-free indexed path — no `SpinLock`, no cross-producer contention (see `SharedCoreCommunication::send`, below). The single consumer's `dequeue` advances `ret` by the count taken from each ring, so slots append rather than overwrite (`src/qb/system/lockfree/mpsc.h:183-187`).

### Two enqueue families — read this before using

The enqueue API splits into two families with very different safety properties. Choosing the wrong one is the most common way to misuse this type.

**Indexed enqueue — no lock, caller-bound producers.** The compile-time `enqueue<_Index>(...)` and runtime `enqueue(index, ...)` overloads take **no lock**. They write straight into the SPSC ring at that index, which is correct only if at most one thread ever uses a given index (`src/qb/system/lockfree/mpsc.h:74-77`, `:102-105`). This is the fast path: when each producer thread is permanently bound to its own ring — as in the engine, where the producer index is the sender core's id — no synchronization is needed at all.

**Round-robin enqueue — `SpinLock`-guarded, any thread.** The `enqueue(T const&)` and `enqueue(T const*, size)` overloads pick a producer ring via a `static thread_local` counter modulo the producer count, then take that producer's `SpinLock` under a `std::lock_guard` before writing (`src/qb/system/lockfree/mpsc.h:133-136`). These overloads are safe to call concurrently from arbitrarily many threads, at the cost of the spinlock; the modulo also balances load across rings.

> **Where the framework's only `SpinLock` lives, precisely.** All four `std::lock_guard<SpinLock>` sites in the tree are inside these round-robin overloads — `src/qb/system/lockfree/mpsc.h:135` and `:155` in the fixed-producer specialisation, `:381` and `:401` in the runtime one. **The engine calls none of them.** `SharedCoreCommunication::send` passes its own resolved core id to the *indexed* overload (`src/qb/core/Main.cpp:215`), so the answer to "is there a lock on the message path?" is no — not a fast one, not an uncontended one: none is taken.

```cpp
// src: src/qb/system/lockfree/mpsc.h
namespace qb::lockfree::mpsc {
template <typename T, std::size_t max_size, size_t nb_producer = 0>
class ringbuffer {
public:
    // --- indexed enqueue: NO lock; one thread per index ---
    template <size_t _Index>             bool   enqueue(T const &t);
    template <size_t _Index, bool _All = true>
                                         size_t enqueue(T const *t, size_t size);
    bool   enqueue(size_t index, T const &t);
    template <bool _All = true>
    size_t enqueue(size_t index, T const *t, size_t size);

    // --- round-robin enqueue: per-producer SpinLock; any thread ---
    size_t enqueue(T const &t);
    template <bool _All = true>
    size_t enqueue(T const *t, size_t size);

    // --- consumer side (one thread only) ---
    size_t dequeue(T *ret, size_t size);                       // TOTAL budget across all rings
    template <typename Func>
    size_t consume_all(Func const &func, T *scratch, size_t chunk); // chunk is PER-PRODUCER
    template <typename Func>
    size_t consume_all(Func const &func);                      // in-place, no copy

    auto  &ringOf(size_t index);                               // direct SPSC-ring access
};
}
```

### Contract and behavior

- **At least one producer is required (runtime variant).** `ringbuffer<T, max_size, 0>` deletes its default constructor and asserts `nb_producer > 0` in the one it has, because the round-robin paths compute `tl_index % _nb_producer` — zero producers would be a division by zero (`src/qb/system/lockfree/mpsc.h:297-311`).
- **`dequeue(T*, size)` appends across producers.** The single-consumer drain advances the output pointer by the count taken from each ring, so items from later producers are appended after earlier ones rather than overwriting them. The in-source comment is explicit that the alternative is silent data loss (`src/qb/system/lockfree/mpsc.h:183-187`).
- **`ringOf(index)` is an escape hatch.** It returns the underlying SPSC ring by reference for direct access. The SPSC single-producer/single-consumer contract then applies to whatever you do with it; there is no MPSC-level guard.

### Where the engine uses it

Each `VirtualCore` consumes from exactly one inbound mailbox. A `Mailbox` is a `lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>` — the runtime-producer-count specialization (`src/qb/core/Main.h:328`) — with the producer count fixed at construction to the number of cores (`src/qb/core/Main.cpp:128-130`). The mailboxes are owned by an internal `SharedCoreCommunication` instance held by `qb::Main` (`src/qb/core/Main.h:323`).

- **`EventBucket`** is a cache-line-aligned padding unit (`QB_LOCKFREE_EVENT_BUCKET_BYTES`, equal to `QB_LOCKFREE_CACHELINE_BYTES`, default 64) so event payloads stay cache-aligned in the ring (`src/qb/utility/prefix.h:68,138-139`).
- **`MaxRingEvents`** is `uint16_t::max() / QB_LOCKFREE_EVENT_BUCKET_BYTES` — **1023** at the default cache line — the per-producer ring capacity, derived so a bucket count fits a 16-bit field (`src/qb/core/Main.h:326`). It is also the widest event that can *ever* cross a core boundary: `VirtualCore::kMaxDeliverableBuckets` is defined as this same constant (`src/qb/core/VirtualCore.h:154`), and a wider event is disposed rather than retried, because the ring enqueue is all-or-nothing and no amount of draining would help.
- **Producers are core-bound.** `SharedCoreCommunication::send` enqueues into the destination mailbox with the *sender's* resolved core id as the producer index — `_mail_boxes[dest_index]->enqueue(source_index, …)` (`src/qb/core/Main.cpp:215`) — so the engine rides the lock-free runtime-indexed path, not the spinlock-guarded round-robin path.
- **The single consumer drains via the copying `consume_all` overload.** `VirtualCore::__receive__` calls `_mail_box.consume_all(func, _event_buffer->data(), MaxRingEvents)`, which copies each producer ring's pending `EventBucket`s into the core's event buffer and then invokes the functor over that buffer (`src/qb/core/VirtualCore.cpp:226-228`). It is **not** the in-place `consume_all(func)` overload, and it cannot be: an event spans several buckets, so the wrap-splitting warning above applies directly. The third argument is a **per-producer** batch limit, not a total budget — that is what makes one call drain every peer core's ring rather than stopping at the first saturated one. It was spelled `dequeue(func, ret, size)` before 3.0, a name that read as a bounded sibling of `dequeue(T*, size)` and was not one.
- **Idle parking is a `condition_variable`, not the ring.** A `Mailbox` wraps the ring with a `std::mutex`/`std::condition_variable` pair used only when a core parks at non-zero latency: a busy producer calls `notify()` to wake an idle consumer (`src/qb/core/Main.h:330-331`, `:348-354`, `:365-369`; the producer side is `src/qb/core/VirtualCore.cpp:386`). At zero latency the consumer spins and `notify()` is a no-op. This parking lock is off the message path — the ring itself stays lock-free.

The full back-pressure protocol when a peer mailbox is full (bounded spin-then-yield, partial flush, guaranteed termination) is documented in [Core invariants](../7_reference/core_invariants.md#bounded-inter-core-flush-no-cross-core-deadlock).

## Unbounded MPSC queue

- **Header:** `qb/system/lockfree/mpsc_unbounded_queue.h`
- **Type:** `qb::lockfree::mpsc_unbounded_queue<T>`

A lock-free, unbounded multi-producer/single-consumer queue implementing the Michael-Scott linked-list algorithm. Unlike the bounded `mpsc::ringbuffer`, it never reports "full" — it allocates a heap node per pushed item. The header names its intended use cases as coroutine ready-queues, task queues, and work-stealing tails, where back-pressure is handled elsewhere (`src/qb/system/lockfree/mpsc_unbounded_queue.h:7`).

**It is a standalone primitive offered to you: the engine's inter-core message path uses the bounded `mpsc::ringbuffer`, not this queue.** Nothing under `qb/src` instantiates it; its only user in this checkout is its own unit test, `qb/tests/core/unit/lockfree/mpsc-unbounded-queue.cpp`. That is a deliberate statement, not an omission — reading it as engine machinery would give you the wrong answer to "how much does qb allocate per message?", which in steady state is nothing at all.

```cpp
// src: src/qb/system/lockfree/mpsc_unbounded_queue.h
namespace qb::lockfree {
template <typename T>
class mpsc_unbounded_queue : public nocopy {
public:
    void push(T item);                       // lock-free; safe from many producers
    bool pop(T &out);                        // single consumer only; false if empty
    [[nodiscard]] std::size_t size() const;  // approximate
    [[nodiscard]] bool empty() const;        // approximate; consumer only
};
}
```

### Contract and behavior

- **`push()` is lock-free and multi-producer-safe.** Any number of threads may push concurrently. A push allocates one heap node and links it via an atomic `exchange` on the tail (`src/qb/system/lockfree/mpsc_unbounded_queue.h:80-84`).
- **`pop()` is single-consumer only.** Only the one designated consumer thread may call `pop()`; it moves the value into `out` and returns `false` when the queue is empty (`src/qb/system/lockfree/mpsc_unbounded_queue.h:107-113`).
- **`T` must be movable.** `push` takes `T` by value and moves it into the node; `pop` moves the node's value into `out` (`src/qb/system/lockfree/mpsc_unbounded_queue.h:43`).
- **`size()` and `empty()` are approximate, and the direction of the error is fixed.** The producer increments the counter *before* publishing the node, so `size()` can only ever over-estimate — an item is counted a few instructions before it becomes poppable (`src/qb/system/lockfree/mpsc_unbounded_queue.h:98-99`). Ordering it the other way round leaves a one-instruction window in which the consumer's `fetch_sub` runs against a counter that was never incremented, and on an unsigned counter at zero that wraps to `SIZE_MAX`. Only the consumer should consult either, and only as a hint (`src/qb/system/lockfree/mpsc_unbounded_queue.h:135-148`).
- **A sentinel node always remains.** The constructor allocates one sentinel and points both `head_` and `tail_` at it; the destructor walks from `head_` deleting every remaining node (`src/qb/system/lockfree/mpsc_unbounded_queue.h:64-68`, `:70-77`). Each `push` adds a heap node; each `pop` frees the consumed one. This trades the ring buffer's zero-allocation steady state for unbounded capacity.

## The CPU layer underneath

`qb/system/cpu.h` supplies the two things the primitives above need from the hardware, plus a host-query class.

**`qb::spin_loop_pause()`** is the pause hint every wait loop on this page issues between reads: `_mm_pause` on x86, `yield` on AArch64 and 32-bit ARM, `__dmb` under MSVC on ARM64, and `std::this_thread::yield()` as a portable last resort (`src/qb/system/cpu.h:194-201` for x86, `:212-225` for ARM). It is `inline` and `noexcept` everywhere.

**`qb::CPU`** is a static-only, non-instantiable class of host queries — every special member is deleted (`src/qb/system/cpu.h:113-119`):

| Member | Returns |
|---|---|
| `Architecture()` | the CPU brand string |
| `Affinity()` | the number of logical processors available |
| `LogicalCores()` / `PhysicalCores()` / `TotalCores()` | core counts; `TotalCores()` is the `{logical, physical}` pair |
| `ClockSpeed()` | clock in Hz, or `-1` when unavailable |
| `HyperThreading()` | whether logical differs from physical |
| `ThreadPinningSupported()` | whether OS-level thread pinning actually takes effect on this host |

The last one deserves its own paragraph, because it exists to make a silent no-op observable. `qb::CoreInitializer::setAffinity()` is best-effort by design: a failed pin only warns and never fails `VirtualCore` init. On **Apple Silicon** it does not merely fail — macOS has no `pthread_setaffinity_np`, qb emulates one with `thread_policy_set(THREAD_AFFINITY_POLICY)`, and arm64 macOS does not implement that flavor: every call answers `KERN_NOT_SUPPORTED` (measured: Apple M4 Pro, Darwin 25.6.0, `ret == 46` for every core), and qb's shim deliberately reports *success* for that code so it does not warn on every core of every run. Pinning therefore does nothing there, silently. `ThreadPinningSupported()` is what lets a test or a user branch on that instead of assuming (`src/qb/system/cpu.h:159-189`).

It is determined once per process and cached, by a **runtime probe** on macOS rather than an `#ifdef __aarch64__` — because an x86_64 binary under Rosetta 2 runs on an arm64 kernel, so only a runtime probe gives the right answer. On other POSIX and on Windows it is `true`, meaning the *mechanism* exists; an out-of-range `CoreId` or a restrictive cgroup can still make an individual request fail. And `true` on macOS is a weaker guarantee than `true` on Linux or Windows even where the flavor is implemented: `<mach/thread_policy.h>` describes it as experimental and as a scheduler *hint* that groups threads sharing an affinity tag onto a shared L2, not a pin to CPU N.

Two RAII helpers also live in this header, for no reason other than that this is where they are declared: `qb::resource(handle, deleter)` wraps a raw handle in a `std::unique_ptr` with a custom deleter (`src/qb/system/cpu.h:46-50`), and `qb::scope_guard(callable)` runs a callable on scope exit unless `dismiss()`ed (`src/qb/system/cpu.h:74-80`). `scope_guard` is `[[nodiscard]]`, move-constructible (the moved-from guard dismisses itself) and neither copyable nor move-assignable.

## Pitfalls

- **Adding a second producer or consumer to an SPSC ring is undefined behavior, not a slowdown.** The indices are not CAS-protected; they assume one writer each. If you need many producers, use `mpsc::ringbuffer` (bounded) or `mpsc_unbounded_queue` (unbounded).
- **`consume_all` splits an item that spans the wrap.** Use the copy-out `dequeue(func, scratch, n)` overload whenever one logical item can occupy more than one slot (`src/qb/system/lockfree/spsc.h:275-283`).
- **The MPSC indexed enqueue overloads take no lock.** `enqueue<_Index>` and `enqueue(index, …)` are safe only when one thread owns that index. If multiple unbound threads must push, use the round-robin `enqueue(T const&)` overloads, which take the per-producer `SpinLock`. The two families share a name; confirm which one you are calling.
- **A non-trivially-copyable `T` will not compile in an SPSC/MPSC ring.** The `static_assert` is deliberate: the bulk paths `memcpy` (`src/qb/system/lockfree/spsc.h:52-53`). Store handles, pointers, or trivially-copyable structs; for arbitrary movable types use `mpsc_unbounded_queue`.
- **A spinlock under real contention wastes a core.** `SpinLock::lock()` never sleeps. Use it only for sections of a few instructions with rare contention; otherwise prefer `std::mutex`.
- **`mpsc_unbounded_queue::size()` over-estimates and can change between the call and the next line.** It is a hint for the consumer, never a loop bound (`src/qb/system/lockfree/mpsc_unbounded_queue.h:135-148`).
- **`mpsc_unbounded_queue` allocates on every push.** It is unbounded by design; if you need a fixed memory ceiling or zero steady-state allocation, the bounded `mpsc::ringbuffer` is the right tool.
- **`setAffinity` can be a silent no-op.** Ask `qb::CPU::ThreadPinningSupported()` before attributing a performance result to pinning (`src/qb/system/cpu.h:159-189`).
- **Do not reach for these in application code.** For inter-actor and inter-core messaging, use `push`, `send`, `reply`, and `broadcast` ([The event system](../2_core_concepts/event_system.md)). They enforce the threading contract for you and are the supported, type-checked path.

## See also

- [Foundations overview](./README.md) — the rest of the layer below the event loop.
- [The pipe](./buffers.md) — the single-threaded buffer that feeds these rings, and where the memory arithmetic for a mailbox lives.
- [The time vocabulary](./time.md) — `qb::duration` and `qb::mono_time`, which `SpinLock`'s timed methods take.
- [Threading model](../2_core_concepts/threading_model.md) — how a `VirtualCore` maps to a thread and how cross-core events ride the MPSC mailbox.
- [Core invariants](../7_reference/core_invariants.md) — the share-nothing contract and the mailbox back-pressure protocol.
- [The engine](../4_qb_core/engine.md) — `qb::Main`, `SharedCoreCommunication`, and core lifecycle.
- [API overview](../7_reference/api_overview.md) · [Glossary](../7_reference/glossary.md) — SPSC, MPSC, mailbox, and false-sharing definitions.
