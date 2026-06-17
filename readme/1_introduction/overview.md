# What qb is

> **Audience:** Evaluator · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

qb is a C++20-first framework with optional C++23 support for building concurrent and distributed systems on the actor model, composed of two libraries: an actor engine (`qb-core`) layered on a standalone non-blocking asynchronous I/O runtime (`qb-io`).

**Prerequisites:** none — **See also:** [Design philosophy](./philosophy.md) · [Core concepts](../2_core_concepts/README.md) · [Getting started](../6_guides/getting_started.md)

## Summary

qb pairs share-nothing actors with a non-blocking I/O engine and native C++20 coroutines. Application code expresses *what* should happen on each message; the runtime owns scheduling, multicore distribution, and non-blocking I/O. Each actor owns its state and communicates only by passing events, which the engine delivers in order and processes one at a time per actor — so whole classes of data races and deadlocks cannot occur by construction.

The framework ships as two libraries that compose:

- **`qb-io`** — the asynchronous runtime: a libev-based event loop, non-blocking TCP, UDP, and SSL/TLS transports, an extensible protocol layer, C++20 coroutines, timers, filesystem watching, and shared utilities (canonical time vocabulary, lock-free queues, cryptography, compression, containers). It is usable on its own, with no dependency on the actor engine.
- **`qb-core`** — the actor engine built on `qb-io`: lightweight actors, a type-safe event system with ordered delivery, multicore scheduling with optional CPU affinity, and lock-free inter-core message passing.

Higher-level application protocols (HTTP/1.1 and HTTP/2, WebSocket, PostgreSQL, Redis) are provided as optional [qbm modules](../../README.md#module-ecosystem) built on this foundation, not as part of the core distribution.

## The two libraries

| Library | Umbrella headers | Role | Standalone? |
|---|---|---|---|
| `qb-io` | `qb/io.h`, `qb/io/async.h` | Event loop, sockets and transports, protocols, coroutines, time, crypto, compression, containers | Yes |
| `qb-core` | `qb/actor.h`, `qb/main.h`, `qb/event.h` | Actors, events, the `Main` engine, `VirtualCore` worker threads, inter-core messaging | No — depends on `qb-io` |

Both are exposed to CMake as the aliases `qb::io` and `qb::core`. Link only `qb::io` when you need the asynchronous runtime without the actor model; link `qb::core` (which brings `qb::io` transitively) for the full engine.
<!-- src: docs-overhaul/qb/FACTBOOK.md:152-154 -->

## The problem qb addresses

Concurrent C++ written with raw threads, mutexes, and condition variables forces every component to reason about shared mutable state. Correctness then depends on a global discipline — lock ordering, memory visibility, and lifetime — that no compiler enforces and no local review can fully verify. Blocking I/O compounds the cost: a thread parked on a `read()` or `connect()` syscall is a worker that does no useful work while it waits.

qb removes both sources of difficulty from application code:

- **No shared mutable state across concurrency boundaries.** An actor's state is reachable only from that actor, and an actor runs on exactly one worker thread. Components communicate by sending events, never by sharing pointers into each other's data. There are no application-level mutexes to order and no condition variables to manage.
- **No blocking on I/O.** Network, timer, and filesystem operations run on a non-blocking event loop. A worker stays busy processing actor logic and ready events instead of waiting on a syscall.

The result is code where each actor's message handler can be written and reasoned about as if it were single-threaded, because — from the actor's point of view — it is.

## Who qb is for

qb targets C++ engineers building servers, network services, simulations, real-time pipelines, and other systems where concurrency and I/O are central. It assumes you are comfortable with:

- Modern C++ on a **C++20** baseline, with an opt-in C++23 path. The build sets `QB_CXX_STANDARD=20` by default, keeps compiler extensions off, and propagates the selected standard to consumers. Coroutines and `std::optional` appear where the API exposes them.
- Core concurrency concepts — threads, asynchronous operations, and the failure modes (data races, deadlocks) that the actor model is designed to prevent.
- TCP/UDP network programming at a basic level.
- CMake for integrating and building dependencies.

The actor model removes most low-level synchronization from your code, but a working understanding of these areas helps when reasoning about throughput, latency, and core placement.
<!-- src: docs-overhaul/qb/FACTBOOK.md:368 -->

## What qb provides

### Actor engine (`qb-core`)

- **`qb::Actor`** — the base class for message-driven components. Lifecycle hooks are `bool onInit()` (called once when the actor is registered; return `false` to abort startup) and `kill()` to terminate.
- **Type-safe events.** Define an event by deriving from `qb::Event`; deliver it with `push<Event>(...)` (the ordered default) or `broadcast<Event>(...)`. Subscribe with `registerEvent<Event>(*this)` and handle it with a matching `on(const Event&)` overload.
- **The `Main` engine.** `qb::Main` (aliased `qb::engine`) configures cores, spawns one `VirtualCore` worker thread per core, and manages `start()`, `stop()`, and `join()`. Actors are placed on a specific core via `addActor<T>(core, args...)`.
- **Multicore distribution.** Each `VirtualCore` owns its actors and runs its own event loop. Cross-core events travel over a per-core lock-free MPSC (multiple-producer, single-consumer) mailbox; an actor is thread-affine and never migrates between cores.

### Asynchronous runtime (`qb-io`)

- **Event loop.** `qb::io::async::listener` is a thread-local, libev-backed event loop. One runs per `VirtualCore`; it can also be driven directly in a plain thread when you use `qb-io` standalone.
- **Transports.** Non-blocking TCP, UDP, Unix-domain sockets, and SSL/TLS, with QUIC/HTTP3 available when built with the optional ngtcp2 backend.
- **Protocols.** An extensible protocol layer (`qb::io::async::AProtocol`) frames and parses your wire format; the `qb::io::use<>` CRTP helpers compose client and server roles with minimal boilerplate.
- **Coroutines.** A C++20/23 coroutine layer (`co_await`/`co_return`) integrates with the event loop, so sequential-looking async flows compile to state machines rather than threads.
- **Timers and callbacks.** `qb::io::async::callback` and `qb::io::async::with_timeout` schedule time-based work on the loop.

### Shared utilities

- **Canonical time vocabulary** — `qb::duration` (a `std::chrono::nanoseconds` span), `qb::mono_time` (steady-clock time point), and `qb::wall_time` (system-clock time point), with helpers in `qb/system/timestamp.h`.
- **URI parsing** — `qb::io::uri`.
- **Cryptography** — hashing, encryption, and key utilities, available when built with OpenSSL (`QB_WITH_SSL`, on by default).
- **Compression** — gzip and deflate, available when built with zlib (`QB_WITH_COMPRESSION`, on by default).
- **Containers** — `qb::string<N>`, `qb::unordered_map`, the `qb::allocator::pipe` buffer, and lock-free queue primitives under `qb::lockfree`.

## A first actor

The following program defines one actor that sends a message to itself, prints it, and terminates. It uses no mutex, condition variable, or shared queue.

```cpp
// src: qb/README.md (Quick start)
#include <qb/main.h>
#include <qb/actor.h>
#include <qb/io.h>

struct GreetingEvent : qb::Event {
    qb::string<64> message;
    explicit GreetingEvent(const char *msg) : message(msg) {}
};

class GreeterActor : public qb::Actor {
public:
    bool onInit() final {
        registerEvent<GreetingEvent>(*this);   // subscribe
        push<GreetingEvent>(id(), "Hello");     // send to self
        return true;                            // actor is ready
    }

    void on(const GreetingEvent &event) {
        qb::io::cout() << "Received: " << event.message << '\n';
        kill();                                 // work done; terminate this actor
    }
};

int main() {
    qb::Main engine;
    engine.addActor<GreeterActor>(0);           // run on core 0
    engine.start();                             // start the engine
    engine.join();                              // block until all actors stop
    return 0;
}
```

The engine delivers events in order and processes them one at a time per actor, so `on(const GreetingEvent&)` never runs concurrently with itself.

## When to use qb

qb fits when most of the following hold:

- The workload is concurrent and **I/O-bound or message-driven** — network servers, protocol gateways, real-time pipelines, simulations, multiplayer backends.
- You want to **scale across cores** from the same code, by placing actors on cores rather than rewriting around threads.
- You prefer **message passing over shared state** as the concurrency model, and want the language to make data races structurally unlikely.
- You need **non-blocking I/O** with predictable latency under load.
- A **C++20** toolchain is available across your build matrix, with C++23 available where you want the newer path.

## When not to use qb

qb is not the right tool when:

- The work is a **single-threaded, synchronous program** with no concurrency or I/O concurrency. The actor runtime adds machinery you would not use.
- The work is **CPU-bound, embarrassingly parallel batch computation** (for example, numerical kernels) better served by a data-parallel or task-graph library than by a message-passing runtime.
- You **cannot adopt C++20**. The framework does not support C++17 as a minimum anymore.
- You need **blocking, request-per-thread I/O** semantics and do not want an event loop. qb is built around non-blocking I/O; forcing a blocking style works against the design.
- Your platform or compiler is outside the [supported matrix](../../README.md#platform-support).

For a deeper treatment of the design choices behind these trade-offs, see [Design philosophy](./philosophy.md).

## See also

- [Design philosophy](./philosophy.md) — the principles behind the actor model, async I/O, and the modular split.
- [Core concepts](../2_core_concepts/README.md) — actors, events, the asynchronous model, and the threading model.
- [qb-io](../3_qb_io/README.md) — the asynchronous runtime in depth.
- [qb-core](../4_qb_core/README.md) — the actor engine and messaging.
- [Getting started](../6_guides/getting_started.md) — build and run your first qb application.
