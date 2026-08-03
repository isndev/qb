# Introduction

> **Audience:** Evaluator · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

The starting point for understanding what qb is, the problems it addresses, and the design principles that shape it.

**Prerequisites:** none — **See also:** [Core concepts](../2_core_concepts/README.md) · [Getting started](../6_guides/getting_started.md)

## Summary

qb is a C++20-first framework with optional C++23 support for building concurrent and distributed systems on the actor model, composed of two libraries: an actor engine (`qb-core`) layered on a standalone non-blocking asynchronous I/O runtime (`qb-io`). This section orients newcomers and evaluators before they reach the hands-on material. Read it to decide whether qb fits your problem and to understand the reasoning behind its architecture.

## Pages in this section

| Page | What it covers |
|---|---|
| [What qb is](./overview.md) | The two-library structure (`qb-io` and `qb-core`), the problem qb addresses, who it is for, a first actor, and when to use — or not use — it. |
| [Design philosophy](./philosophy.md) | The principles behind qb: share-nothing actor isolation, asynchronous-by-default I/O, the layered and modular split, explicit modern C++20/23, and lock-free inter-core messaging, each with its rationale. |

## Suggested reading order

1. **[What qb is](./overview.md)** — start here for the shape of the framework and a runnable first actor.
2. **[Design philosophy](./philosophy.md)** — then read why the architecture is built the way it is.

## Next steps

After this section:

- **[Core concepts](../2_core_concepts/README.md)** — actors, events, the asynchronous I/O model, and the threading model in detail.
- **[Getting started](../6_guides/getting_started.md)** — build and run your first qb application.

## See also

- [qb-io](../3_qb_io/README.md) — the asynchronous runtime in depth.
- [qb-core](../4_qb_core/README.md) — the actor engine and messaging.
- [Project README](../../README.md) — the top-level overview, platform support, and module ecosystem.
