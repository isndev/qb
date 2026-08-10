# Frequently asked questions

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Short, grounded answers to the questions that come up most when adopting qb, each linking to the page that owns the full explanation.

**Prerequisites:** none — **See also:** [Documentation home](../README.md), [Glossary](./glossary.md), [Core invariants](./core_invariants.md)

## Reporting a bug versus a security issue

These go to two different places. Choose by impact, not by certainty.

| You found | Where it goes | How |
|---|---|---|
| A reproducible defect or a feature request | Public issue tracker | Follow the bug checklist in [CONTRIBUTING.md](../../CONTRIBUTING.md): search first, reduce to a minimal reproduction, state expected versus actual behavior and your environment (OS, compiler and version, qb version, build flags). |
| A vulnerability — memory-safety defect, denial-of-service vector, authentication or cryptographic weakness, input-handling flaw | Private advisory | Follow [SECURITY.md](../../SECURITY.md). Open a private advisory through the repository's Security tab; **never** in a public issue, pull request, or discussion. |

If you are unsure whether something is exploitable, treat it as a security issue and report it privately. A maintainer can always redirect it to the public tracker; a public disclosure cannot be taken back.

Supported versions, scope, and the disclosure timeline are in [SECURITY.md](../../SECURITY.md). Where to ask "how do I…" questions, and what support covers, is in [SUPPORT.md](../../SUPPORT.md).

## Is qb thread-safe? How do I manage actor state?

An actor's own member variables need no locks. Each actor is pinned to one `VirtualCore` and processes its events one at a time, so its handlers and callbacks never run concurrently with each other. Keep state private and mutate it only in response to events.

You are responsible for synchronizing anything *outside* that model:

- Global or `static` data shared between actors.
- Resources reached through a third-party library that is not part of an actor.
- Memory you deliberately share between actors (for example, the contents behind a `std::shared_ptr` you passed in an event).

The framework-native answer is to wrap the shared resource in a single owning actor and serialize access through events to it, rather than reaching for a `std::mutex`. See [The actor model](../2_core_concepts/actor_model.md) and [Resource management](../6_guides/resource_management.md).

## Why does the framework avoid mutexes?

Because the concurrency model removes the need for them in application code. Actors share no state and run their mailbox sequentially, so two of an actor's handlers can never race on its data — a property the runtime gives you by construction, not one you enforce with locks. Cross-core message passing is carried by lock-free MPSC queues inside the engine, not by guarded shared queues. See [The threading model](../2_core_concepts/threading_model.md) and the [lock-free primitives reference](./lockfree_primitives.md).

The framework does not forbid `std::mutex`. It is the right tool when you bridge to code outside the actor model (see the previous question). The point is that idiomatic qb code does not need one, and reaching for a lock to protect an actor's own state is a sign the state should live behind a single actor instead.

## `push()` versus `send()` — which should I use?

Use `push()`. It is the default and covers nearly every case.

| Method | Ordering | Event payload constraint |
|---|---|---|
| `push<Event>(dest, …)` | Ordered: FIFO per sender→receiver pair | Any event, including members with non-trivial destructors (`std::vector`, `std::shared_ptr`, `std::unique_ptr`) |
| `send<Event>(dest, …)` | **Unordered**, even to the same destination | Event type **must be trivially destructible** (POD members or `qb::string<N>` only) |

`send()` exists for narrow, profiled cases where same-core latency matters and ordering does not; misusing it produces ordering bugs that are hard to trace. Both signatures are in `qb/src/qb/core/Actor.h` (`push` at `qb/src/qb/core/Actor.h:877`, `send` at `:900`).

Neither row admits a **by-value `std::string`**, and that exclusion is independent of the ordering/destructibility axis above: the runtime relocates events with raw `memcpy`, copying the bytes to a new address and abandoning the source without running a destructor there. A member that points into its own storage — which is exactly what a *short* `std::string` is on libstdc++, whose small-string buffer is addressed by an internal pointer — keeps addressing the old bytes afterwards, so the receiver reads reused memory and `~basic_string()` frees an address that never came from the heap. This is not cross-core-only: the source pipe `memcpy`s what it already holds when it grows, and `reply`/`forward` byte-recycle the event, so a same-core `push` is exposed too. libc++ recomputes `data()` from `this`, which is why the defect corrupts on Linux while passing every macOS test. Use `qb::string<N>` for bounded text, or box the string in a `std::shared_ptr`. See [Messaging](../4_qb_core/messaging.md#push--ordered-the-default) for the full rule.

One contract applies to both, and to `broadcast()`: they are declared `noexcept`. If growing the pipe buffer or running an event constructor throws — for example, an allocation failure under memory pressure — the throw crosses the `noexcept` boundary and calls `std::terminate()` (`qb/src/qb/core/Actor.h:873`; the three declarations are `push` at `:877`, `send` at `:900` and `broadcast` at `:1016`). Keep events small and allocation-light. Full messaging semantics live in [Messaging](../4_qb_core/messaging.md).

## Why is `qb::string<N>` preferred over `std::string` for event fields?

For event fields *preferred* is too soft on one of the three axes below: on the third, a by-value `std::string` is not a legal event member at all. `qb::string<N>` stores its characters inline. It publicly derives from `std::array<char, N + 1>` (`qb/src/qb/string.h:86`), so an event that uses it has a fixed, self-contained layout with no heap pointer. Three consequences matter for events:

- **No per-event heap allocation** for strings up to `N` characters. Because `push()`/`send()` are `noexcept` and an allocation failure inside them terminates the process, an inline field removes one source of throwing on the messaging hot path.
- **A predictable layout** that does not depend on a standard library's small-string-optimization details, which keeps event objects stable when they are copied or moved through the engine.
- **It is trivially relocatable, which `std::string` is not.** The engine moves events by raw `memcpy` and never runs a destructor on the source bytes; a short `std::string` addresses its own inline buffer through an internal pointer on libstdc++, so it dangles after the move. `qb::string<N>` is an inline `char` array plus an integer length (`qb/src/qb/string.h:108`) — nothing inside it points at itself. This is the reason the previous question rules a by-value `std::string` out of `push()` as well as `send()`. When the text has no usable upper bound, box it in a `std::shared_ptr` rather than inlining a `std::string`.

Mind the trade-offs:

- It **silently truncates**. Any assignment, append, or constructor that exceeds `N` clamps the length to `N` rather than throwing (`qb/src/qb/string.h:201`). Size `N` for your worst case. Only `at()` and an out-of-range `substr()` throw.
- `qb::string<N>` overrides the common accessors (`size()`, `length()`, `at()`, `operator[]`, `end()`) to honor the *logical* length it tracks internally (`size`/`length` at `qb/src/qb/string.h:527-540`, `at` at `:335-341`, `end` at `:456-459`). Because it publicly derives from `std::array<char, N + 1>`, the base members stay reachable through explicit `std::array<…>::`-qualification or by slicing to the array base, and those report the *physical* array length `N + 1`. Prefer the `qb::string` members; do not reach for the `std::array` base directly.

For payloads that are large or genuinely dynamic, do not inline them. Carry a `std::shared_ptr<T>` (shared lifetime) or `std::unique_ptr<T>` (transferred ownership) in the event so only the pointer moves through the pipe. See [Messaging](../4_qb_core/messaging.md) for the data-handling patterns, and the canonical type reference for `qb::string` in [the API overview](./api_overview.md).

```cpp
// src: derived from qb/src/qb/core/Actor.h messaging contract
#include <qb/actor.h>
#include <qb/string.h>
#include <memory>
#include <vector>

// Small, fixed field: inline, no heap, trivially destructible -> usable with send().
struct StatusEvent : qb::Event {
    qb::string<32> code;
    explicit StatusEvent(const char *c) : code(c) {}
};

// Large payload: carry a smart pointer so only the pointer is copied through the pipe.
struct BlobEvent : qb::Event {
    std::shared_ptr<std::vector<char>> data;
    explicit BlobEvent(std::shared_ptr<std::vector<char>> d) : data(std::move(d)) {}
};
```

## How does qb handle errors and exceptions?

qb does not implement an Erlang-style supervision tree, and it does not catch exceptions per actor. The model gives you three guarantees and expects you to compose recovery from ordinary actors:

1. **Isolation.** Actors share no state, so a logic error in one actor cannot corrupt another's data.
2. **A fail-stop boundary.** An exception that escapes an actor handler is *not* caught per-actor — it unwinds the worker thread and stops every actor on that `VirtualCore`. Design handlers so a throw is either impossible or caught locally.
3. **Typed I/O error events.** Network and protocol failures arrive as events (for example `qb::io::async::event::disconnected`), not as exceptions, so you handle them in an `on()` overload like any other message.

To signal a *local* failure, `co_return false` from `onInit()` (it is a `qb::io::async::task<bool>` coroutine, so a plain `return false;` does not compile) to abort an actor's startup, or call `kill()` to terminate an actor cleanly (`qb/src/qb/core/Actor.h:331-333,354-355` and `:373`). Supervision strategies — health checks, restart, escalation — are application code you build on top of the boundary. The full treatment, including the `qb::Main` failure-reporting API, is in [Error handling and resilience](../6_guides/error_handling.md).

## Can I use coroutines inside an actor?

Yes — through two entry points, `spawn()` (recommended) and `spawn_detached()`, under strict lifetime rules. They are the only supported way to run a coroutine from within an actor (`qb/src/qb/core/Actor.h:1238-1239` and `:1201-1202`). `spawn()` *scopes* the coroutine to the actor — it is cancelled when the actor is killed — and hands it a `qb::ScopedCoroContext` with cancellation-aware operations and the native `ask()` request/response helper; `spawn_detached()` runs the coroutine detached, so it outlives the actor and receives a plain `qb::CoroContext`. Prefer `spawn()` unless the work must deliberately outlive its actor.

A spawned coroutine runs in an isolated context and **must not touch actor state after a `co_await`**: the actor may be destroyed while the coroutine is suspended, so dereferencing `this` or an actor member after suspension is undefined behavior. The rules:

- Copy every value you need **by value before the first `co_await`**. Never capture `this` or a reference to an actor member.
- After suspension, communicate only through the `CoroContext` argument. `ctx.push<Event>(args…)` posts an event back to the spawning actor itself; `ctx.push_to<Event>(dest, args…)` posts to another actor by id (`qb/src/qb/core/Actor.h:1397-1403` and `:1412-1413`). Both are safe even after the spawning actor has died — events addressed to a dead actor are dropped by the router. `ctx.id()` and `ctx.time()` are also safe (`qb/src/qb/core/Actor.h:1428-1429` and `:1437`).
- Keep coroutines short-lived; a long-running coroutine widens the window in which the actor can die underneath it.

```cpp
// src: derived from qb/src/qb/core/Actor.h spawn contract (Actor.h:1187)
#include <qb/actor.h>          // qb::Actor, CoroContext
#include <qb/io/async.h>       // qb::io::async::task

// Inside a qb::Actor subclass. fetch() stands in for any coroutine
// returning qb::io::async::task<Reply>.
void on(RequestEvent &ev) {
    // Copy everything needed BEFORE spawning; no 'this' capture.
    auto key    = ev.key;       // by value
    auto sender = ev.sender;    // ActorId, by value

    spawn([key, sender](auto ctx) -> qb::io::async::task<void> {
        auto reply = co_await fetch(key);                  // actor may be destroyed here
        // push_to() targets another actor by id; safe even if the spawning actor is gone.
        ctx.template push_to<ResultEvent>(sender, reply);
    });
}
```

The coroutine layer itself — `task<T>`, awaiters, channels, scopes, generators — is documented in [C++20 coroutines](../3_qb_io/coroutines.md). Note that those primitives run on a single-threaded scheduler, which is why they need no internal locking.

## Can I call a blocking third-party library from an actor?

Not directly inside an `on()` handler or a registered callback. A blocking call (synchronous file or network I/O, `std::this_thread::sleep_for`, a blocking library call) stalls the actor's `VirtualCore` and, with it, every other actor on that core. Two patterns keep the loop responsive:

- **Move a short, infrequent blocking call off the handler frame with `qb::io::async::defer`.** The handler returns first; the blocking work runs at the tail of the same loop turn, still occupying that turn but no longer holding up the handler that queued it. Note that `qb::io::async::callback(f)` with **no** delay does *not* do this — it runs `f` inline, inside the handler, which is the very thing you were trying to avoid. Only `callback(f, d)` with `d > 0` schedules.
- **Delegate to dedicated worker actors** — possibly on cores configured with higher latency — that perform the blocking operation and reply with an event.

See [Async operations in actors](../5_core_io_integration/async_in_actors.md).

## How do actors find each other without shared memory?

Through `ActorId` values, obtained in one of several ways:

- **At creation.** Capture the `ActorId` returned (or the handle) when you add an actor, and pass it to the actors that need to reach it.
- **In event payloads.** Include `id()` in an event so the receiver can reply.
- **Service actors.** A `ServiceActor<Tag>` is a per-core singleton. Same-core actors fetch a typed pointer with `getService<T>()`; any core resolves its id with `getServiceId<Tag>(core_id)`.
- **`require<T...>()`.** Ask the framework to locate live instances of given actor types; matching actors reply with `qb::RequireEvent` carrying their `ActorId` (`qb/src/qb/core/Actor.h:997-998`).
- **A custom registry actor** you write, acting as a naming service.

Patterns are detailed in [Actor patterns](../4_qb_core/patterns.md).

## My actors are unresponsive or the system is slow. What should I check?

In rough order of likelihood:

1. **A blocking call inside a handler.** The most common cause — see the blocking-library question above. No handler should run a long computation or a synchronous syscall.
2. **High `VirtualCore` latency.** A core configured with a high latency setting parks when idle, delaying event delivery. Lower it for latency-sensitive cores.
3. **A core pinned at 100% CPU.** Profile to find the actor in a tight loop. (A zero-latency core also spins when idle by design, which can look like saturation.)
4. **A logical deadlock.** The model prevents data races, not request cycles. Two actors each waiting on a reply from the other, with no timeout, will hang.
5. **Resource exhaustion** — memory, file descriptors, sockets.
6. **Oversized events copied by value.** Carry large payloads behind a smart pointer (see the `qb::string<N>` question).

See [Performance tuning](../6_guides/performance_tuning.md) and [The engine](../4_qb_core/engine.md).

## Which platforms and compilers are supported?

Continuous integration builds and tests every change on:

| OS | Compilers | Standard library | CI |
|---|---|---|---|
| Linux (`ubuntu-latest`) | GCC, Clang | libstdc++ | enabled |
| macOS (`macos-latest`) | Apple Clang | libc++ | enabled |
| Windows (`windows-latest`) | MSVC | MSVC STL | **currently disabled** |

> **Windows/MSVC is supported source, but is not exercised by CI right now.** The hosted-runner
> job is commented out in `.github/workflows/cmake.yml` (it took ~70 minutes and its tests timed
> out under that load, reporting runner noise rather than defects); the maintainer validates the
> full MSVC solution out of band before each release. Treat a Windows build as verified by you,
> not by this project's CI. See [INSTALL.md](../../INSTALL.md#supported-toolchains).

You need a C++20-capable compiler from one of those families, CMake 3.24 or newer, and — on non-Windows platforms — a POSIX threads (pthreads) implementation. Supported architectures are x86_64 and ARM64, including Apple Silicon. The full matrix and prerequisites are in [INSTALL.md](../../INSTALL.md).

## Where is the authoritative API reference?

The header files under `qb/src/qb/` are ground truth for every signature, template parameter, and default. The [API overview](./api_overview.md) is the map into them, the [glossary](./glossary.md) defines the vocabulary, and the [invariants](./core_invariants.md) pages record the contracts the runtime guarantees.

## See also

- [Error handling and resilience](../6_guides/error_handling.md) — the fail-stop boundary and supervision patterns in full
- [Messaging](../4_qb_core/messaging.md) — `push`/`send`/`broadcast`, event payloads, and ordering
- [C++20 coroutines](../3_qb_io/coroutines.md) — the coroutine layer `spawn_detached` runs on
- [The threading model](../2_core_concepts/threading_model.md) — why no mutexes are needed in actor code
- [SECURITY.md](../../SECURITY.md) · [SUPPORT.md](../../SUPPORT.md) · [CONTRIBUTING.md](../../CONTRIBUTING.md) · [INSTALL.md](../../INSTALL.md)
