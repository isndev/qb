<!-- Verified-against: qb 2.0.0 (C++20 default, C++23 supported) -->
# qb Actor Framework

<p align="center"><img src="./resources/logo.svg" width="180px" alt="qb Actor Framework logo" /></p>

qb is a C++20-first framework with optional C++23 support for building concurrent and distributed systems on the actor model. It pairs
share-nothing actors with a non-blocking asynchronous I/O engine and native C++20 coroutines, so
application code expresses *what* should happen on each message while the runtime handles scheduling,
multicore distribution, and non-blocking I/O.

[![C++20/23](https://img.shields.io/badge/C%2B%2B-20%2F23-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.24+-blue.svg)](https://cmake.org/)
[![Platforms](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](#platform-support)
[![Architectures](https://img.shields.io/badge/Arch-x86__64%20%7C%20ARM64-lightgrey.svg)](#platform-support)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](./LICENSE)

## At a glance

The framework ships as two libraries that compose:

- **`qb-core`** — the actor engine: lightweight actors, a type-safe event system with ordered delivery,
  multicore scheduling with CPU affinity, and lock-free inter-core message passing.
- **`qb-io`** — the asynchronous runtime: a libev-based event loop, non-blocking TCP/UDP/SSL transports,
  an extensible protocol layer, C++20 coroutines, timers, filesystem watching, and utilities (time,
  crypto, compression, containers).

Higher-level protocols (HTTP/1.1, HTTP/2, and HTTP/3, WebSocket, PostgreSQL, Redis) are provided as optional
[qbm modules](#module-ecosystem) built on this foundation.

## Quick start

### Generate a project

The `qb-new-project.sh` helper scaffolds a buildable project:

```bash
curl -fsSL https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh | bash /dev/stdin MyProject
cd MyProject
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
./build/qb-sample-project
```

### Your first actor

```cpp
#include <qb/main.h>
#include <qb/actor.h>
#include <qb/io.h>

struct GreetingEvent : qb::Event {
    qb::string<64> message;
    explicit GreetingEvent(const char *msg) : message(msg) {}
};

class GreeterActor : public qb::Actor {
public:
    bool onInit() final {
        registerEvent<GreetingEvent>(*this);   // subscribe
        push<GreetingEvent>(id(), "Hello");     // send to self
        return true;                            // actor is ready
    }

    void on(const GreetingEvent &event) {
        qb::io::cout() << "Received: " << event.message << '\n';
        kill();                                 // work done; terminate this actor
    }
};

int main() {
    qb::Main engine;
    engine.addActor<GreeterActor>(0);           // run on core 0
    engine.start();                             // start the engine
    engine.join();                              // block until all actors stop
    return 0;
}
```

No mutexes, condition variables, or shared queues: actors own their state and communicate only by
messages, which the engine delivers in order and processes one at a time per actor.

## Why qb

- **Share-nothing concurrency.** Each actor owns its state. Because actors never touch each other's
  memory and process their mailbox sequentially, whole classes of data races and deadlocks cannot occur
  by construction.
- **Asynchronous by default.** `qb-io` runs network, timer, and filesystem events on a non-blocking event
  loop, so a core stays busy doing useful work instead of waiting on syscalls.
- **Coroutines without a runtime tax.** `co_await` integrates with the event loop, letting you write
  sequential-looking async flows that compile to state machines, not threads.
- **Multicore from the same code.** Place actors on specific cores; messages move between cores over
  lock-free queues. See [the threading model](./readme/2_core_concepts/threading_model.md).
- **Modern, explicit C++20/23.** Clean CRTP-based APIs, RAII lifetimes, and a `std::chrono` time vocabulary
  (`qb::duration`, `qb::mono_time`, `qb::wall_time`).

## Architecture

```
        Your actors  ─────────────────────────────────────────────
                          │ events (ordered, one-at-a-time)
        ┌─────────────────▼──────────────────┐
        │  qb-core   actors · scheduling ·    │   one VirtualCore per worker thread,
        │            lock-free mailboxes      │   pinned to a CPU core
        └─────────────────┬──────────────────┘
        ┌─────────────────▼──────────────────┐
        │  qb-io     event loop · transports ·│   non-blocking I/O, timers,
        │            coroutines · protocols   │   coroutines (libev under the hood)
        └─────────────────────────────────────┘
```

Concepts are introduced in [Core concepts](./readme/2_core_concepts/); the engine internals are covered in
[qb-core](./readme/4_qb_core/) and the runtime in [qb-io](./readme/3_qb_io/).

## Module ecosystem

Optional modules add application protocols on top of qb. Each is a separate repository, added as a
submodule and discovered by CMake:

| Module | Provides |
|---|---|
| [qbm-http](https://github.com/isndev/qbm-http) | HTTP/1.1, plus HTTP/2 on SSL-enabled builds and HTTP/3 on QUIC builds, routing, middleware, authentication, and WebSocket (RFC 6455) |
| [qbm-pgsql](https://github.com/isndev/qbm-pgsql) | Asynchronous PostgreSQL client with prepared statements and transactions |
| [qbm-redis](https://github.com/isndev/qbm-redis) | Asynchronous Redis client covering the full command surface |

```bash
git submodule add https://github.com/isndev/qbm-http qbm/http
```

```cmake
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")
target_link_libraries(my_app PRIVATE qbm::http)
```

## Building

### Requirements

- A C++20 compiler by default. C++23 is supported with `-DQB_CXX_STANDARD=23` for validating newer
  standard-library paths.
- CMake **3.24** or newer.
- libev and stduuid ship bundled and need no installation. GoogleTest and Google Benchmark are fetched
  automatically when tests or benchmarks are enabled.

### Integrate into your build

```cmake
# Embed the source tree
add_subdirectory(qb)
target_link_libraries(my_app PRIVATE qb::core qb::io)

# …or consume an installed copy
find_package(qb CONFIG REQUIRED)   # provides qb::core and qb::io
```

### Build from source

```bash
git clone --recursive https://github.com/isndev/qb.git
cd qb
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Common options

| Option | Default | Purpose |
|---|---|---|
| `QB_WITH_SSL` | `ON` | SSL/TLS and crypto (OpenSSL); auto-disabled if OpenSSL is absent |
| `QB_WITH_COMPRESSION` | `ON` | Compression (zlib) |
| `QB_WITH_QUIC` | `AUTO` | QUIC/HTTP3 via ngtcp2: `AUTO` enables it when found |
| `QB_WITH_LOGGING` | `ON` | Logging support |
| `QB_BUILD_TESTS` | `ON` | Build the test suite |
| `QB_BUILD_BENCHMARKS` | `OFF` | Build benchmarks (Google Benchmark) |
| `QB_ENABLE_NATIVE_ARCH` | `ON` | Tune codegen for the build host (`-march=native`); turn **off** for portable binaries |

The complete option list is in [CMake options](./readme/7_reference/cmake_options.md); installation details
are in [INSTALL.md](./INSTALL.md).

## Platform support

Continuous integration builds and tests every change on:

| OS | Compilers | Standard library |
|---|---|---|
| Linux (`ubuntu-latest`) | GCC, Clang | libstdc++ |
| macOS (`macos-latest`) | Apple Clang | libc++ |
| Windows (`windows-latest`) | MSVC | MSVC STL |

Supported architectures: x86_64 and ARM64 (including Apple Silicon).

## Documentation

- **[Documentation home](./readme/README.md)** — the full guide, organized for progressive learning:
  - [1. Introduction](./readme/1_introduction/) — what qb is, its philosophy, and when to use it
  - [2. Core concepts](./readme/2_core_concepts/) — actors, events, async I/O, concurrency, threading model
  - [3. qb-io](./readme/3_qb_io/) — the asynchronous runtime, transports, protocols, coroutines
  - [4. qb-core](./readme/4_qb_core/) — the actor engine and messaging
  - [5. Integration](./readme/5_core_io_integration/) — actors and async I/O together, with worked examples
  - [6. Guides](./readme/6_guides/) — getting started, patterns, performance, error handling, migration
  - [7. Reference](./readme/7_reference/) — API overview, build, invariants, benchmarks, FAQ, glossary
- **Project policies:** [INSTALL](./INSTALL.md) · [VERSIONING](./VERSIONING.md) ·
  [CHANGELOG](./CHANGELOG.md) · [SECURITY](./SECURITY.md) · [SUPPORT](./SUPPORT.md) ·
  [CONTRIBUTING](./CONTRIBUTING.md)

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](./LICENSE).

## Acknowledgments

qb builds on several open-source projects: [libev](http://software.schmorp.de/pkg/libev.html) (event loop),
[stduuid](https://github.com/mariusbancila/stduuid) (UUIDs), [nlohmann/json](https://github.com/nlohmann/json)
(JSON), [OpenSSL](https://www.openssl.org/) and [Argon2](https://github.com/P-H-C/phc-winner-argon2)
(TLS and password hashing), [zlib](https://zlib.net/) (compression), and the ska flat-hash-map and nanolog
designs. Their authors have our thanks.
