# Core concepts

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.6.0 (C++20 default, C++23 supported)

The foundational model behind qb: isolated actors, the events they exchange, the non-blocking I/O loop that drives them, and the threading that runs them in parallel.

**Prerequisites:** [Introduction](../1_introduction/README.md) — **See also:** [qb-io module](../3_qb_io/README.md), [qb-core module](../4_qb_core/README.md)

## Summary

qb is built on a small set of ideas that reinforce each other. An **actor** is an isolated unit of state and behavior; it never shares memory with another actor and communicates only by passing **events**. Each actor lives on exactly one **`VirtualCore`** worker thread, which drains the actor's mailbox one event at a time on top of a single-threaded, libev-backed **asynchronous I/O** loop. Because actors are thread-affine and share nothing, the only genuinely multi-threaded surface in the system is the lock-free message-passing layer between cores.

The five pages below explain these ideas one at a time. Read them in order the first time through: each builds on the previous. `concurrency.md` and `threading_model.md` are deliberately split — the first describes the programming model you write against (the actor as the unit of concurrency), the second describes the runtime mechanics underneath it (one thread per core, mailboxes, message flushing).

## Pages in this section

| Page | What it covers |
| --- | --- |
| [The actor model in qb](./actor_model.md) | What an actor is, how `qb::Actor` and `qb::ActorId` work, the `onInit` / `on` / `kill` lifecycle, and why isolated state needs no locks. |
| [The event system](./event_system.md) | Defining events on `qb::Event`, the delivery primitives (`push`, `broadcast`, and friends), and the ordering and lifetime guarantees the runtime makes. |
| [The asynchronous I/O model](./async_io.md) | The single-threaded, libev-backed event loop, the `listener`, `on()` readiness handlers, async callbacks, and the coroutine layer for `co_await`-style flows. |
| [Concurrency in qb](./concurrency.md) | The actor as the unit of concurrency: state ownership, sequential mailbox processing, and why application code never reaches for a mutex. |
| [The threading model](./threading_model.md) | One `VirtualCore` worker thread per engine core, private lock-free mailboxes, thread affinity, and the cross-core message-passing boundary. |

## Suggested reading order

1. **[The actor model in qb](./actor_model.md)** — start here; everything else assumes you know what an actor is.
2. **[The event system](./event_system.md)** — how actors talk to each other.
3. **[The asynchronous I/O model](./async_io.md)** — what runs the actors and their I/O between events.
4. **[Concurrency in qb](./concurrency.md)** — the model you write against.
5. **[The threading model](./threading_model.md)** — the runtime mechanics that make the model safe and parallel.

## Next steps

Once these concepts are familiar, continue with the module documentation:

- The **[qb-io module](../3_qb_io/README.md)** for the standalone asynchronous I/O library — event loop, sockets, transports, protocols, and coroutines.
- The **[qb-core module](../4_qb_core/README.md)** for the actor engine — `qb::Main`, `qb::VirtualCore`, the `qb::Actor` API, and inter-core messaging.
- The **[Getting started guide](../6_guides/getting_started.md)** if you have not yet built and run a first actor.
