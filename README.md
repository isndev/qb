<!-- Verified-against: qb 3.1.0 (C++20 default, C++23 supported) -->

# qb Actor Framework

<p align="center"><img src="./resources/logo.svg" width="180px" alt="qb Actor Framework logo" /></p>

qb is a C++20 framework for concurrent and distributed systems built on the actor model. It pairs
share-nothing actors with a non-blocking I/O runtime and native C++20 coroutines, so application code
says *what* happens on each message while the runtime owns scheduling, multicore placement and I/O.
It is also measured: against CAF and SObjectizer on the Savina suite, qb is the fastest of the three
in every one of the 64 cells on each of four hosts, and at or under a raw-thread floor in most
two-core cells — the numbers, the protocol and the losses are [below](#measured).

[![C++20/23](https://img.shields.io/badge/C%2B%2B-20%2F23-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.24+-blue.svg)](https://cmake.org/)
[![Platforms](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](#platforms-and-toolchains)
[![Architectures](https://img.shields.io/badge/Arch-x86__64%20%7C%20ARM64-lightgrey.svg)](#platforms-and-toolchains)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](./LICENSE)

## Hello, actor

```cpp
#include <qb/main.h>
#include <qb/actor.h>
#include <qb/io.h>

struct Greeting : qb::Event {
    qb::string<64> text;
    explicit Greeting(const char *t) : text(t) {}
};

class Greeter : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<Greeting>(*this);      // subscribe
        push<Greeting>(id(), "hello");       // send to self, in order
        co_return true;                      // the actor is live
    }
    void on(const Greeting &e) {
        qb::io::cout() << e.text << '\n';
        kill();                              // done
    }
};

int main() {
    qb::Main engine;
    engine.addActor<Greeter>(0);             // core 0
    engine.start();
    engine.join();                           // until every actor has stopped
    return engine.hasError() ? 1 : 0;
}
```

No mutex, no condition variable, no shared queue: an actor owns its state and is reached only by
messages, which the engine delivers in order and processes one at a time.

The one oddity is deliberate. The payload is `qb::string<64>`, not `std::string`, because an event is
moved by raw `memcpy` — the bytes are copied to a new address and the source is abandoned without a
destructor — so no member may point into its own storage. A short `std::string` on libstdc++ does
(its inline buffer), and is therefore illegal as a by-value payload; [`qb::string<N>`](./readme/0_foundations/containers.md),
plain data, `std::unique_ptr`, `std::shared_ptr` and `std::vector` are all fine. C++20 has no
`is_trivially_relocatable`, so there is no compile-time check —
[Inter-actor messaging](./readme/4_qb_core/messaging.md) has the mechanism and its debug-only guard.

## Ask another actor, and await the answer

Request/reply is a free function and a `co_await`. The awaitable lives in the caller's frame: no
coroutine frame is allocated for the ask, the timeout is a deadline in the core's own clock, and the
reply comes back through the ordinary handler.

```cpp
#include <qb/main.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>

struct Quote : qb::Request<int> {             // the reply type is the template argument
    int symbol{0};
    explicit Quote(int s) : symbol(s) {}
};

class Market : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override { registerEvent<Quote>(*this); co_return true; }
    void on(Quote &q) { qb::answer(*this, q, [](Quote const &r) { return r.symbol * 100; }); }
};

class Trader : public qb::Actor {
    qb::ActorId _market;
public:
    explicit Trader(qb::ActorId market) : _market(market) {}
    qb::io::async::task<bool> onInit() override {
        registerEvent<Quote>(*this);
        auto market = _market;                // capture by value, never `this`
        spawn([market](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto reply = co_await qb::ask<Quote>(ctx, market, std::chrono::milliseconds{500}, 42);
            qb::io::cout() << "quote " << reply.response << '\n';
            ctx.push_to<qb::KillEvent>(market);   // the two of us are done
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
    void on(Quote &q) { resolve_ask(q); }     // routes the reply to the waiting coroutine
};

int main() {
    qb::Main engine;
    auto market = engine.addActor<Market>(0);
    engine.addActor<Trader>(1, market);       // a second core: the reply crosses a thread
    engine.start();
    engine.join();
    return engine.hasError() ? 1 : 0;
}
```

`qb::ask` throws `timeout_error` if the deadline passes and `cancelled_error` if the actor is killed
while waiting; a reply that lands after the timeout reaches `on(Quote &)` as an ordinary, unsolicited
event, which is what `resolve_ask` returning `false` means. Streams (`qb::ask_stream`), scatter-gather
(`qb::ask_all`, `qb::ask_any`), discovery (`qb::require<T>`) and deadlines shared across a chain
(`qb::ask_by`) are in the [pattern library](./readme/4_qb_core/patterns_library.md).

## The model in three pictures

**One thread per VirtualCore.** A `qb::VirtualCore` is a worker thread that owns its actors and drives
exactly one event loop. The loop polls sockets, fires timers, resumes coroutines and dispatches your
handlers, in the same pass and the same thread. The only cross-thread channel is the mailbox.

```mermaid
flowchart TB
    subgraph VC0["VirtualCore 0 — one worker thread"]
        direction TB
        A0["your actors — one event at a time, in order"]
        C0["qb-core: scheduling · mailboxes · actor lifecycle"]
        I0["qb-io: one event loop — sockets · timers · files · coroutines"]
        A0 --> C0 --> I0
    end
    subgraph VC1["VirtualCore 1 — one worker thread"]
        direction TB
        A1["your actors"]
        C1["qb-core"]
        I1["qb-io"]
        A1 --> C1 --> I1
    end
    VC0 <-- "lock-free MPSC mailboxes — the only cross-thread channel" --> VC1
```

**Where an event goes.** A `qb::ActorId` is `{ServiceId, CoreId}` in 32 bits, and the core half *is*
the routing decision: a send resolves the destination core and appends bytes to a buffer dedicated to
it. Nothing looks an actor up across a thread boundary, and the actor state path carries no lock, no
atomic and no fence.

```mermaid
flowchart LR
    P["push&lt;E&gt;(target, args...)"] --> R{"target on this core?"}
    R -- "yes" --> L["this core's pipe — built in place, no copy, no lock"]
    R -- "no" --> M["that core's mailbox — one lock-free ring per producer core"]
    L --> D["next pass: on(E &amp;) on the target actor"]
    M --> D
```

**What `co_await` gives back.** A coroutine suspends by *returning*: the stack unwinds to the
scheduler and the loop, every other actor and session on the core gets its turn, and the frame resumes
on the same thread when its reply, timer or socket is ready. Sequential-looking code compiles to a
state machine, not a thread.

```mermaid
sequenceDiagram
    participant H as your handler
    participant S as coroutine scheduler
    participant L as the core's event loop
    participant O as other actors on the core
    H->>S: co_await qb::ask / ctx.sleep / a socket read
    S->>L: frame parked, stack unwound
    L->>O: the same pass keeps dispatching
    L-->>S: the reply lands (or the timer fires, or the socket is readable)
    S-->>H: resumed on the same thread, no lock taken
```

## Why qb

- **One address, one decision.** Routing is a 32-bit compare, and the state path is lock-free by
  construction, not by discipline: `_alive` is a plain `bool` on purpose
  ([threading model](./readme/2_core_concepts/threading_model.md)).
- **One loop, two jobs.** I/O is not a subsystem an actor talks to; it is the same crank that
  dispatches events. A server actor is a mixin over its own loop, and a database client held by an
  actor needs no pump ([network actors](./readme/5_core_io_integration/network_actors.md)).
- **Coroutines that give the core back.** The 2-core park cell of the ping-pong benchmark went from
  26.24 µs to 153 ns per round trip when the park moved inside the loop and the wake stopped being a
  lost signal — a throughput property, measured, not a style
  ([`run_sync` belongs only where the thread is yours to block](./readme/5_core_io_integration/async_in_actors.md#the-two-call-chains)).
- **The type system catches the time bugs.** `qb::duration` is `std::chrono::nanoseconds`, so
  `setLatency(500)` does not compile; `qb::mono_time` and `qb::wall_time` are distinct types, so a
  monotonic deadline can never be compared to wall-clock time
  ([the time vocabulary](./readme/0_foundations/time.md)).

### What it costs

Four things, honestly. None of them produces a compile error, which is exactly why the documentation
exists:

| Cost | Where it is documented |
|---|---|
| An event is `memcpy`-relocated and its source destructor never runs, so a payload must be trivially **relocatable**, not merely copyable — and C++20 has no trait for that. | [messaging.md](./readme/4_qb_core/messaging.md) |
| The reference `push` returns lives until your **handler returns** — across further pushes, since the pipe is segmented and never moves an event — but not across a `co_await` and never in a member. | [buffers.md](./readme/0_foundations/buffers.md) |
| Blocking the calling thread inside a handler freezes every actor on that core, with no diagnostic. | [async_in_actors.md](./readme/5_core_io_integration/async_in_actors.md) |
| The runtime allocates in proportion to the square of the core count and never shrinks — 22.5 MiB at rest on 8 cores. | [buffers.md](./readme/0_foundations/buffers.md) |

## Measured

The comparison lives in its own repository, [qb-vs-others](https://github.com/isndev/qb-vs-others):
qb, [CAF](https://github.com/actor-framework/actor-framework) 1.1.0 and
[SObjectizer](https://github.com/stiffstream/sobjectizer) 5.8.5.1 on the
[Savina](https://github.com/shamsimam/savina) shapes, every framework built from source in one
project under one set of flags, every worker pinned through its own public API, every cell verified
by the harness, and a raw `std::thread` + SPSC-ring **floor** that is not a framework and is never
ranked as one. Its `FAIRNESS.md` is the deliverable; the tables are what it produced.

One core, spin, nanoseconds per unit — qb, the second-fastest framework, the floor (WSL2 Debian 13
g++ 14.2 and Windows 11 MSVC 19.51 on one i9-12900K, pinned, 9 repetitions + 2 warm-up, one quiet
session per host, 2026-09-13):

| shape (unit) | g++-14: qb · second · floor | MSVC 19.51: qb · second · floor |
|---|---|---|
| ping-pong (round trip) | **22** · SObjectizer 139 · 2 | **30** · SObjectizer 182 · 2 |
| counting (message) | **7** · SObjectizer 105 · 3 | **10** · SObjectizer 139 · 3 |
| thread-ring (hop) | **17** · SObjectizer 70 · 3 | **18** · SObjectizer 91 · 9 |
| fork-join (message) | **8** · SObjectizer 103 · 3 | **11** · SObjectizer 139 · 5 |
| big (round trip) | **18** · SObjectizer 133 · 7 | **17** · SObjectizer 183 · 11 |

Two cores, the cells that decide a ranking, measured as a census of twelve interleaved launches
(median of the launch medians; the floor's SPSC ring pays one cache line per message where qb's
producer publishes a batch):

| cell | qb | CAF | floor |
|---|---|---|---|
| ping-pong 2c, g++-14 | **155.9** | 279.6 | 182.9 |
| thread-ring 2c, g++-14 | **75.4** | 140.3 | 103.1 |
| ping-pong 2c, MSVC | **186.8** | 475.8 | 180.8 |
| thread-ring 2c, MSVC | **105.1** | 235.4 | 112.4 |
| ping-pong 2c, Apple M4 Pro (unpinned) | **196.4** | 386.6 | 237.7 |
| ping-pong 2c, arm64 Linux guest | **172.0** | 297.4 | 207.1 |

Across all eight shapes and four hosts qb is the fastest framework in every cell; the geometric mean
of qb over the best rival is 0.13 on the x86-64 hosts, 0.105 on macOS and 0.139 on the arm64 guest.
CAF's two-core cells run on one thread and still lose to a two-thread one by 1.8–2.5×.

**Where qb loses, named.** Not against a framework, and not against its previous release, but
against the floor in two kinds of cell and against itself between compilers: the one-core cells
whose floor is a bare function call (qb pays 7–22 ns per unit for the mailbox, the pipe and the
dispatch, 2.4–2.7× a floor of a few nanoseconds); `fib`, an actor created and destroyed per unit, and
`bank-transaction`, an `ask` round trip per transfer, at 3–4× their floors even after the actor arena
and the frame-free ask of 3.2; and MSVC against g++ on the same source, +28 % at one core, half of
which clang-cl recovers. The full account is `docs/TUNING.md` in that repository, §13.5 and §13.9,
and each host's `results/<host>/README.md` carries its own session, controls and censuses.

## Install

**FetchContent**, no submodules:

```cmake
include(FetchContent)
FetchContent_Declare(qb GIT_REPOSITORY https://github.com/isndev/qb.git
                        GIT_TAG main)        # main is the released line; pin a vX.Y.Z tag in production
FetchContent_MakeAvailable(qb)
target_link_libraries(my_app PRIVATE qb::core qb::io)
```

**A source tree you already have**, or **an installed copy**:

```cmake
add_subdirectory(qb)                          # embed
target_link_libraries(my_app PRIVATE qb::core qb::io)

find_package(qb CONFIG REQUIRED)              # or consume a prefix built with QB_INSTALL=ON
target_link_libraries(my_app PRIVATE qb::core qb::io)
```

**A whole project**, from the [`qb-sample-project`](https://github.com/isndev/qb-sample-project)
template — fetch the script, read it, run it; it creates nothing outside the directory it names:

```bash
curl -fsSL https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh -o qb-new-project.sh
bash qb-new-project.sh MyProject && cd MyProject
cmake -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --parallel && ctest --test-dir build
```

The generated project fetches qb and the modules at the release the script ships with and carries a
ctest suite and a CI workflow; `qb-new-module.sh` does the same for a module. Both are bash (WSL or
Git Bash on Windows). [INSTALL.md](./INSTALL.md) has the toolchain notes, the installed-prefix
contract and the scaffolding details.

**Build qb itself**:

```bash
git clone --recursive https://github.com/isndev/qb.git && cd qb
cmake -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Platforms and toolchains

| OS | Architectures | Compilers | Standard library | Event loop backend | Verified by |
|---|---|---|---|---|---|
| Linux | x86-64, arm64 | GCC 14, Clang 19 and 22 | libstdc++ | epoll (default); io_uring at parity, opt-in; a parked core wakes under a millisecond on Linux ≥ 5.11 | CI, every push, `epoll` and `iouring` |
| macOS | Apple Silicon, x86-64 | Apple Clang | libc++ | kqueue | CI, every push |
| Windows | x86-64 | MSVC 19.5x, clang-cl | MSVC STL | wepoll over IOCP | the maintainer's gate before each release (`dev/agent/verify-windows.ps1`); the hosted CI job is disabled on purpose, and [INSTALL.md](./INSTALL.md#supported-toolchains) says why |

Clang older than 22 folds a by-value coroutine parameter into the caller's `byval` slot at the wrong
alignment (LLVM issue 159571); qb's own patterns carry the shield, and the Clang 19 lane exists to
prove it. Thread pinning is best-effort: `setAffinity` asks the OS and a refusal only warns, and on
Apple Silicon nothing is pinned — branch on `qb::CPU::ThreadPinningSupported()`, never on the call
returning ([the engine](./readme/4_qb_core/engine.md)).

## Two libraries, three modules, one loop

- **`qb-io`** — the runtime: an event loop, non-blocking TCP/UDP/SSL/QUIC transports, a protocol
  layer, C++20 coroutines, timers, file watching, and utilities (time, crypto, compression,
  containers). It has no reference to actors and stands on its own in any event-driven C++20 program.
- **`qb-core`** — the actor engine on top of it: lightweight actors, a typed event system with
  ordered delivery, multicore scheduling, lock-free inter-core messaging.
- **[qbm-http](https://github.com/isndev/qbm-http)**, **[qbm-pgsql](https://github.com/isndev/qbm-pgsql)**,
  **[qbm-redis](https://github.com/isndev/qbm-redis)** — HTTP/1.1, HTTP/2, HTTP/3 and WebSocket;
  PostgreSQL; Redis. Each is something an actor *composes*, not a client it calls: `qb::http::Server<>`
  is a mixin, a `qb::pg::tcp::database` held by an actor needs no pump, because the core's loop pass
  already carries its bytes. None speaks its wire protocol through a third-party client.
- **[qev](https://github.com/isndev/qev)** — the event loop, a maintained libev fork (kqueue,
  epoll with `epoll_pwait2`, io_uring, a real epoll on Windows through wepoll), published on its own
  under libev's API and held byte-identical with the copy qb embeds.
- **[qb-examples](https://github.com/isndev/qb-examples)** — 97 runnable programs in seven tiers,
  each with a checked header block and an expected output, run as part of the release gate.

```cmake
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")   # the modules live under qbm/<name>
target_link_libraries(my_app PRIVATE qbm::http)
```

## Build options

| Option | Default | Purpose |
|---|---|---|
| `QB_WITH_SSL` | `ON` | SSL/TLS and crypto (OpenSSL); auto-disabled if OpenSSL is absent |
| `QB_WITH_COMPRESSION` | `ON` | Compression (zlib) |
| `QB_WITH_QUIC` | `AUTO` | QUIC/HTTP3 via ngtcp2, on when found |
| `QB_WITH_LOGGING` | `ON` | Logging support |
| `QB_BUILD_TESTS` | `ON` | Build the test suite |
| `QB_BUILD_BENCHMARKS` | `OFF` | Build the benchmarks (Google Benchmark) |
| `QB_ENABLE_NATIVE_ARCH` | `OFF` | `-march=native`; only for host-local builds |

The gates are real `#ifdef` boundaries in the installed headers and propagate `PUBLIC` to your
target, so what your code can reach matches what was compiled; a build whose OpenSSL or zlib was not
found no longer compiles as if it had been. Requirements: a C++20 compiler (C++23 with
`-DQB_CXX_STANDARD=23`), CMake 3.24; the loop, stduuid, nanolog and the ska flat-hash-map are bundled;
GoogleTest and Benchmark are fetched on demand; **nlohmann/json is a real dependency** — `qb::json`
*is* `nlohmann::json`, a system copy ≥ 3.11 is used when found and a pinned tag fetched otherwise,
and an installable build needs the system copy. The complete list is
[CMake options](./readme/7_reference/cmake_options.md).

## Documentation

- **[The book](./readme/README.md)**, in reading order: [Foundations](./readme/0_foundations/),
  [Introduction](./readme/1_introduction/), [Core concepts](./readme/2_core_concepts/),
  [qb-io](./readme/3_qb_io/), [qb-core](./readme/4_qb_core/),
  [Integration](./readme/5_core_io_integration/), [Guides](./readme/6_guides/),
  [Reference](./readme/7_reference/).
- **Two pages worth opening first:** [Core invariants](./readme/7_reference/core_invariants.md) —
  the contract you owe the runtime and the one it owes you, each cited to what enforces it — and
  [Asynchronous work inside an actor](./readme/5_core_io_integration/async_in_actors.md), the
  `co_await` and `run_sync` call chains side by side against the loop pass.
- **For coding agents:** [`llms.txt`](./llms.txt) (the index and the five rules that decide whether
  generated qb code is correct) and [`llms-full.txt`](./llms-full.txt) (the mental model and the
  deterministic API reference, ~34k tokens), generated from `llm/` and checked in CI. Over MCP with
  nothing to host: `{ "mcpServers": { "qb": { "url": "https://gitmcp.io/isndev/qb" } } }`.
- **Policies:** [INSTALL](./INSTALL.md) · [VERSIONING](./VERSIONING.md) · [CHANGELOG](./CHANGELOG.md)
  · [SECURITY](./SECURITY.md) · [SUPPORT](./SUPPORT.md) · [CONTRIBUTING](./CONTRIBUTING.md)

## Versioning and compatibility

Semantic versioning on the public API. `main` is exactly the last release and `develop` the next
one; the version has one source of truth, `QB_FRAMEWORK_VERSION` in
[`cmake/qbConfig.cmake`](./cmake/qbConfig.cmake), from which every other version string is derived.
The three modules and the examples ship in lockstep with qb; qev versions on its own. A link-time
fingerprint refuses to combine a library and a consumer built with different feature gates
([VERSIONING.md](./VERSIONING.md)).

## License

Apache License, Version 2.0 — [LICENSE](./LICENSE). qb builds on
[libev](http://software.schmorp.de/pkg/libev.html), [stduuid](https://github.com/mariusbancila/stduuid),
[nlohmann/json](https://github.com/nlohmann/json), [OpenSSL](https://www.openssl.org/),
[Argon2](https://github.com/P-H-C/phc-winner-argon2), [zlib](https://zlib.net/), and the ska
flat-hash-map and nanolog designs; every bundled component and its license is inventoried in
[THIRD-PARTY-NOTICES](./THIRD-PARTY-NOTICES), installed alongside the library.
