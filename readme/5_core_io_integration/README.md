# Integrating core actors with asynchronous I/O

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.1.0 (C++20 default, C++23 supported)

How `qb-core` actors use the `qb-io` event loop to perform network, timer, and file work without blocking their `VirtualCore`.

**Prerequisites:** [Core concepts: the actor model](../2_core_concepts/actor_model.md), [Reference: `qb-io` async system](../3_qb_io/async_system.md) — **See also:** [qb-core module](../4_qb_core/README.md), [qb-io module](../3_qb_io/README.md)

## Summary

`qb-core` and `qb-io` are not two libraries bolted together; they share one execution context. Each `qb::VirtualCore` runs a single thread that drives exactly one `qb::io::async::listener` event loop, and that loop dispatches both actor messages and I/O events. An actor that reads a socket, arms a timer, or watches a directory does so on the same loop that delivers its `on(Event&)` handlers — never on a background thread, never behind a lock.

Three properties follow from that shared loop, and every page in this section is an application of them:

- **Non-blocking by construction.** I/O is initiated without blocking; the loop calls an `on(...)` handler when data is ready or an operation completes. A handler that blocks — a synchronous `read`, a `sleep`, a mutex wait — stalls every actor and every pending I/O event on that core.
- **Unified scheduling.** Deferred continuations (`qb::io::async::defer`) and timers (`qb::io::async::callback` with a positive delay, `scoped_callback`, the `with_timeout<T>` mixin) run on the same listener that dispatches messages, in the same single-threaded context.
- **Thread affinity, not locks.** Every I/O object created on a core's loop is bound to that core. Cross-core work travels as events (`push`/`broadcast`) or as a moved-in socket, never as a shared pointer to live I/O state.

This section breaks the integration into two task pages and a set of worked example analyses.

## Pages in this section

| Page | What it covers |
|---|---|
| [Asynchronous work inside an actor](./async_in_actors.md) | The two call chains — `co_await` versus `run_sync` — annotated against the loop pass, so it is visible which one gives the core back. Then the primitives: deferred continuations (`qb::io::async::defer`), delayed callbacks (`qb::io::async::callback`, `scoped_callback`), inactivity timers (`with_timeout<T>`), coroutines (`Actor::spawn` / `spawn_detached`), periodic work (`qb::ICallback`), and patterns for keeping blocking file I/O off the loop. |
| [Building network actors](./network_actors.md) | Turning an actor into a non-blocking TCP, UDP, or SSL/TLS endpoint with the `qb::io::use<Self>` mixins — clients, servers, acceptors, session pools, and cross-core socket transfer. |
| [Case studies: example analyses](./examples/README.md) | Walkthroughs of three complete applications (`01-tcp-chat`, `03-file-pipeline`, `02-pubsub-broker`) that combine `qb-core` and `qb-io` end to end. |

## Suggested reading order

1. **[Asynchronous work inside an actor](./async_in_actors.md)** — start here. It establishes the single-thread-per-core contract and the timer, coroutine, and offloading mechanisms that the networking page and the examples all rely on.
2. **[Building network actors](./network_actors.md)** — applies those mechanisms to network endpoints via `qb::io::use<>`, including the split acceptor / session-manager architecture used by the larger examples.
3. **[Case studies: example analyses](./examples/README.md)** — read after the two task pages. Each analysis maps the concepts above onto a runnable program; the `01-tcp-chat` and `02-pubsub-broker` studies are the natural follow-ups to the networking page, while `03-file-pipeline` extends the blocking-I/O pattern from the async page.

## See also

- [Reference: `qb-io` async system](../3_qb_io/async_system.md) — the listener, timers, and watchers in full.
- [Reference: C++20 coroutines](../3_qb_io/coroutines.md) — `task<T>`, awaiters, and combinators used by `spawn` / `spawn_detached`.
- [Reference: `qb::Actor`](../4_qb_core/actor.md) — actor lifecycle, `qb::ICallback` registration, and the messaging API the callbacks reach back through.
- [Foundations: the time vocabulary](../0_foundations/time.md) — `qb::duration`, `qb::mono_time`, and `qb::wall_time`, the canonical span and time-point types every timeout argument uses.
