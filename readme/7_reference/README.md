# Reference

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.6.0 (C++20 default, C++23 supported)

Fast-lookup material for working with qb: an API map, the build and dependency reference, the thread-safety and lifetime invariants each library upholds, the test and benchmark suites, plus an FAQ and a glossary.

**Prerequisites:** none — **See also:** [Developer guides](../6_guides/README.md), [qb-core module](../4_qb_core/README.md), [qb-io module](../3_qb_io/README.md)

## Summary

The earlier sections teach concepts in narrative order. This section is the opposite: each page answers a specific factual question — which header owns a type, which CMake variable controls a feature, which thread may touch a given object, what a term means — without requiring you to read a tutorial first. Every claim is grounded in the header or build file that owns it, and the invariant pages are the contracts the libraries assume; read them before writing a custom actor, protocol, transport, or any code that touches a lock-free primitive directly.

## Pages in this section

| Page | What it covers |
|---|---|
| [Public API overview](./api_overview.md) | A reference map of the public API: the namespaces, key types, and signatures of `qb-core` and `qb-io`, each linked to the header that owns it. |
| [Building from source](./building.md) | Configuring, building, testing, and installing qb with CMake — requirements, presets, options, generators, and the install layout. |
| [CMake and third-party dependencies](./cmake_dependencies.md) | How qb resolves each dependency as bundled in-tree, fetched on demand, or system-supplied, driven by `QB_DEPS_FETCH_FALLBACK` and the `QB_USE_SYSTEM_*` switches. |
| [CMake options reference](./cmake_options.md) | Every `QB_*` CMake variable that configures a build, its default, and what it controls. |
| [qb-core thread-safety and lifecycle invariants](./core_invariants.md) | The rules `qb-core` assumes and the guarantees it gives: which thread owns what, when an actor is alive, and in what order events arrive. Required reading before writing actor, coroutine, or event-router code. |
| [qb-io invariants: threading, lifetime, and ownership](./io_invariants.md) | The rules the asynchronous stack assumes — one event loop per thread, where callbacks run, when objects may be destroyed, and who owns each socket. Required reading before writing a custom protocol, transport, or async base class. |
| [Frequently asked questions](./faq.md) | Short, grounded answers to the questions that come up most when adopting qb, each linking to the page that owns the full explanation. |
| [Glossary](./glossary.md) | A definition for every domain term used across the documentation, grounded in the header that owns it and linked to the page that explains it in full. |
| [Lock-free primitives](./lockfree_primitives.md) | The lock-free building blocks under `qb/system/lockfree` — SPSC and MPSC ring buffers, an unbounded MPSC queue, and a spinlock — that back the inter-core message path, plus the threading contract for using them directly. |
| [Testing the framework](./testing.md) | How the test suite is organized, how to build and run it with CTest and GoogleTest, and how the coverage option is wired. |
| [Benchmarks](./benchmarks.md) | The qb-core micro-benchmark suite: the Google Benchmark targets gated by `QB_BUILD_BENCHMARKS`, what each one measures, and how to build, run, and read them. |

## Suggested reading order

Reference pages are meant for lookup, not front-to-back reading. Use these entry points instead:

- **Evaluating or first building qb.** Start with [Building from source](./building.md), then [CMake options reference](./cmake_options.md) and [CMake and third-party dependencies](./cmake_dependencies.md) when a configure step needs tuning.
- **Looking up an API.** Go straight to the [Public API overview](./api_overview.md); follow its links into the owning headers. Keep the [Glossary](./glossary.md) open for unfamiliar terms.
- **Writing framework-level code** (a custom actor base, protocol, transport, or anything using a lock-free primitive directly). Read [qb-core thread-safety and lifecycle invariants](./core_invariants.md) and [qb-io invariants: threading, lifetime, and ownership](./io_invariants.md) first; consult [Lock-free primitives](./lockfree_primitives.md) only if you call those structures yourself rather than through the engine.
- **Contributing or measuring.** [Testing the framework](./testing.md) for the suite layout and CTest workflow; [Benchmarks](./benchmarks.md) for the micro-benchmark targets.
- **Stuck on a specific question.** Check the [FAQ](./faq.md) before reading a full page — each answer links to the page that owns the detail.

For task-oriented, narrative material, see the [Developer guides](../6_guides/README.md); for the conceptual treatment of each library, see [qb-core](../4_qb_core/README.md) and [qb-io](../3_qb_io/README.md).
