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

It renders the template under the name you passed: `MyProject` is substituted into the CMake project
and target names, the C++ namespaces, the include guards and the directory names, so nothing has to
be renamed by hand. That is also why the name is validated up front — it must match
`[A-Za-z][A-Za-z0-9_-]*`, and anything else exits 2 rather than failing at the first compile error.
The result is a fresh git repository with a single initial commit and no remote, not a copy of the
template's history, and there are no submodules to initialize: the generated `CMakeLists.txt` fetches
qb and the qbm modules with CMake's `FetchContent` at the first configure. The script creates nothing
outside `MyProject/`, refuses to run if that name is taken, aborts on the first failed step, and
removes what it made if it does not finish — worth knowing, because the invocation above pipes it
into `bash` in whatever directory you happen to be standing in. It reports the file count, the
template ref and the qb ref it wrote into the tree, and why it chose each.

The generated project builds to `./build/bin/MyProject` and ships a ctest suite (targets named
`MyProject-test-<tier>-<name>`) plus a GitHub Actions workflow. To build against a qb checkout you
already have, configure with `-DFETCHCONTENT_SOURCE_DIR_QB=/path/to/qb`.

> **Which qb the generated project builds against is decided by the script, not stored in the
> template.** It used to be a submodule gitlink committed in the template, a pinned dependency that
> could only drift, and it drifted two majors. Nothing stores it now: each script carries the version
> of the qb it ships with, asserted equal to `QB_FRAMEWORK_VERSION` in
> [`cmake/qbConfig.cmake`](./cmake/qbConfig.cmake) by
> [`scripts/check-scaffold-consistency.sh`](./scripts/check-scaffold-consistency.sh), and writes the
> matching ref into the generated tree — so the URL the one-liner is fetched from selects the
> pairing. The template content resolves against the same version: an explicit `QB_TEMPLATE_REF`
> wins, then a `v<version>` tag on the template, then, only when qb `v<version>` is known *not* to
> exist, the template's `develop` branch, and finally the template's default branch, which is
> reported as a fallback. The script always prints which ref it used and why. Both templates now
> carry CI of their own that renders with this same script and builds, tests and runs the result on
> Linux and macOS.
>
> One honest caveat: 3.0.0 is not tagged yet, so today the script resolves to the templates'
> `develop` branch and writes `develop` as the qb ref, and says so when it runs. The generated tree
> therefore follows a moving branch rather than a pinned release; to pin it, set `QB_GIT_REF` in the
> generated `CMakeLists.txt` to a released tag. Two overrides exist for the other direction:
> `QB_TEMPLATE_DIR=<path>` renders from a local template checkout (for template authors), and
> `QB_REF=<ref>` overrides the qb ref written into the generated tree.

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

The generated module ships its public headers under `src/qbm/mymodule/`, a tiered `tests/` suite
registered with `qb_register_module_test()`, and a superbuild root that builds and runs those tests
with no further setup:

```bash
cmake -S .github/ci/superbuild -B build
cmake --build build --parallel --target qbm-mymodule-tests
ctest --test-dir build -L module:qbm-mymodule
```

> **Why a superbuild root rather than `cmake -S . -B build`?** A qbm module cannot be configured
> standalone: it calls `qb_register_module()` and `qb_add_test()`, development-time helpers an
> installed qb does not ship. `.github/ci/superbuild/CMakeLists.txt` is the same answer `qbm-http`,
> `qbm-pgsql` and `qbm-redis` give — a minimal root that adds a qb *source* tree first and the module
> second — in the same shape, with one deliberate difference: theirs require both trees to be passed
> in, because their CI always passes them, while the generated one defaults its own paths and fetches
> qb, so it works with no arguments on a machine that has no qb checkout.
>
> The module name is not merely a directory name, which is why it is stricter than a project's:
> `mymodule` becomes the `qbm-mymodule` target, the `qbm::mymodule` namespace and the
> `src/qbm/mymodule/` directory that `qb_register_module()` requires, so it must be a lowercase
> identifier (`[a-z][a-z0-9_]*`) and anything else exits 2 rather than failing at the first compile
> error. The qb the module builds against is chosen by the script at scaffold time exactly as it is
> for a project, including the `develop` caveat that applies while 3.0.0 is untagged.

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
