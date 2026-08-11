# qb documentation

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

The narrative guide to the qb actor framework, organized into seven sections for progressive learning, from first principles to production reference.

**Prerequisites:** none — **See also:** [project README](../README.md) · [Getting started](./6_guides/getting_started.md)

## About this guide

qb is a C++20-first framework with optional C++23 support for concurrent and distributed systems built on the actor model. It pairs share-nothing actors with a non-blocking asynchronous I/O engine (`qb-io`) and native C++20 coroutines: `qb-core` is the actor engine, and `qb-io` is the runtime it stands on. This guide explains the model, the two libraries, and how to build with them.

The sections below progress from concepts to reference. New readers should follow them in order; experienced readers can jump to the section that matches the task at hand. Each section opens with its own index page that links to the pages within it.

## Sections

| # | Section | What it covers |
|---|---|---|
| 0 | [Foundations](./0_foundations/README.md) | The layer beneath the event loop: the time vocabulary, the allocator pipe, containers, encoding helpers, the lock-free primitives, and the ABI machinery. Optional before adopting; required before contributing. |
| 1 | [Introduction](./1_introduction/README.md) | What qb is, the design philosophy behind it, and when the actor model is the right fit. |
| 2 | [Core concepts](./2_core_concepts/README.md) | The vocabulary: actors, the event system, the asynchronous I/O model, concurrency, and the threading model. |
| 3 | [qb-io](./3_qb_io/README.md) | The asynchronous runtime: the event loop, transports, the protocol layer, coroutines, SSL/TLS, QUIC, and utilities. |
| 4 | [qb-core](./4_qb_core/README.md) | The actor engine: `qb::Actor`, the `qb::Main` engine and `qb::VirtualCore` scheduling, messaging, and actor patterns. |
| 5 | [Core and I/O integration](./5_core_io_integration/README.md) | How actors use `qb-io` together — async work inside actors and network actors — with worked examples. |
| 6 | [Guides](./6_guides/README.md) | Task-oriented walkthroughs: getting started, patterns, performance tuning, error handling, resource management, and migration. |
| 7 | [Reference](./7_reference/README.md) | API overview, build system, CMake options, core and I/O invariants, benchmarks, testing, FAQ, and glossary. |

## Suggested reading order

1. Work through [Getting started](./6_guides/getting_started.md) to build and run a first actor — the model is easier to read once you have seen it run.
2. Read [Introduction](./1_introduction/README.md) to understand the design and decide whether it fits the problem.
3. Read [Core concepts](./2_core_concepts/README.md) for the vocabulary used throughout the rest of the guide.
4. Go deeper into the library that matters for the task: [qb-core](./4_qb_core/README.md) for the actor engine, [qb-io](./3_qb_io/README.md) for the runtime.
5. See the two combined in [Core and I/O integration](./5_core_io_integration/README.md), then consult [Guides](./6_guides/README.md) and [Reference](./7_reference/README.md) as needed.

[Foundations](./0_foundations/README.md) is deliberately outside that order. Its public half — `qb::duration`, `qb::string<N>`, the containers — you will meet in the first actor you write, and each page stands alone; its machinery half is what you read before contributing to the engine, or the day one of the framework's rules surprises you and you want the mechanism rather than the rule.

## Project policies

| Document | Purpose |
|---|---|
| [INSTALL](../INSTALL.md) | Prerequisites, build, and installation instructions. |
| [VERSIONING](../VERSIONING.md) | Semantic Versioning policy and the source of the version number. |
| [CHANGELOG](../CHANGELOG.md) | Notable changes per release. |
| [SECURITY](../SECURITY.md) | Supported versions and how to report a vulnerability. |
| [SUPPORT](../SUPPORT.md) | Where to ask questions and get help. |
| [CONTRIBUTING](../CONTRIBUTING.md) | How to propose changes and the contribution workflow. |

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE).
