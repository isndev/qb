# Case studies: core and I/O integration examples

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

Five worked analyses of complete applications that combine `qb-core` actors with `qb-io` networking, watchers, and deferred work end to end.

**Prerequisites:** [Integrating core actors with asynchronous I/O](../README.md), [Asynchronous operations inside actors](../async_in_actors.md), [Building network actors](../network_actors.md) — **See also:** [qb-core module](../../4_qb_core/README.md), [qb-io module](../../3_qb_io/README.md)

## Summary

The two task pages in this section — [async operations inside actors](../async_in_actors.md) and [building network actors](../network_actors.md) — describe the mechanisms in isolation. These case studies show them composed into runnable programs. Each analysis breaks down one example from the repository: its actor topology, the `qb-io` facilities it drives, the events that travel between actors, and the design pattern it illustrates.

Every analysis is grounded in source you can build and run. The networking examples (`chat_tcp`, `message_broker`) live under `examples/core_io/`; `file_monitor` and `file_processor` live there too; the `distributed_computing` study analyzes `examples/core/example10_distributed_computing.cpp`, a pure `qb-core` simulation that uses `qb::ICallback` and `qb::io::async::callback` for timing rather than network I/O. Read an analysis alongside its source rather than as a substitute for it.

## Pages in this section

| Analysis | Source | What it demonstrates |
|---|---|---|
| [TCP chat system](./chat_tcp_analysis.md) | `examples/core_io/chat_tcp/` | A multi-core TCP chat server and client. A dedicated `AcceptActor` round-robins connections to a `ServerActor` pool; each `ServerActor` owns its `ChatSession`s; a central `ChatRoomActor` holds chat state. Covers the `qb::io::use<>` client/server mixins, a custom binary `ChatProtocol`, and non-actor session objects. |
| [Distributed computing simulation](./distributed_computing_analysis.md) | `examples/core/example10_distributed_computing.cpp` | Task generation, priority scheduling with load balancing, worker execution, result collection, and system monitoring across cores. A `qb-core`-only example: periodic work runs on `qb::ICallback` and `qb::io::async::callback`; coordination travels entirely as events. |
| [File system monitor](./file_monitor_analysis.md) | `examples/core_io/file_monitor/` | A `DirectoryWatcher` actor driving `qb::io::async::directory_watcher` (inotify on Linux; periodic stat polling elsewhere, including macOS and BSD) to surface create, modify, and delete events, fanned out to subscriber actors and acted on by a `FileProcessor`. |
| [Asynchronous file processing](./file_processor_analysis.md) | `examples/core_io/file_processor/` | A manager-worker pattern that keeps blocking file I/O off the event loop. A `FileManager` dispatches `ReadFileRequest`/`WriteFileRequest` to a `FileWorker` pool; each worker wraps synchronous `qb::io::system::file` calls in `qb::io::async::callback`. |
| [Publish/subscribe message broker](./message_broker_analysis.md) | `examples/core_io/message_broker/` | A topic-based TCP broker. An `AcceptActor` feeds a `ServerActor` pool of `BrokerSession`s; a central `TopicManagerActor` owns subscriptions and fans published payloads out through a shared `broker::MessageContainer` so the payload is stored once and shared across deliveries. |

## Suggested reading order

1. **[TCP chat system](./chat_tcp_analysis.md)** — start here. It is the most direct application of [building network actors](../network_actors.md): the split acceptor / session-pool / state-hub topology, the `qb::io::use<>` mixins, and a custom protocol, in one self-contained program.
2. **[Publish/subscribe message broker](./message_broker_analysis.md)** — the natural follow-up. It reuses the chat topology and adds topic routing and the zero-copy `broker::MessageContainer` broadcast pattern.
3. **[Asynchronous file processing](./file_processor_analysis.md)** — the canonical answer to "I have blocking I/O inside an actor." Read it with the offloading section of [async operations inside actors](../async_in_actors.md).
4. **[File system monitor](./file_monitor_analysis.md)** — extends the watcher material from the async page to a platform-backed `directory_watcher` and a subscriber fan-out.
5. **[Distributed computing simulation](./distributed_computing_analysis.md)** — read last. It is `qb-core`-only and the most elaborate event topology of the five; it consolidates load balancing, heartbeating, and orchestration without any network surface.

## What each analysis covers

Every page follows the same shape:

- **Purpose and architecture** — the actor topology and which actor owns which state.
- **QB facilities in use** — where the example reaches for `qb::io::use<>`, `qb::io::async::callback`, `qb::ICallback`, custom `qb::Event` types, and protocol classes, with the relevant source path.
- **Design pattern** — the reusable shape (split acceptor / session pool, manager-worker offload, watcher fan-out, zero-copy broadcast) abstracted from the concrete program.

These are case studies, not API references. When an analysis names a type, follow the cross-link to the page that owns its definition rather than relying on the summary here.

## See also

- [Integrating core actors with asynchronous I/O](../README.md) — the section overview and the single-thread-per-core contract these examples depend on.
- [Asynchronous operations inside actors](../async_in_actors.md) — `qb::io::async::callback`, `scoped_callback`, `with_timeout<T>`, `qb::ICallback`, and the blocking-I/O offload pattern.
- [Building network actors](../network_actors.md) — the `qb::io::use<>` mixins and the acceptor / session-manager architecture the `chat_tcp` and `message_broker` studies build on.
- [Reference: `qb::Actor`](../../4_qb_core/actor.md) — actor lifecycle, `qb::ICallback` registration, and the messaging API the examples use.
