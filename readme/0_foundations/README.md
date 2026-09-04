# Foundations

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.1.0 (C++20 default, C++23 supported) — ef7d3ea7

The layer beneath the event loop: the vocabulary qb adds to the standard library, and the machinery `qb-io` and `qb-core` are built out of. Everything here lives in `qb/system/` and `qb/utility/` and depends on nothing else in the framework.

**Prerequisites:** none — **See also:** [qb-io](../3_qb_io/README.md) · [qb-core](../4_qb_core/README.md) · [Core invariants](../7_reference/core_invariants.md)

## Two readers, one tier

This tier serves two people, and the split is clean enough to draw. Some of it is **public vocabulary** you write against on your first day — `qb::duration` is in the signature of every timeout in the framework. The rest is **machinery**: you will never name it, but the rules it imposes on your code are the ones that bite hardest, and they make no sense until you have seen the shape they come from.

| | Type or facility | Page | You will… |
|---|---|---|---|
| **Public surface** | `qb::duration`, `qb::mono_time`, `qb::wall_time`, `qb::date`, `qb::time_of_day`, `qb::calendar_interval` | [time.md](./time.md) | write these in your own signatures, constantly |
| | `qb::string<N>` | [containers.md](./containers.md) | put one in every event that carries text |
| | `qb::unordered_map` / `unordered_set` and the `icase_*` wrappers | [containers.md](./containers.md) | use them wherever you would reach for `std::unordered_map` |
| | `qb::to_number`, `qb::endian`, `qb::uuid` | [encoding.md](./encoding.md) | reach for these the first time you parse or emit a wire format |
| **Machinery** | `qb::allocator::pipe<T>` | [buffers.md](./buffers.md) | never name it — but the `push` reference rule is its cursor model |
| | `spsc::ringbuffer`, `mpsc::ringbuffer`, `SpinLock`, `qb::CPU` | [concurrency_primitives.md](./concurrency_primitives.md) | use these through `push`/`send`, not directly |
| | `abi.h`, `prefix.h`, `CacheLine`, `EventBucket` | [abi_and_build_fingerprint.md](./abi_and_build_fingerprint.md) | meet this the day a link fails with a symbol you have never seen |

Read the public-surface rows before you write your first actor. Read the machinery rows before you contribute to the engine — or the day one of its rules surprises you and you want the reason rather than the rule.

## The pages

| Page | What it owns |
|---|---|
| [The time vocabulary](./time.md) | `qb::duration` / `mono_time` / `wall_time`, the civil calendar types, the UTC codecs, the measurement helpers, and the one seam where time degrades to a `double`. The two compile-time rejections that make the vocabulary worth having. |
| [The pipe](./buffers.md) | `qb::allocator::pipe<T>`: the `_begin`/`_end`/`_capacity` cursor model and the four operations on it, why compaction is more dangerous than reallocation, `pipe<char>::put<T>` as the framework-wide serialisation extension point, and why the engine's memory only grows. |
| [Containers](./containers.md) | `qb::unordered_map` / `unordered_set` node stability and what bug classes it does and does not remove, the flat variants, the case-insensitive wrappers, `qb::string<N>`, and `qb::ring_buffer`. |
| [Concurrency primitives](./concurrency_primitives.md) | The SPSC and MPSC rings, the unbounded MPSC queue, `SpinLock`, and the per-primitive threading contract. Plus `qb/system/cpu.h`: `spin_loop_pause`, the host queries, and whether thread pinning does anything on this machine. |
| [Encoding and conversion](./encoding.md) | `qb::to_number` / `to_number_prefix`, `qb::endian`, `qb::uuid`. |
| [ABI and the build fingerprint](./abi_and_build_fingerprint.md) | `qb/utility/abi.h`, the cache-line constant and how it reaches the 1023-bucket event ceiling, `QB_ABI_ANCHOR`, and the rest of `qb/utility/`. |

## What is *not* here

Two things sit one layer up and are documented there, not here:

- **Timers, `sleep`, `callback` and the loop's cached "now"** are `qb-io` facilities that *take* `qb::duration`. They live in [the async I/O system](../3_qb_io/async_system.md); this tier owns only the type they speak.
- **URI parsing, cryptography, JWT, compression and JSON** are batteries `qb-io` ships and links; they need OpenSSL, zlib or nlohmann, and they are on [qb-io utilities](../3_qb_io/utilities.md).

The dividing line is a dependency, not a judgement: everything on this tier is header-only or backed by the compiled library with no third-party dependency at all, and none of it needs an event loop, a socket, or an actor to be useful.

## Four facts worth carrying out of this tier

If you read nothing else here, these four are the ones that change how you write code.

1. **`qb::duration` is `std::chrono::nanoseconds`, and a bare integer does not convert to it.** `setLatency(500)` is a compile error, not a 500-of-something. See [time.md](./time.md#qbduration-rejects-a-bare-integer).
2. **`qb::mono_time` and `qb::wall_time` are different types, and subtracting one from the other does not compile.** That kills the whole "the timeout fired early because NTP stepped the clock" family at build time. See [time.md](./time.md#mono_time-and-wall_time-do-not-mix).
3. **A pointer into a `pipe<char>` dies at the next allocation on that pipe** — including the in-place compaction case, which stays inside a live allocation and therefore no allocator debugger can see. The event pipes are `segmented_pipe`s precisely so that an event never moves: the `Actor::push` reference lives until your handler returns. See [buffers.md](./buffers.md#what-allocate_back-actually-does) and [its Events section](./buffers.md#events).
4. **There is no lock on the message path.** Not a fast one, not an uncontended one: the engine calls the *indexed* MPSC enqueue, and the only `SpinLock` in the framework sits in the round-robin overloads the engine never uses. See [concurrency_primitives.md](./concurrency_primitives.md#two-enqueue-families--read-this-before-using).

## See also

- [qb-io](../3_qb_io/README.md) — the async runtime built directly on this tier.
- [qb-core](../4_qb_core/README.md) — the actor engine built on `qb-io`.
- [Core invariants](../7_reference/core_invariants.md) — the contracts these mechanisms produce, stated as invariants.
- [Building qb](../7_reference/building.md) — the build options the ABI axes read.
