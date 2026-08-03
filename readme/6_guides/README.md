# Guides

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Task-oriented walkthroughs that take you from a first build to a production deployment, plus the patterns, tuning knobs, failure model, and migration paths you reach for along the way.

**Prerequisites:** [Core concepts](../2_core_concepts/README.md) — **See also:** [qb-core](../4_qb_core/README.md), [Core and I/O integration](../5_core_io_integration/README.md), [Reference](../7_reference/README.md)

## Summary

The earlier sections explain *what* the framework is: the actor model, the asynchronous runtime, and how the two compose. This section is about *doing*. Each page is a self-contained walkthrough that starts from a concrete goal, shows complete and compilable code, and ends with the pitfalls that bite real systems. Pages cross-link to the conceptual chapters and the [reference](../7_reference/README.md) rather than restating definitions, so follow those links when you need the underlying contract.

## Pages in this section

| Page | What it covers |
| --- | --- |
| [Getting started](./getting_started.md) | Install qb, build and run a first actor, add a non-blocking timer, and find the next page to read. |
| [Advanced usage](./advanced_usage.md) | Custom wire protocols, scaling actors across cores, coroutines inside actors, service actors, and composing the qbm modules with your own actors. |
| [Error handling and resilience](./error_handling.md) | The exception policy, the `VirtualCore` fail-stop boundary, supervision patterns you build yourself, asynchronous I/O error events, and `async::callback` lifetime rules. |
| [Patterns cookbook](./patterns_cookbook.md) | Compilable recipes for one-shot and periodic timers, request/reply, broadcast fan-out, multi-stage pipelines, and graceful shutdown. |
| [Performance tuning](./performance_tuning.md) | Core placement, event-loop idle latency, allocation behavior, the lock-free transport, and the build flags (`QB_ENABLE_NATIVE_ARCH`, `QB_ENABLE_LTO`) that change codegen. |
| [Resource management](./resource_management.md) | How RAII, actor ownership, and the actor lifecycle release memory, descriptors, sockets, and TLS contexts deterministically — and where the framework hands ownership back to you. |
| [Migration guide](./migration_guide.md) | Moving from `std::thread` plus locked queues to actors, and from the pre-2.0 time types to the `qb::duration`/`qb::mono_time`/`qb::wall_time` chrono model. |
| [Production readiness checklist](./production_checklist.md) | Building a portable binary, configuring TLS, capping resource use, wiring logging and signal handling, running the test suite, and deciding what to monitor. |

## Suggested reading order

Start with [Getting started](./getting_started.md) to stand up a working build and a first actor. From there the path depends on what you are doing:

1. **Building an application.** Read the [patterns cookbook](./patterns_cookbook.md) for the everyday interactions (timers, request/reply, fan-out, pipelines, shutdown), then [advanced usage](./advanced_usage.md) when you need custom protocols, multicore scaling, or coroutines inside actors.
2. **Hardening it.** Read [error handling and resilience](./error_handling.md) to understand the failure model and the fail-stop boundary, then [resource management](./resource_management.md) for deterministic cleanup of memory, descriptors, sockets, and TLS contexts.
3. **Shipping it.** Use [performance tuning](./performance_tuning.md) to address measured throughput and latency goals, and work through the [production readiness checklist](./production_checklist.md) before you deploy.

If you are porting existing code, read the [migration guide](./migration_guide.md) first; it maps thread-and-lock idioms onto actors and lists the retired time types you must replace.

## See also

- [Core concepts](../2_core_concepts/README.md) — the actor model, the threading model, and the event system these guides assume.
- [qb-core](../4_qb_core/README.md) and [Core and I/O integration](../5_core_io_integration/README.md) — the API chapters the guides link into.
- [Reference](../7_reference/README.md) — API overview, invariants, build and CMake options, testing, FAQ, and glossary.
