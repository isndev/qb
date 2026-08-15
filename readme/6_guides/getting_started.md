# Getting started

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Install qb, build and run a first actor, add a non-blocking timer, and find the next page to read.

**Prerequisites:** a C++20 toolchain and CMake 3.24+ (see [Build requirements](#1-prerequisites)) — **See also:** [Building from source](../7_reference/building.md), [CMake and dependencies](../7_reference/cmake_dependencies.md), [Core concepts: the actor model](../2_core_concepts/actor_model.md), [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md)

## Summary

This page takes you from an empty directory to a running, multi-actor program. You will:

1. Confirm the toolchain qb needs.
2. Bring qb into a project — either by scaffolding a new one or by embedding the source tree.
3. Write a first actor, then a two-actor program that exchanges messages.
4. Add a non-blocking timer driven by the `qb-io` event loop.
5. Build, run, and check for engine errors.

Every API used here is part of `qb-core` (the actor engine) and `qb-io` (the asynchronous runtime). The two libraries compose: linking `qb::core` pulls in `qb::io`.

## 1. Prerequisites

| Requirement | Detail |
|---|---|
| C++20 compiler | CI builds with GCC and Clang on Linux and Apple Clang on macOS; the MSVC/Windows job is currently disabled and validated out of band. qb propagates a PUBLIC `cxx_std_${QB_CXX_STANDARD}` usage requirement, so consumers compile with the selected qb standard. Use `-DQB_CXX_STANDARD=23` to validate the C++23 path. |
| CMake | **3.24 or newer.** 3.24 is the floor because qb resolves fetchable dependencies with `FetchContent` + `find_package` integration. |
| Git | Required to clone qb and to fetch its submodules. GoogleTest and Google Benchmark are downloaded by `FetchContent` when tests or benchmarks are enabled. |
| OpenSSL (optional) | Enables SSL/TLS transports and the `qb::crypto`/`qb::jwt` utilities. Controlled by `QB_WITH_SSL` (default `ON`, auto-disabled if OpenSSL is absent). |
| zlib (optional) | Enables compression utilities. Controlled by `QB_WITH_COMPRESSION` (default `ON`). |

libev and stduuid ship bundled in the source tree and need no installation. For the full option list, see [Build requirements](../7_reference/building.md) and [CMake and dependencies](../7_reference/cmake_dependencies.md).

<!-- src: README.md (Requirements), qb/readme/7_reference/cmake_dependencies.md -->

## 2. Bring qb into a project

There are two on-ramps. Pick one.

### Option A — scaffold a new project

The `qb-new-project.sh` helper bootstraps a project from the `qb-sample-project` template. It clones the template into a scratch directory *outside* the directory you ran it in, copies that template's `template/` payload subtree into the name you pass, substitutes the name through file contents **and** path names, and commits the result once into a **fresh** git repository with no remote — not the template's history under a new name. There are no submodules to initialize: the generated tree fetches qb and the qbm modules with `FetchContent`, at a ref this script writes rather than one stored in the template.

```bash
curl -fsSL https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh | bash /dev/stdin MyProject
cd MyProject
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
```

> **Check what it printed before you build — the report is there to be read.** Two refs are resolved at scaffold time, and the script prints each one *with the reason it chose it*: `template : qb-sample-project @ <ref>` and `qb ref : <ref>`. Both come from one source of truth, `QB_SHIPPED_VERSION` in the script itself, which `qb/scripts/check-scaffold-consistency.sh` asserts equal to `QB_FRAMEWORK_VERSION` in `qb/cmake/qbConfig.cmake` — so the URL you fetch the script from selects the pairing (`.../qb/main/script/...` is the default branch, carrying a pre-release 3.0.0 until `v3.0.0` is tagged; `.../qb/<tag>/script/...` pins one release). The qb ref is `v<version>` when that tag exists and the `develop` branch when it does not. The template ref is the same ladder with one difference that matters — `v<version>`, then the development line **only while qb itself is unreleased**, then, **only as a last rung**, the template's own default branch. Since 2026-08-11 both templates carry `v3.0.0`, so the first rung matches and the later ones are never reached. That last rung is the one to watch: the script labels it `FALLBACK -- ... may not match qb <version>`, and a template from a different era is exactly how this rotted the first time. `QB_TEMPLATE_REF=<ref>` overrides the ladder outright, and `QB_REF=<ref>` does the same for the qb the generated tree builds against. This is the drift the old design could not see at all, because the answer used to be *stored* — a qb submodule gitlink committed in the template, which drifted two majors before anyone noticed. Nothing is stored now.

The script creates nothing outside `MyProject/`, refuses to run if that name is taken, aborts on the first failed step, and deletes what it made if it does not finish — worth knowing because the recommended invocation above pipes it into `bash` in whatever directory you happen to be standing in. Four anti-vacuity checks stand between the copy and the word `Created`: the template's payload directory must exist and be non-empty, the tree must actually have been personalised, no `@QB_...@` placeholder may be left unresolved, and the result must contain a `CMakeLists.txt`. Each aborts non-zero, so a template that yields an empty or unsubstituted tree fails loudly instead of exiting 0 over it — which is what a previous version of this script did.

`qb-new-module.sh`, in the same directory, is its module-side counterpart: it scaffolds a qbm module from the `qb-sample-module` template. Run it from your project's `qbm/` directory. Which revision of the template you get is resolved from the qb version the script ships with, not left to chance — see [Composing qbm modules](advanced_usage.md#composing-qbm-modules).

> **Both scripts are bash.** They declare `#!/usr/bin/env bash`, use `set -euo pipefail`, and shell out to `git` — so neither runs in `cmd.exe` or PowerShell. On Windows, invoke them from **WSL** or **Git Bash**. This is the only part of qb that needs a POSIX shell: once the project exists, the MSVC build path is unaffected (see [Platform notes](../7_reference/building.md#platform-notes)). Both are fetched from a branch, so the URL selects the version — `.../qb/main/script/...` is the default branch, carrying a pre-release 3.0.0 until `v3.0.0` is tagged; `.../qb/<tag>/script/...` pins one release. Downloading with `-o` and reading before running does the same thing without executing unreviewed code in your working directory.

<!-- src: qb/script/qb-new-project.sh:54, qb/script/qb-new-project.sh:71, qb/script/qb-new-project.sh:107-111, qb/script/qb-new-project.sh:134-144, qb/script/qb-new-project.sh:157-164, qb/script/qb-new-project.sh:184-187, qb/script/qb-new-project.sh:193-208, qb/script/qb-new-project.sh:226-267, qb/script/qb-new-project.sh:270-312, qb/script/qb-new-project.sh:328-343, qb/script/qb-new-project.sh:401-429, qb/script/qb-new-project.sh:431-451, qb/script/qb-new-project.sh:466-472, qb/script/qb-new-module.sh:66, qb/script/qb-new-module.sh:126-136, qb/script/qb-new-module.sh:149-156 -->

### Option B — embed qb in an existing CMake project

Clone qb with its submodules into your project, then add it as a subdirectory:

```bash
git clone --recursive https://github.com/isndev/qb.git
```

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.24)
project(my_app CXX)

add_subdirectory(qb)                                   # builds qb-core and qb-io
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE qb::core)         # qb::core pulls in qb::io
```

To consume an installed copy instead of embedding the source, replace `add_subdirectory(qb)` with `find_package(qb CONFIG REQUIRED)`, which provides the same `qb::core` and `qb::io` targets.

<!-- src: README.md (Integrate into your build) -->

## 3. Your first actor

An actor owns its state, subscribes to the event types it handles in `onInit()`, and reacts to each message one at a time. The program below defines one actor that sends an event to itself and terminates on receipt.

```cpp
// main.cpp
#include <qb/main.h>     // qb::Main — the engine
#include <qb/actor.h>    // qb::Actor base class, qb::ActorId
#include <qb/event.h>    // qb::Event base class
#include <qb/io.h>       // qb::io::cout — thread-safe console output

struct GreetingEvent : qb::Event {
    qb::string<64> message;
    explicit GreetingEvent(const char *msg) : message(msg) {}
};

class GreeterActor : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<GreetingEvent>(*this);     // subscribe to GreetingEvent
        push<GreetingEvent>(id(), "Hello");      // send one to self
        co_return true;                          // returning false aborts creation
    }

    void on(const GreetingEvent &event) {
        qb::io::cout() << "Received: " << event.message << '\n';
        kill();                                  // work done; terminate this actor
    }
};

int main() {
    qb::Main engine;
    engine.addActor<GreeterActor>(0);            // construct on core 0
    engine.start();                              // start worker threads (async)
    engine.join();                               // block until all actors stop
    return 0;
}
```

What each call does:

- **`engine.addActor<GreeterActor>(0)`** registers the actor for `VirtualCore` 0, reserves its `qb::ActorId`, and returns that ID immediately. The actor itself is constructed on the worker thread when `start()` runs. All actors must be added before `start()`.
- **`onInit()`** runs once, after construction and ID assignment, before any event is processed. Subscribe to your event types here with `registerEvent<T>(*this)`. It is an async coroutine — it may `co_await` (e.g. `co_await context().sleep(...)` / `co_await qb::ask(...)`); `co_return true` activates, `co_return false` or an uncaught exception fails init and immediately destroys the actor.
- **`push<GreetingEvent>(id(), "Hello")`** enqueues an event for the destination `ActorId` (here, `id()` — this actor itself). `push` guarantees ordered delivery from the same source to the same destination.
- **`on(const GreetingEvent &)`** is the handler the engine invokes when the subscribed event arrives.
- **`kill()`** marks the actor for termination after the current handler returns.
- **`engine.start()`** launches one worker thread per used core, then returns. **`engine.join()`** blocks until every actor has terminated.

`registerEvent`, `push`, `on`, and `kill` are members of `qb::Actor`; `addActor`, `start`, and `join` are members of `qb::Main`.

<!-- src: README.md (Your first actor), examples/01-actors/01-hello-actor.cpp -->

### The `start()` / `join()` contract

`Main::start(bool async = true)` has two modes:

- **`start()` (async, the default)** launches all `VirtualCore` worker threads and returns immediately. The calling thread is free; call `join()` to block until shutdown. This is the idiom used above.
- **`start(false)`** turns the calling thread into one of the worker threads and blocks until the engine stops. Use it when you do not want a separate main thread; in that mode there is no separate thread to `join()`.

Either way, check `engine.hasError()` after the engine stops to detect a core that terminated on an error.

<!-- src: qb/src/qb/core/Main.h:546-579 -->

## 4. A two-actor program: ping/pong

This expands the first example into two actors that exchange messages. `PongerActor` replies to each `PingEvent` with a `PongEvent`; `PingerActor` sends a fixed number of pings, then shuts both actors down.

```cpp
// ping_pong.cpp
#include <qb/main.h>
#include <qb/actor.h>
#include <qb/event.h>
#include <qb/io.h>

// --- Events ---
struct PingEvent : qb::Event {
    qb::ActorId from;   // so Ponger knows whom to reply to
    int value;
    PingEvent(qb::ActorId sender, int v) : from(sender), value(v) {}
};

struct PongEvent : qb::Event {
    int value;
    explicit PongEvent(int v) : value(v) {}
};

// --- Ponger: replies to each Ping with a Pong ---
class PongerActor : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<PingEvent>(*this);
        // qb::KillEvent is auto-subscribed by default; no need to register it.
        co_return true;
    }

    void on(const PingEvent &event) {
        qb::io::cout() << "Ponger received ping #" << event.value << '\n';
        push<PongEvent>(event.from, event.value);   // reply with the same value
    }
};

// --- Pinger: sends a fixed number of pings, then stops both actors ---
class PingerActor : public qb::Actor {
    qb::ActorId _ponger;
    int _sent = 0;
    static constexpr int _max = 3;

public:
    explicit PingerActor(qb::ActorId ponger) : _ponger(ponger) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<PongEvent>(*this);
        send_ping();
        co_return true;
    }

    void on(const PongEvent &event) {
        qb::io::cout() << "Pinger received pong #" << event.value << '\n';
        if (_sent < _max) {
            send_ping();
        } else {
            push<qb::KillEvent>(_ponger);   // ask Ponger to terminate
            kill();                         // terminate self
        }
    }

private:
    void send_ping() {
        ++_sent;
        push<PingEvent>(_ponger, id(), _sent);
    }
};

int main() {
    qb::Main engine;

    qb::ActorId ponger = engine.addActor<PongerActor>(0);
    if (!ponger.is_valid()) {
        qb::io::cout() << "Failed to add PongerActor\n";
        return 1;
    }

    qb::ActorId pinger = engine.addActor<PingerActor>(0, ponger);
    if (!pinger.is_valid()) {
        qb::io::cout() << "Failed to add PingerActor\n";
        return 1;
    }

    engine.start();
    engine.join();

    return engine.hasError() ? 1 : 0;
}
```

Notes on the new pieces:

- **`addActor` returns `ActorId::NotFound` on failure** (for example, if a core is full). `ActorId::is_valid()` checks for that sentinel, so guard the returned IDs before using them.
- **`qb::KillEvent` is auto-subscribed.** Every actor subscribes to `qb::KillEvent`, `qb::SignalEvent`, `qb::UnregisterCallbackEvent`, `qb::PingEvent`, and `qb::RequireEvent` at construction, so neither actor registers `KillEvent` explicitly. The default `on(const KillEvent &)` handler calls `kill()`. (`qb::PingEvent` is the framework's `require<>` health-check event, distinct from this example's own `PingEvent`, which is *not* auto-subscribed — that is why `PongerActor` still registers it. To opt out of the five default subscriptions, construct with `qb::no_default_events` — and register `qb::SignalEvent` yourself, since that, not `KillEvent`, is how `Main::stop()` and the terminal signals reach an actor.)
- **`push<qb::KillEvent>(_ponger)`** sends a kill request to another actor; the receiver's default handler terminates it gracefully.

Both actors run on core 0 here. To distribute them across cores, pass a different `CoreId` to `addActor`; messages cross cores over lock-free queues with no code change. See [the threading model](../2_core_concepts/threading_model.md).

<!-- src: examples/01-actors/02-messaging.cpp (the shipped request/response program this section mirrors), qb/src/qb/core/Actor.cpp:114-125,168-171, qb/src/qb/core/ActorId.h:401,442, qb/src/qb/core/Main.h:611 -->

## 5. Add a non-blocking timer

Actor handlers run on the `VirtualCore`'s event-loop thread, so they must not block. To run code after a delay, spawn a coroutine with `Actor::spawn` and `co_await` its context's `sleep`. The coroutine *suspends* rather than blocking; the core keeps serving every other actor while it waits, and resumes it between event deliveries.

`spawn` takes a lambda whose one parameter is a `qb::ScopedCoroContext`. That context is bound to this actor's cancellation scope, and it carries the actor's `ActorId` **by value** — which is what makes the pattern safe: the delay is tied to the actor's lifetime, and the work that follows it comes back through the mailbox rather than through a pointer:

```cpp
#include <qb/main.h>
#include <qb/actor.h>
#include <qb/event.h>
#include <qb/io.h>
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

struct TickEvent : qb::Event {};

class HeartbeatActor : public qb::Actor {
    int _ticks = 0;

public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<TickEvent>(*this);
        schedule_tick();
        co_return true;
    }

    // The tick lands here, in an ordinary handler, on a demonstrably live actor.
    void on(const TickEvent &) {
        qb::io::cout() << "tick " << ++_ticks << '\n';
        if (_ticks < 3)
            schedule_tick();   // re-arm for the next tick
        else
            kill();
    }

private:
    void schedule_tick() {
        // Sleep ~500 ms on the event loop, then send ourselves a TickEvent.
        // Capture nothing — never `this`.
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(500ms);
            ctx.template push<TickEvent>();
        });
    }
};

int main() {
    qb::Main engine;
    engine.addActor<HeartbeatActor>(0);
    engine.start();
    engine.join();
    return engine.hasError() ? 1 : 0;
}
```

Two rules make this shape the one to copy:

- **Capture by value; never capture `this`.** `ctx` holds the actor's id, not its address, so the resumed body can only reach the actor through the mailbox — and an event addressed to a dead actor is simply dropped.
- **`kill()` cancels a pending `ctx.sleep`.** The sleep is armed against the actor's cancellation scope, and `kill()` cancels that scope, so the coroutine unwinds instead of resuming into a destroyed actor.

The primitive you may have expected here, `qb::io::async::callback(func, delay)`, is deliberately *not* what this section teaches. Its timer is owned by the event loop, not by any actor: it fires whenever the loop says so, whatever happened to the actor in the meantime, so `[this]` in its lambda is a use-after-free waiting for the right timing. Adding `if (!is_alive()) return;` does not rescue it — `is_alive()` is a read of an actor member, so on a destroyed actor *evaluating the guard is itself the invalid access*. Reach for `callback` only for work that must deliberately outlive the actor; when you want a timer you can cancel by hand, use `qb::io::async::scoped_callback(func, delay)` and keep the returned `std::unique_ptr` as an actor member, so destroying the actor destroys the timer. Note also that a non-positive delay — and the single-argument overload `callback(func)` — runs `func()` **inline at the call site**, scheduling nothing; `qb::io::async::defer(func)` is the one that waits for the current handler to unwind.

For inactivity timeouts, coroutine-based async flows, and the full event-loop surface available to actors, see [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md).

<!-- src: qb/src/qb/io/async/io.h:358-382, qb/src/qb/io/async/io.h:312-318,343,479-484, qb/src/qb/core/Actor.h:1238-1239,1717-1719, qb/src/qb/core/Actor.cpp:205-208,283-289, examples/01-actors/06-doing-things-later.cpp, examples/01-actors/06-doing-things-later.cpp:246-249 -->

## 6. Build and run

Place your source next to a `CMakeLists.txt` that embeds qb (see [Option B](#option-b--embed-qb-in-an-existing-cmake-project)), then:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
./build/my_app
```

On Windows with a multi-config generator, the executable lands under a configuration subdirectory, for example `build/Release/my_app.exe`.

Expected output for the ping/pong program:

```text
Ponger received ping #1
Pinger received pong #1
Ponger received ping #2
Pinger received pong #2
Ponger received ping #3
Pinger received pong #3
```

A non-zero exit code means `engine.hasError()` reported a core that terminated on an error.

## Pitfalls

- **Add all actors before `start()`.** `Main::core()` and `Main::addActor()` are only valid before the engine runs; `Main::core()` throws `std::runtime_error` once the engine is running.
- **Never construct an actor directly.** Actors must be created from inside a `VirtualCore` worker thread via `Main::addActor<T>(...)` (or `core(idx).addActor<T>(...)`). The constructor asserts it is running on a worker thread.
- **A core with zero actors fails startup.** Startup reports `Error::NoActor` if any used core has no actors; surface it with `hasError()` after the engine stops.
- **`push`/`send`/`broadcast` are `noexcept`.** An allocation failure while enqueuing cannot be reported and calls `std::terminate()`. Keep events small and allocation-light.
- **Do not block in a handler or callback.** `on(...)` handlers and `qb::io::async::callback` bodies run on the core's event-loop thread; a blocking call stalls every actor on that core. Use the async surface in [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) instead.
- **Subscribe with `registerEvent<T>` in `onInit()`, not your constructor.** `onInit()` is the documented initialization hook: it runs once the actor is fully appended to its core, and `co_return false` from it aborts creation cleanly. A constructor cannot signal initialization failure that way.

<!-- src: qb/src/qb/core/Main.cpp:484-486 (Main::core throws while running), :341-343 (Error::NoActor for a 0-actor core), qb/src/qb/core/Actor.cpp:114-125 (ctor asserts the worker thread), qb/src/qb/core/Actor.h:334-336 (onInit is where registerEvent belongs) -->

## Where to go next

- **[Core concepts: the actor model](../2_core_concepts/actor_model.md)** — actors, events, lifecycle, and the guarantees that follow from share-nothing state.
- **[Core concepts: the event system](../2_core_concepts/event_system.md)** — `push` vs `send`, ordered delivery, `reply`/`forward`, and broadcasts.
- **[Threading model](../2_core_concepts/threading_model.md)** — placing actors on cores and how inter-core messaging works.
- **[Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md)** — timers, deferred callbacks, and coroutines from within an actor.
- **[Patterns cookbook](./patterns_cookbook.md)** and **[Performance tuning](./performance_tuning.md)** — recurring designs and how to make them fast.
- **Worked examples** ship under `examples/` in the repository: `examples/01-actors/` (actors), `examples/02-io/` (async I/O), and `examples/05-services/` (the two combined). They build with the framework.
