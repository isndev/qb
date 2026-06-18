# Integrating core actors with asynchronous I/O

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

How `qb-core` actors use the `qb-io` event loop to perform network, timer, and file work without blocking their `VirtualCore`.

**Prerequisites:** [Core concepts: the actor model](../2_core_concepts/actor_model.md), [Reference: `qb-io` async system](../3_qb_io/async_system.md) — **See also:** [qb-core module](../4_qb_core/README.md), [qb-io module](../3_qb_io/README.md)

## Summary

`qb-core` and `qb-io` are not two libraries bolted together; they share one execution context. Each `qb::VirtualCore` runs a single thread that drives exactly one `qb::io::async::listener` event loop, and that loop dispatches both actor messages and I/O events. An actor that reads a socket, arms a timer, or watches a directory does so on the same loop that delivers its `on(Event&)` handlers — never on a background thread, never behind a lock.

Three properties follow from that shared loop, and every page in this section is an application of them:

- **Non-blocking by construction.** I/O is initiated without blocking; the loop calls an `on(...)` handler when data is ready or an operation completes. A handler that blocks — a synchronous `read`, a `sleep`, a mutex wait — stalls every actor and every pending I/O event on that core.
- **Unified scheduling.** Timers and deferred work scheduled with `qb::io::async::callback`, `scoped_callback`, or the `with_timeout<T>` mixin run on the same listener that dispatches messages, in the same single-threaded context.
- **Thread affinity, not locks.** Every I/O object created on a core's loop is bound to that core. Cross-core work travels as events (`push`/`broadcast`) or as a moved-in socket, never as a shared pointer to live I/O state.

This section breaks the integration into two task pages and a set of worked example analyses.

## Pages in this section

| Page | What it covers |
|---|---|
| [Asynchronous operations inside actors](./async_in_actors.md) | Deferred callbacks (`qb::io::async::callback`, `scoped_callback`), inactivity timers (`with_timeout<T>`), coroutines (`Actor::spawn_async`), periodic work (`qb::ICallback`), and patterns for keeping blocking file I/O off the loop. |
| [Building network actors](./network_actors.md) | Turning an actor into a non-blocking TCP, UDP, or SSL/TLS endpoint with the `qb::io::use<Self>` mixins — clients, servers, acceptors, session pools, and cross-core socket transfer. |
| [Case studies: example analyses](./examples/README.md) | Walkthroughs of five complete applications (`chat_tcp`, `distributed_computing`, `file_monitor`, `file_processor`, `message_broker`) that combine `qb-core` and `qb-io` end to end. |

## Suggested reading order

1. **[Asynchronous operations inside actors](./async_in_actors.md)** — start here. It establishes the single-thread-per-core contract and the timer, coroutine, and offloading mechanisms that the networking page and the examples all rely on.
2. **[Building network actors](./network_actors.md)** — applies those mechanisms to network endpoints via `qb::io::use<>`, including the split acceptor / session-manager architecture used by the larger examples.
3. **[Case studies: example analyses](./examples/README.md)** — read after the two task pages. Each analysis maps the concepts above onto a runnable program; the `chat_tcp` and `message_broker` studies are the natural follow-ups to the networking page, while `file_processor` and `file_monitor` extend the blocking-I/O and watcher patterns from the async page.

## See also

- [Reference: `qb-io` async system](../3_qb_io/async_system.md) — the listener, timers, and watchers in full.
- [Reference: C++20 coroutines](../3_qb_io/coroutines.md) — `task<T>`, awaiters, and combinators used by `spawn_async`.
- [Reference: `qb::Actor`](../4_qb_core/actor.md) — actor lifecycle, `qb::ICallback` registration, and the messaging API the callbacks reach back through.
- [Reference: time utilities](../3_qb_io/utilities.md) — `qb::duration`, `qb::mono_time`, and `qb::wall_time`, the canonical span and time-point types every timeout argument uses.
