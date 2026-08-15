# Case studies: core and I/O integration examples

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Three worked analyses of complete applications that combine `qb-core` actors with `qb-io` networking and deferred work end to end.

**Prerequisites:** [Integrating core actors with asynchronous I/O](../README.md), [Asynchronous operations inside actors](../async_in_actors.md), [Building network actors](../network_actors.md) — **See also:** [qb-core module](../../4_qb_core/README.md), [qb-io module](../../3_qb_io/README.md)

## Summary

The two task pages in this section — [async operations inside actors](../async_in_actors.md) and [building network actors](../network_actors.md) — describe the mechanisms in isolation. These case studies show them composed into runnable programs. Each analysis breaks down one example from the repository: its actor topology, the `qb-io` facilities it drives, the events that travel between actors, and the design pattern it illustrates.

Every analysis is grounded in source you can build and run, and all three now live under `examples/05-services/`: `01-tcp-chat/`, `02-pubsub-broker/` and `03-file-pipeline/`. One of the three is **Unix-only** — `examples/05-services/CMakeLists.txt:50-52` wraps the file pipeline in `if (NOT QB_PLATFORM_WINDOWS)`, so it is not configured on Windows. Read an analysis alongside its source rather than as a substitute for it.

**A fourth project in that tier has no page here.** `examples/05-services/04-shutdown-and-drain/` (one binary, `qb-example-services-shutdown-and-drain`) is not analysed by any walkthrough below. Its subject is the shutdown sequence itself — SIGTERM arrives as a `qb::SignalEvent`, the acceptor stops accepting, work already taken is drained, every output buffer is flushed, and the process exits with a code that says whether it ever bound its port. Read its source and its header block directly.

**This section used to carry five pages.** The two that are gone analysed programs the 3.0 restructure retired rather than moved: `distributed_computing` (a `qb-core`-only scheduler, replaced by `examples/04-patterns/03-worker-pool.cpp` and `04-scatter-gather.cpp`) and `file_monitor` (replaced by `examples/02-io/08-timeouts-and-watchers.cpp`). A walkthrough outlives its subject by exactly zero commits, so both went with the programs they read.

## Pages in this section

| Analysis | Source | What it demonstrates |
|---|---|---|
| [TCP chat system](./chat_tcp_analysis.md) | `examples/05-services/01-tcp-chat/` | A multi-core TCP chat server and client. A dedicated `AcceptActor` round-robins connections to a `ServerActor` pool; each `ServerActor` owns its `ChatSession`s; a central `ChatRoomActor` holds chat state. Covers the `qb::io::use<>` client/server mixins, a custom binary `ChatProtocol`, and non-actor session objects. |
| [Asynchronous file processing](./file_processor_analysis.md) | `examples/05-services/03-file-pipeline/` | A manager-worker pattern that keeps blocking file I/O off the event loop. A `FileManager` dispatches `ReadFileRequest`/`WriteFileRequest` to a `FileWorker` pool; each worker wraps synchronous `qb::io::sys::file` calls in `qb::io::async::callback`. |
| [Publish/subscribe message broker](./message_broker_analysis.md) | `examples/05-services/02-pubsub-broker/` | A topic-based TCP broker. An `AcceptActor` feeds a `ServerActor` pool of `BrokerSession`s; a central `TopicManagerActor` owns subscriptions and fans published payloads out through a shared `broker::MessageContainer` so the payload is stored once and shared across deliveries. |

## Suggested reading order

1. **[TCP chat system](./chat_tcp_analysis.md)** — start here. It is the most direct application of [building network actors](../network_actors.md): the split acceptor / session-pool / state-hub topology, the `qb::io::use<>` mixins, and a custom protocol, in one self-contained program.
2. **[Publish/subscribe message broker](./message_broker_analysis.md)** — the natural follow-up. It reuses the chat topology and adds topic routing and the zero-copy `broker::MessageContainer` broadcast pattern.
3. **[Asynchronous file processing](./file_processor_analysis.md)** — the canonical answer to "I have blocking I/O inside an actor." Read it with the offloading section of [async operations inside actors](../async_in_actors.md).

## What each analysis covers

Every page follows the same shape:

- **Purpose and architecture** — the actor topology and which actor owns which state.
- **QB facilities in use** — where the example reaches for `qb::io::use<>`, `Actor::spawn` with `qb::ScopedCoroContext`, `qb::io::async::callback`, `qb::ICallback`, custom `qb::Event` types, and protocol classes, with the relevant source path. (`qb::ICallback` appears in two of the three: the console `InputActor` of `01-tcp-chat` and of `02-pubsub-broker`, where it reads `std::cin` once per loop turn — a blocking call that both pages now flag, kept survivable only by isolating that actor on its own core.)
- **Design pattern** — the reusable shape (split acceptor / session pool, manager-worker offload, zero-copy broadcast) abstracted from the concrete program.

> **Every page here carries a correction, not just a description.** `01-tcp-chat`,
> `02-pubsub-broker` and `03-file-pipeline` were written before qb 3.0's
> cancellation-scoped coroutines, when the programs they analyse armed delayed work with
> `qb::io::async::callback([this]{ … }, delay)` — a timer the event loop owns and the actor does
> not, which fires after `~Actor()` and makes any `is_alive()` / `_is_active` guard inside it the
> use-after-free rather than the protection against it. **All of them have since been
> repaired**: every such site now waits with `spawn(...)` + `co_await ctx.sleep(d)` inside the
> actor's cancellation scope and wakes the actor with a self-addressed tick event. Each page keeps
> the correction, because the shape is what a reader has to learn — and because the *no-delay*
> overload `qb::io::async::callback(fn)`, which runs inline and is not this hazard, is still used
> in `qb-example-services-file-pipeline` and is easy to confuse with it. The AddressSanitizer
> evidence that made it a rule was measured on a pre-3.0 example, since retired, that carried the
> shape at eight sites; `qb-example-modules-redis-transactions` produced a second, independent
> instance.

These are case studies, not API references. When an analysis names a type, follow the cross-link to the page that owns its definition rather than relying on the summary here.

## See also

- [Integrating core actors with asynchronous I/O](../README.md) — the section overview and the single-thread-per-core contract these examples depend on.
- [Asynchronous operations inside actors](../async_in_actors.md) — `qb::io::async::callback`, `scoped_callback`, `with_timeout<T>`, `qb::ICallback`, and the blocking-I/O offload pattern.
- [Building network actors](../network_actors.md) — the `qb::io::use<>` mixins and the acceptor / session-manager architecture the `chat_tcp` and `message_broker` studies build on.
- [Reference: `qb::Actor`](../../4_qb_core/actor.md) — actor lifecycle, `qb::ICallback` registration, and the messaging API the examples use.
