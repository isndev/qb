# Case studies: core and I/O integration examples

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Five worked analyses of complete applications that combine `qb-core` actors with `qb-io` networking, watchers, and deferred work end to end.

**Prerequisites:** [Integrating core actors with asynchronous I/O](../README.md), [Asynchronous operations inside actors](../async_in_actors.md), [Building network actors](../network_actors.md) — **See also:** [qb-core module](../../4_qb_core/README.md), [qb-io module](../../3_qb_io/README.md)

## Summary

The two task pages in this section — [async operations inside actors](../async_in_actors.md) and [building network actors](../network_actors.md) — describe the mechanisms in isolation. These case studies show them composed into runnable programs. Each analysis breaks down one example from the repository: its actor topology, the `qb-io` facilities it drives, the events that travel between actors, and the design pattern it illustrates.

Every analysis is grounded in source you can build and run. **The tree moved with 3.0** and these pages name the new paths: the two networking studies are `examples/05-services/01-tcp-chat/` and `examples/05-services/02-pubsub-broker/`, the file-processing one is `examples/05-services/03-file-pipeline/`, and `file_monitor` has not moved — it is retired into three other homes and still sits in `examples/core_io/file_monitor/` until its replacements are written. Two of the five are **Unix-only**: `examples/05-services/CMakeLists.txt:50-52` wraps the file pipeline in `if (NOT QB_PLATFORM_WINDOWS)`, and `examples/core_io/CMakeLists.txt:48-50` does the same for `file_monitor`, so neither is configured on Windows. The `distributed_computing` study analyzes `examples/core/example10_distributed_computing.cpp`, a pure `qb-core` simulation whose every periodic activity is a lifetime-bound coroutine — `spawn(...)` + `co_await ctx.sleep(...)` — rather than network I/O. Read an analysis alongside its source rather than as a substitute for it.

## Pages in this section

| Analysis | Source | What it demonstrates |
|---|---|---|
| [TCP chat system](./chat_tcp_analysis.md) | `examples/05-services/01-tcp-chat/` | A multi-core TCP chat server and client. A dedicated `AcceptActor` round-robins connections to a `ServerActor` pool; each `ServerActor` owns its `ChatSession`s; a central `ChatRoomActor` holds chat state. Covers the `qb::io::use<>` client/server mixins, a custom binary `ChatProtocol`, and non-actor session objects. |
| [Distributed computing simulation](./distributed_computing_analysis.md) | `examples/core/example10_distributed_computing.cpp` | Task generation, priority scheduling with round-robin dispatch, worker execution, result collection, and system monitoring across cores. A `qb-core`-only example: every periodic activity is a `spawn(...)` + `co_await ctx.sleep(...)` coroutine bound to its actor's cancellation scope, and telemetry is a request/response poll — coordination travels entirely as events, with no shared state between actors. |
| [File system monitor](./file_monitor_analysis.md) | `examples/core_io/file_monitor/` | A `DirectoryWatcher` actor driving `qb::io::async::directory_watcher` (inotify on Linux; periodic stat polling elsewhere, including macOS and BSD) to surface create, modify, and delete events, fanned out to subscriber actors and acted on by a `FileProcessor`. |
| [Asynchronous file processing](./file_processor_analysis.md) | `examples/05-services/03-file-pipeline/` | A manager-worker pattern that keeps blocking file I/O off the event loop. A `FileManager` dispatches `ReadFileRequest`/`WriteFileRequest` to a `FileWorker` pool; each worker wraps synchronous `qb::io::sys::file` calls in `qb::io::async::callback`. |
| [Publish/subscribe message broker](./message_broker_analysis.md) | `examples/05-services/02-pubsub-broker/` | A topic-based TCP broker. An `AcceptActor` feeds a `ServerActor` pool of `BrokerSession`s; a central `TopicManagerActor` owns subscriptions and fans published payloads out through a shared `broker::MessageContainer` so the payload is stored once and shared across deliveries. |

## Suggested reading order

1. **[TCP chat system](./chat_tcp_analysis.md)** — start here. It is the most direct application of [building network actors](../network_actors.md): the split acceptor / session-pool / state-hub topology, the `qb::io::use<>` mixins, and a custom protocol, in one self-contained program.
2. **[Publish/subscribe message broker](./message_broker_analysis.md)** — the natural follow-up. It reuses the chat topology and adds topic routing and the zero-copy `broker::MessageContainer` broadcast pattern.
3. **[Asynchronous file processing](./file_processor_analysis.md)** — the canonical answer to "I have blocking I/O inside an actor." Read it with the offloading section of [async operations inside actors](../async_in_actors.md).
4. **[File system monitor](./file_monitor_analysis.md)** — extends the watcher material from the async page to a platform-backed `directory_watcher` and a subscriber fan-out.
5. **[Distributed computing simulation](./distributed_computing_analysis.md)** — read last. It is `qb-core`-only and the most elaborate event topology of the five; it consolidates load balancing, heartbeating, and orchestration without any network surface, and it is the page that explains why a `qb::io::async::callback([this]{ ... }, delay)` timer guarded by an `_is_active` flag is a use-after-free rather than a pattern.

## What each analysis covers

Every page follows the same shape:

- **Purpose and architecture** — the actor topology and which actor owns which state.
- **QB facilities in use** — where the example reaches for `qb::io::use<>`, `Actor::spawn` with `qb::ScopedCoroContext`, `qb::io::async::callback`, `qb::ICallback`, custom `qb::Event` types, and protocol classes, with the relevant source path. (`qb::ICallback` appears in only two of the five: the console `InputActor` of `chat_tcp` and of `message_broker`, where it reads `std::cin` once per loop turn — a blocking call that both pages now flag, kept survivable only by isolating that actor on its own core.)
- **Design pattern** — the reusable shape (split acceptor / session pool, manager-worker offload, watcher fan-out, zero-copy broadcast) abstracted from the concrete program.

> **Four of the five pages carry a correction, not just a description.** `chat_tcp`,
> `message_broker`, `qb-example-services-file-pipeline` and `file_monitor` were written before qb 3.0's
> cancellation-scoped coroutines, when the programs they analyse armed delayed work with
> `qb::io::async::callback([this]{ … }, delay)` — a timer the event loop owns and the actor does
> not, which fires after `~Actor()` and makes any `is_alive()` / `_is_active` guard inside it the
> use-after-free rather than the protection against it. **All five programs have since been
> repaired**: every such site now waits with `spawn(...)` + `co_await ctx.sleep(d)` inside the
> actor's cancellation scope and wakes the actor with a self-addressed tick event. Each page keeps
> the correction, because the shape is what a reader has to learn — and because the *no-delay*
> overload `qb::io::async::callback(fn)`, which runs inline and is not this hazard, is still used
> in `qb-example-services-file-pipeline` and `file_monitor` and is easy to confuse with it.
> `distributed_computing` carries the AddressSanitizer evidence that made it a rule;
> `qb-example-modules-redis-transactions` in `examples/qbm/redis/` produced a second, independent instance.

These are case studies, not API references. When an analysis names a type, follow the cross-link to the page that owns its definition rather than relying on the summary here.

## See also

- [Integrating core actors with asynchronous I/O](../README.md) — the section overview and the single-thread-per-core contract these examples depend on.
- [Asynchronous operations inside actors](../async_in_actors.md) — `qb::io::async::callback`, `scoped_callback`, `with_timeout<T>`, `qb::ICallback`, and the blocking-I/O offload pattern.
- [Building network actors](../network_actors.md) — the `qb::io::use<>` mixins and the acceptor / session-manager architecture the `chat_tcp` and `message_broker` studies build on.
- [Reference: `qb::Actor`](../../4_qb_core/actor.md) — actor lifecycle, `qb::ICallback` registration, and the messaging API the examples use.
