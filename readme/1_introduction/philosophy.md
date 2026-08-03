# Design philosophy

> **Audience:** Evaluator · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

This page explains the design principles behind qb — share-nothing actor isolation, asynchronous-by-default I/O, a layered and modular architecture, explicit modern C++20/23, and lock-free inter-core messaging — and the rationale for each choice.

**Prerequisites:** none — **See also:** [What qb is](./overview.md) · [Core concepts](../2_core_concepts/README.md) · [Concurrency in qb](../2_core_concepts/concurrency.md)

## Summary

qb is built on a small set of decisions that reinforce one another. Actors own their state and never share memory, so data races are structurally absent rather than defended against. I/O is non-blocking by default, so a worker thread spends its time computing instead of waiting on syscalls. The framework is two composable libraries — `qb-io` (the asynchronous runtime) and `qb-core` (the actor engine) — so you can adopt only what you need. The public API is modern, explicit C++20 with CRTP-based static polymorphism and a `std::chrono` time vocabulary, so abstractions cost little at runtime and lifetimes are clear. Inter-core messages move over lock-free queues, so adding cores adds throughput without adding locks.

Each principle below states the choice, then the reason it was made.

## Share-nothing actor isolation

An **actor** is a self-contained unit of computation with a unique identity (`qb::ActorId`) that holds private state and communicates only by passing events. It is the central abstraction of `qb-core`. You compose a system from actors instead of orchestrating raw threads, mutexes, and condition variables.

Three properties define the model:

- **Private state.** Each actor owns its data. No other actor accesses that data directly; the only way to affect an actor is to send it an event.
- **Sequential processing.** An actor processes the events in its mailbox one at a time. Within a single actor you write ordinary single-threaded code — there is no internal locking, because no two handlers for the same actor run concurrently.
- **Message-passing only.** Actors interact by exchanging asynchronous events, never by reading or writing shared memory.

**Why share-nothing.** Data races and deadlocks come from concurrent access to shared mutable state. If actors never touch one another's memory and each actor handles its mailbox sequentially, those failure modes cannot occur by construction — they are designed out, not guarded against at runtime. The result is code that is simpler to reason about (one actor at a time, like a single-threaded program), more resilient (a failure tends to be contained in one actor), and naturally testable (an isolated actor is exercised by feeding it events).

The framework holds this line in code: all event handlers (`on(EventType&)`) for the actors on a given worker thread run sequentially on that thread, so actor state needs no mutexes or atomics (`src/qb/io/async.h:20`).

**See:** [The actor model](../2_core_concepts/actor_model.md) · [Actor reference](../4_qb_core/actor.md) · [Event messaging](../4_qb_core/messaging.md)

## Asynchronous by default

A blocking I/O call halts the calling thread until the operation completes. Under load, threads that block on the network or disk waste a core that could be doing useful work. `qb-io` is built around a non-blocking, event-driven runtime so that no actor handler blocks waiting on I/O.

- **Non-blocking transports.** TCP, UDP, and SSL/TLS are initiated without stalling the caller. Control returns immediately, and the thread proceeds to its next unit of work — typically another actor's event.
- **Filesystem integration.** File descriptors are blocking, so filesystem access is integrated through watchers (`file_watcher`, `directory_watcher`) and by offloading blocking reads, keeping the loop responsive rather than treating file I/O as a non-blocking transport.
- **Kernel event notification.** The event loop is built on libev, which uses the most efficient readiness mechanism the platform provides (epoll on Linux, kqueue on the BSDs and macOS, and a wepoll/IOCP backend on Windows). When a socket becomes readable or writable, the loop is notified rather than polled.
- **One loop per worker.** Each worker thread runs its own thread-local event loop (`qb::io::async::listener`). I/O readiness is dispatched to the handler that registered for it, on the same thread, so I/O state — like actor state — needs no synchronization.

**Why async by default.** Throughput and tail latency under load depend on keeping cores busy with work rather than parked in a blocking syscall. An event-driven loop lets one thread service many connections and timers, so the cost of a slow or idle peer is a suspended operation, not a stranded thread.

For sequential-looking async code, `qb-io` integrates C++20 coroutines with the same loop: `co_await` suspends a coroutine until an awaited event is ready and resumes it on the loop thread, without spawning a thread per logical task. See [coroutines](../3_qb_io/coroutines.md).

**See:** [Asynchronous I/O model](../2_core_concepts/async_io.md) · [The async system](../3_qb_io/async_system.md)

## Layered, modular architecture

qb is not a monolith. It ships as two libraries with a deliberate dependency direction:

- **`qb-io`** is a standalone asynchronous I/O library: the event loop, sockets (TCP/UDP/SSL/QUIC), transports, an extensible protocol layer, coroutines, timers, and utilities. It depends on `qb-core` for nothing and can be used entirely on its own when your needs are purely I/O-focused (`src/qb/io.h`).
- **`qb-core`** is the actor runtime — `qb::Main`, `qb::VirtualCore`, `qb::Actor`, `qb::Event` — layered on top of `qb-io`.

Within that structure, extension points are explicit rather than hidden:

- **Custom protocols.** The `qb::io::async::AProtocol<IO_>` interface defines exactly how bytes on a connection are framed into and parsed from your application's messages, so you control wire formats without modifying the transport.
- **Clear actor surface.** `qb::Actor` extends through documented virtual methods — `onInit()` for setup — and a typed event mechanism (`on(EventType&)`, `registerEvent<EventType>(*this)`).

**Why a layered split.** Separating the I/O runtime from the actor engine lets the framework fit diverse projects: an evaluator can take `qb-io` for an async network service without buying into the actor model, and an adopter can replace or extend the protocol and transport layers to match a specific system. Each layer has one responsibility, which keeps both easier to reason about and to test.

**See:** [qb-io overview](../3_qb_io/README.md) · [qb-core overview](../4_qb_core/README.md) · [Protocols](../3_qb_io/protocols.md)

## Explicit, modern C++20/23

qb targets C++20 by default, supports C++23 explicitly, and uses the language directly rather than hiding it behind a runtime. The design favors abstractions that the compiler can resolve, and lifetimes that the type system can express.

- **Static polymorphism via CRTP.** I/O building blocks are assembled with the Curiously Recurring Template Pattern — the `qb::io::use<>` helper composes transport, protocol, and handler behavior into a derived type at compile time. Dispatch is resolved statically, so the high-level API carries little or no virtual-call overhead on the hot path.
- **RAII lifetimes.** Resources are tied to object lifetime, so cleanup is deterministic and tied to scope rather than manual bookkeeping.
- **One `std::chrono` time vocabulary.** Durations and time points are expressed with `qb::duration` (a `std::chrono::nanoseconds` span), `qb::mono_time` (a `steady_clock` time point), and `qb::wall_time` (a `system_clock` time point), defined in `src/qb/system/time.h`. Timeouts, latencies, and delays across the framework take `qb::duration`, so units are checked by the type system rather than passed as bare numbers.
- **`std::filesystem::path` for filesystem paths.** Anything that names a file or directory on the local filesystem — `sys::file::open`, the file/directory watchers, the TLS certificate/key/CA/DH helpers, the HTTP static-file root — takes a `std::filesystem::path`, so Unicode paths work correctly across platforms (Windows opens them via the wide-character APIs). The type also marks intent: paths that are *not* local filesystem locations — URLs, URIs, route patterns, and remote/wire paths handled by the other side — stay `std::string`. Local resource paths resolve through `qb::io::sys::resolve_resource()`, which looks up a relative path against the working directory and then the executable's own directory, so a binary shipped alongside its assets is self-locating and runs from any working directory (`src/qb/io/system/file.h`).
- **Type-safe events.** Events are ordinary types; the compiler checks that a handler exists for the event it is dispatched, moving a class of routing errors to compile time.

**Why explicit and modern.** Generic, value-semantic, RAII-based C++ lets the framework offer developer-facing abstractions whose runtime cost is visible and small. A single, typed time vocabulary removes a recurring source of bugs — mixed units and ambiguous "is this seconds or milliseconds" parameters — by making the unit part of the type.

**See:** [The use<> helper and async system](../3_qb_io/async_system.md) · [Time utilities](../3_qb_io/utilities.md)

## Performance through lock-free messaging

qb is engineered for systems where latency and throughput matter. Its performance posture follows from the model rather than from micro-optimizations bolted on top.

- **Multicore by placement.** Actors are distributed across `qb::VirtualCore` worker threads — one thread per core — for true parallelism. You assign actors to cores and pin those cores to CPUs with `qb::CoreIdSet` affinity (`engine.core(0).setAffinity(qb::CoreIdSet{2})`), which lets you keep related work on the same core for cache locality.
- **Lock-free inter-core mailboxes.** When an event crosses from one core to another, it travels through a per-core multi-producer, single-consumer (MPSC) lock-free ring buffer (`src/qb/system/lockfree/mpsc.h`). The buffer is built from per-producer SPSC sub-rings, so producers on different cores never contend on the same ring, and the single owning consumer drains them all. Adding cores adds queues, not lock contention.
- **Ordered and unordered delivery.** `push<Event>()` delivers in FIFO order relative to other pushes from the same source to the same destination (`src/qb/core/Pipe.h:118`); `send<Event>()` is unordered, fire-and-forget delivery restricted to trivially-destructible events. Choosing the weaker guarantee where ordering is not needed avoids paying for it.
- **Low-overhead buffers.** Resizable I/O and event buffers (`qb::allocator::pipe`) reduce allocation and copying on the data path.

**Why lock-free.** Locks serialize the threads that contend on them; under load a contended lock becomes the bottleneck that caps scaling. Routing cross-core messages through lock-free MPSC queues lets each worker enqueue without blocking another, so throughput grows with cores instead of stalling on a shared lock. For workloads in finance, gaming, real-time data, and similar domains, that difference is the point.

This page describes the rationale; for the mechanism — how cores are scheduled, how mailboxes are drained, and how idle cores back off — see the threading model.

**See:** [The threading model](../2_core_concepts/threading_model.md) · [Lock-free primitives](../7_reference/lockfree_primitives.md) · [Performance tuning](../6_guides/performance_tuning.md)

## How the principles fit together

The choices are mutually reinforcing rather than independent. Share-nothing isolation is what makes lock-free messaging sufficient: because actors never share memory, the only cross-thread traffic is the message queue, and a lock-free queue is enough to carry it. Asynchronous-by-default I/O is what keeps a worker thread productive between messages, so the per-core sequential model does not become a per-core bottleneck. The layered architecture lets each of these be adopted at the level you need, and modern C++20/23 is the medium that makes the abstractions cheap and the lifetimes clear. Taken together, they aim at one outcome: concurrent systems that are correct by construction and fast by design.

## See also

- [What qb is and when to use it](./overview.md)
- [The actor model](../2_core_concepts/actor_model.md)
- [Asynchronous I/O model](../2_core_concepts/async_io.md)
- [Concurrency in qb](../2_core_concepts/concurrency.md)
- [The threading model](../2_core_concepts/threading_model.md)
- [Getting started guide](../6_guides/getting_started.md)
