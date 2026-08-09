<!-- Verified-against: qb 3.0.0 (C++20 default, C++23 supported) -->

# qb Actor Framework (QBAF)

<p align="center"><img src="./resources/logo.svg" width="180px" alt="qb Actor Framework logo" /></p>

qb — the qb Actor Framework, **QBAF** — is a C++20-first framework with optional C++23 support for building concurrent
and distributed systems on the actor model. It pairs
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

The `qb-new-project.sh` helper scaffolds a project from the
[`qb-sample-project`](https://github.com/isndev/qb-sample-project) template:

```bash
curl -fsSL https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh | bash /dev/stdin MyProject
cd MyProject
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
```

It clones the template into `MyProject/`, drops the `origin` remote so the result is yours, and
initializes the submodules — including qb itself. It creates nothing outside `MyProject/`, refuses
to run if that name is taken, aborts on the first failed step, and removes what it made if it does
not finish — worth knowing, because the invocation above pipes it into `bash` in whatever directory
you happen to be standing in. It prints the file count and the qb the template pinned; if you see
`0 files` or an unexpected qb, stop and read the next paragraph.

> **The template is versioned separately from the framework, and the two are not currently in
> step.** The one-liner fetches the script from `main`, the script clones the template from *its*
> default branch, and the qb the template pins as a submodule is a third, independent pointer —
> today it names a commit from before v2.0.0, so a freshly scaffolded project does **not** build
> against this qb. Until the template is refreshed, prefer
> [Integrate into your build](#integrate-into-your-build): add qb to a CMake project you already
> have. `QB_TEMPLATE_REF=<ref>` pins the template clone once the template carries a ref matching a
> qb release.

> **Both scaffolding scripts are bash** (`#!/usr/bin/env bash`, `set -euo pipefail`) and shell out
> to `git` only. They do **not** run in `cmd.exe` or PowerShell. On Windows, run them from **WSL**
> or **Git Bash**. Nothing else in qb's build requires a shell — MSVC builds work normally once the
> project exists; this constraint applies only to the two generators.
>
> Both are fetched **from a branch**, so the URL selects the version: `.../qb/main/script/...` is
> the released line, `.../qb/<tag>/script/...` pins one release. `curl | bash` also means the code
> runs unreviewed in the directory you are standing in — `curl -fsSL <url> -o scaffold.sh`, read it,
> then `bash scaffold.sh MyProject` does the same thing and lets you look first.

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
    qb::io::async::task<bool> onInit() final {
        registerEvent<GreetingEvent>(*this);   // subscribe
        push<GreetingEvent>(id(), "Hello");     // send to self
        co_return true;                         // actor is ready
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

| Module                                           | Provides                                                                                                                                  |
|--------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| [qbm-http](https://github.com/isndev/qbm-http)   | HTTP/1.1, routing, middleware; plus HTTP/2, WebSocket (RFC 6455), and JWT authentication on SSL-enabled builds, and HTTP/3 on QUIC builds |
| [qbm-pgsql](https://github.com/isndev/qbm-pgsql) | Asynchronous PostgreSQL client with prepared statements and transactions                                                                  |
| [qbm-redis](https://github.com/isndev/qbm-redis) | Asynchronous Redis client covering the full command surface                                                                               |

```bash
git submodule add https://github.com/isndev/qbm-http qbm/http
```

```cmake
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")
target_link_libraries(my_app PRIVATE qbm::http)
```

### Scaffold your own module

`qb-new-module.sh` is the module-side counterpart of `qb-new-project.sh`. It scaffolds a qbm module
from the [`qb-sample-module`](https://github.com/isndev/qb-sample-module) template:

```bash
curl -fsSL https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-module.sh | bash /dev/stdin mymodule
```

Run it from your project's `qbm/` directory; the result is picked up by the
`qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")` call above and exposed as `qbm::mymodule`. The
same guards apply as for `qb-new-project.sh`: it creates nothing outside `mymodule/`, refuses to
overwrite it, aborts on the first failed step, cleans up after itself, and — being bash — needs WSL
or Git Bash on Windows.

> **The module template predates the 3.0 source layout and does not configure against this qb.**
> It was last touched in 2019: it registers a header-only module without `HEADER_ONLY`, keeps its
> headers in a flat `actor/ event/ service/` tree rather than under `src/qbm/<name>/`, calls a
> `qb_register_module_gtest()` that no longer exists, and overrides `onInit()` with the pre-2.6
> `bool` signature instead of `qb::io::async::task<bool>`. Until it is refreshed, copy the shape of
> a real module instead — `qbm-http`, `qbm-pgsql` and `qbm-redis` are the reference, and each
> carries a `.github/ci/superbuild/CMakeLists.txt` showing the only root from which a single module
> builds. A qbm module cannot be configured standalone: it calls `qb_register_module()` and
> `qb_add_test()`, which an installed qb does not ship.

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

| Option                  | Default | Purpose                                                                               |
|-------------------------|---------|---------------------------------------------------------------------------------------|
| `QB_WITH_SSL`           | `ON`    | SSL/TLS and crypto (OpenSSL); auto-disabled if OpenSSL is absent                      |
| `QB_WITH_COMPRESSION`   | `ON`    | Compression (zlib)                                                                    |
| `QB_WITH_QUIC`          | `AUTO`  | QUIC/HTTP3 via ngtcp2: `AUTO` enables it when found                                   |
| `QB_WITH_LOGGING`       | `ON`    | Logging support                                                                       |
| `QB_BUILD_TESTS`        | `ON`    | Build the test suite                                                                  |
| `QB_BUILD_BENCHMARKS`   | `OFF`   | Build benchmarks (Google Benchmark)                                                   |
| `QB_ENABLE_NATIVE_ARCH` | `OFF`   | Tune codegen for the build host (`-march=native`); turn **on** only for host-local builds |

The complete option list is in [CMake options](./readme/7_reference/cmake_options.md); installation details
are in [INSTALL.md](./INSTALL.md).

## Platform support

Continuous integration builds and tests every change on:

| OS                         | Compilers   | Standard library | CI                    |
|----------------------------|-------------|------------------|-----------------------|
| Linux (`ubuntu-latest`)    | GCC, Clang  | libstdc++        | enabled               |
| macOS (`macos-latest`)     | Apple Clang | libc++           | enabled               |
| Windows (`windows-latest`) | MSVC        | MSVC STL         | **currently disabled** |

> **Windows/MSVC is supported source, but is not exercised by CI right now.** The hosted-runner
> job is commented out in `.github/workflows/cmake.yml` (it took ~70 minutes and its tests timed
> out under that load, reporting runner noise rather than defects); the maintainer validates the
> full MSVC solution out of band before each release. Treat a Windows build as verified by you,
> not by this project's CI. See [INSTALL.md](INSTALL.md#supported-toolchains).

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

## For AI assistants

This repository publishes machine-readable documentation following the
[llms.txt](https://llmstxt.org/) convention, so a coding agent can read qb without
guessing:

- **[`llms.txt`](./llms.txt)** — the index: a one-paragraph summary, the five rules that decide whether generated qb code is correct, and a link
  list of every document in this repository.
- **[`llms-full.txt`](./llms-full.txt)** — ~34k tokens: `llm/qb.llm.md` (the mental model, invariants and footguns) and `llm/qb.llm.api.md` (a deterministic public-API reference, every signature verified against the headers under `src/`), concatenated into one fetch.

Both files are generated by `scripts/gen-llms-txt.py` from `llm/` and checked in CI
(`scripts/doc-lint.sh` section 1d), so they cannot drift from the documentation they index.

**Use it over MCP, with nothing to host and nothing to install.**
[GitMCP](https://gitmcp.io) exposes any public GitHub repository as an MCP endpoint and reads
`llms.txt` first (its documented order is `llms.txt`, then an AI-optimised documentation
build, then `README.md`):

```json
{ "mcpServers": { "qb": { "url": "https://gitmcp.io/isndev/qb" } } }
```

Claude Desktop and other clients without native remote-MCP support wrap the same URL:
`"command": "npx", "args": ["mcp-remote", "https://gitmcp.io/isndev/qb"]`.

**Cursor `@Docs`** — add
`https://raw.githubusercontent.com/isndev/qb/main/llms-full.txt`.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](./LICENSE).

## Acknowledgments

qb builds on several open-source projects: [libev](http://software.schmorp.de/pkg/libev.html) (event loop),
[stduuid](https://github.com/mariusbancila/stduuid) (UUIDs), [nlohmann/json](https://github.com/nlohmann/json)
(JSON), [OpenSSL](https://www.openssl.org/) and [Argon2](https://github.com/P-H-C/phc-winner-argon2)
(TLS and password hashing), [zlib](https://zlib.net/) (compression), and the ska flat-hash-map and nanolog
designs. Their authors have our thanks.
