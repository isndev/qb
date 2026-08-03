<!-- Verified-against: qb 2.6.0 (C++20 default, C++23 supported) -->
# Building from source

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 2.6.0 (C++20 default, C++23 supported)

A reference for configuring, building, testing, and installing the qb framework from source with CMake: requirements, presets, options, generators, and install layout.

**Prerequisites:** [Installation](../../INSTALL.md) (the adopter on-ramp) — **See also:** [CMake options reference](./cmake_options.md), [CMake and dependencies](./cmake_dependencies.md), [Testing](./testing.md), [Getting started](../6_guides/getting_started.md), [Production checklist](../6_guides/production_checklist.md)

This page is the contributor-facing build reference. If you only want to *add qb to an application* and build it, start at [INSTALL.md](../../INSTALL.md); come back here for the command, preset, and option surface. The exhaustive `QB_*` option catalog is owned by [cmake_options.md](./cmake_options.md) — this page tabulates the options you set most often during a build and links there for the rest. Dependency resolution (GoogleTest, Google Benchmark, zlib, OpenSSL, ngtcp2) is owned by [cmake_dependencies.md](./cmake_dependencies.md); running and writing tests is owned by [testing.md](./testing.md). This page links to those pages rather than restating them.

## Requirements

| Requirement | Detail | Source |
|---|---|---|
| C++ compiler | C++20-capable: GCC, Clang, Apple Clang, or MSVC. qb sets `QB_CXX_STANDARD=20` by default, accepts `QB_CXX_STANDARD=23`, and keeps `CMAKE_CXX_STANDARD_REQUIRED=ON` with extensions off. | `qb/cmake/qbConfig.cmake` |
| CMake | 3.24 or newer. 3.24 is the floor because dependency resolution uses the `FetchContent` + `find_package` integration (`FIND_PACKAGE_ARGS`). | `qb/CMakeLists.txt:31`, `qb/CMakePresets.json:3-7` |
| Threads | A POSIX threads (pthreads) implementation is required on non-Windows platforms; configuration fails with a fatal error if it is missing. | `qb/cmake/qbCompiler.cmake:389-392` |
| Git | Needed on the configure machine only when a fetchable dependency (GoogleTest, Google Benchmark, zlib) is absent from the system and is built from source. | see [cmake_dependencies.md](./cmake_dependencies.md) |

Architectures: x86_64 and ARM64 (including Apple Silicon). The continuous integration matrix builds and tests every change on Linux (GCC, Clang / libstdc++), macOS (Apple Clang / libc++), and Windows (MSVC / MSVC STL). See [INSTALL.md](../../INSTALL.md#supported-toolchains) for the matrix.

Dependencies are resolved automatically: most builds need nothing installed beyond a compiler and CMake. libev and stduuid are qb forks, vendored and built from `qb/include/qb/vendor/`; OpenSSL, Argon2, and ngtcp2 are system-only and gate optional features when absent. The full policy lives in [cmake_dependencies.md](./cmake_dependencies.md).

## Quick build

An out-of-source build. libev and stduuid are vendored as committed files under `qb/include/qb/vendor/`, so they arrive with any clone — the `--recursive` flag is not needed for them:

```bash
# src: qb/INSTALL.md
git clone https://github.com/isndev/qb.git
cd qb
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

If the bundled modules are somehow missing from your checkout, restore them from the repo (a `git submodule update` will not bring them back, because they are committed files, not submodules):

```bash
git checkout -- include/qb/vendor
```

`-B build` selects (and creates) the build tree; the source tree is left untouched. `cmake --build` drives whichever generator CMake picked, so the same commands work across Make, Ninja, and Visual Studio. `--parallel` builds with all available cores.

## CMake presets

`qb/CMakePresets.json` (schema version 3) ships configure, build, and test presets. Presets are the supported way to get a known-good configuration without memorizing option combinations. All configure presets inherit a hidden `base` preset that sets `QB_CXX_STANDARD=20`, `QB_BUILD_TESTS=ON`, `QB_BUILD_EXAMPLES=ON`, `QB_BUILD_BENCHMARKS=OFF`, `QB_WITH_QUIC=AUTO`, and — note these two, which differ from the standalone option defaults — `QB_INSTALL=OFF` (presets do not generate install rules) and `QB_ENABLE_NATIVE_ARCH=OFF` (presets produce portable, non-host-tuned binaries). It writes the build tree to `build/<presetName>` (`CMakePresets.json:9-25`). C++23 validation is available through the `debug-cxx23` and `dev-cxx23` presets.

```bash
cmake --preset debug        # configure
cmake --build --preset debug
ctest --preset debug
```

### Configure presets

| Preset | Build type | Key cache variables | Source |
|---|---|---|---|
| `debug` | `Debug` | inherits `base` | `CMakePresets.json:35-43` |
| `release` | `Release` | tests enabled, optimized codegen | `CMakePresets.json:80-88` |
| `relwithdebinfo` | `RelWithDebInfo` | inherits `base` | `CMakePresets.json:71-79` |
| `dev` | `Debug` | `QB_BUILD_TESTS=ON`, `QB_BUILD_BENCHMARKS=ON`, `QB_BUILD_EXAMPLES=ON` | `CMakePresets.json:44-52` |
| `sanitize` | `Debug` | `QB_SANITIZE=address,undefined` | `CMakePresets.json:89-97` |
| `sanitize-thread` | `Debug` | `QB_SANITIZE=thread` | `CMakePresets.json:98-106` |
| `coverage` | `Debug` | `QB_BUILD_COVERAGE=ON` | `CMakePresets.json:107-117` |
| `release-lto` | `Release` | inherits `release` + `QB_ENABLE_LTO=ON` | `CMakePresets.json:118-126` |
| `release-native` | `Release` | inherits `release` + `QB_ENABLE_NATIVE_ARCH=ON` (the only preset that turns native arch back ON — `base` pins it OFF) | `CMakePresets.json:127-135` |
| `release-portable` | `Release` | inherits `release` + `QB_ENABLE_NATIVE_ARCH=OFF` (portable, distributable binaries) | `CMakePresets.json:136-144` |

### Build and test presets

`buildPresets`: `debug`, `dev`, `debug-cxx23`, `dev-cxx23`, `relwithdebinfo`, `release`,
`sanitize`, `sanitize-thread`, `coverage`, `coverage-html`, `coverage-xml`, `release-lto`,
`release-native`, and `release-portable`.
`testPresets`: `debug`, `dev`, `debug-cxx23`, `dev-cxx23`, `relwithdebinfo`, `release`, `sanitize`,
`sanitize-thread`, and `coverage`. Sanitized presets run serially because
instrumented async tests are slower and should not compete for scheduler/timing
resources. Every test preset sets `outputOnFailure`.

## Manual configuration

When a preset does not fit, configure by passing options directly. The pattern is `cmake -D<OPTION>=<VALUE> -B <build-dir>`:

```bash
# Release build, no tests, SSL and compression on (the defaults for those two)
cmake -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_TESTS=OFF -B build

# Debug build with tests, SSL off, compression off (minimal)
cmake -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_TESTS=ON \
      -DQB_WITH_SSL=OFF -DQB_WITH_COMPRESSION=OFF -B build
```

If `CMAKE_BUILD_TYPE` is not set, qb defaults it to `Release` (`qb/cmake/qbConfig.cmake:125`). qb also enables `CMAKE_EXPORT_COMPILE_COMMANDS` by default (for clangd / IDE tooling) unless a parent project already set it (`qbConfig.cmake:133-134`).

## Build options

Pass these at configure time (`cmake -D<NAME>=<VALUE> ...`). Defaults and source locations are taken from `qb/cmake/qbConfig.cmake`. The tables below cover the options you reach for during a build; [cmake_options.md](./cmake_options.md) is the canonical catalog of every `QB_*` variable, and the dependency-resolution options are documented in depth in [cmake_dependencies.md](./cmake_dependencies.md).

### Build configuration

| Option | Type / default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` \| `Release` \| `RelWithDebInfo` \| `MinSizeRel`; default `Release` | Standard CMake build configuration (`qbConfig.cmake:125`). |
| `BUILD_SHARED_LIBS` / `QB_BUILD_SHARED_LIBS` | bool; `QB_BUILD_SHARED_LIBS` defaults to the value of `BUILD_SHARED_LIBS` (itself `OFF` unless set) | Build `qb-io`/`qb-core` (and modules) as shared objects instead of static. Setting `BUILD_SHARED_LIBS=ON` switches qb to shared; `QB_BUILD_SHARED_LIBS` is an explicit qb-only override (`qbConfig.cmake:82`). The whole tree is built position-independent regardless (`CMAKE_POSITION_INDEPENDENT_CODE ON`, `qbConfig.cmake:155`). |
| `QB_BUILD_TESTS` | bool; `ON` | Build the unit and system tests (GoogleTest). Gates GoogleTest resolution (`qbConfig.cmake:59`). |
| `QB_BUILD_BENCHMARKS` | bool; `OFF` | Build performance benchmarks (Google Benchmark) (`qbConfig.cmake:61`). |
| `QB_BUILD_EXAMPLES` | bool; `ON` | Build the example applications (`qbConfig.cmake:60`). |
| `QB_BUILD_DOCS` | bool; `OFF` | Add the Doxygen documentation subdirectory (`qbConfig.cmake:79`, `CMakeLists.txt:200-202`). |
| `QB_INSTALL` | bool; `ON` | Generate installation rules (`cmake --install`) (`qbConfig.cmake:83`, `CMakeLists.txt:207`). |
| `CMAKE_INSTALL_PREFIX` | path | Standard install root. |

### Optional features

| Option | Type / default | Effect |
|---|---|---|
| `QB_WITH_SSL` | bool; `ON` | SSL/TLS and crypto in `qb-io` via OpenSSL. Forced `OFF` (with the feature disabled rather than a build failure) when OpenSSL is not found. Argon2 password hashing is enabled when libargon2 is also present (`qbConfig.cmake:99`). |
| `QB_WITH_COMPRESSION` | bool; `ON` | Compression in `qb-io` via zlib — system first, fetched as a fallback when `QB_DEPS_FETCH_FALLBACK=ON` (`qbConfig.cmake:100`). |
| `QB_WITH_QUIC` | `AUTO` \| `ON` \| `OFF`; `AUTO` | QUIC transport via libngtcp2. `AUTO` enables it iff libngtcp2 is found (quiet when absent); `ON` requires it (warns if missing); `OFF` disables it. Requires `QB_WITH_SSL` (`qbConfig.cmake:103-105`). |
| `QB_WITH_LOGGING` | bool; `ON` | Logging subsystem (nanolog); defines `QB_WITH_LOGGING=1` (`qbConfig.cmake:98,319`). |
| `QB_STDOUT_LOGGING` | bool; `OFF` | Stdout logging fallback; defines `QB_STDOUT_LOGGING=1` (`qbConfig.cmake:110,334`). |
| `QB_WITH_PROFILING` | bool; `OFF` | Link gperftools (tcmalloc/profiler) when found. Incompatible with `QB_SANITIZE` (`qbConfig.cmake:105`). |

### Performance

| Option | Type / default | Effect |
|---|---|---|
| `QB_ENABLE_OPTIMIZATIONS` | bool; `ON` | Extra Release optimization flags — `-funroll-loops`, `-ftree-vectorize`, `-ffunction-sections`/`-fdata-sections` on GCC/Clang, `/Ot`/`/Gy` on MSVC (`qbConfig.cmake:86`, `qbCompiler.cmake`). |
| `QB_ENABLE_NATIVE_ARCH` | bool; `ON` | Tune codegen for the build-host CPU: `-march=native`, falling back to `-mcpu=native` (validated per compiler; `/arch:AVX2` on MSVC). **Turn `OFF` for portable / distributable binaries** — see the `release-portable` preset (`qbConfig.cmake:91`, `qbCompiler.cmake:239-264`). |
| `QB_ENABLE_LTO` | bool; `OFF` | Link-time optimization for Release (`-flto`, or `/GL` + `/LTCG` on MSVC) (`qbConfig.cmake:87`, `qbCompiler.cmake:271-294`). |
| `QB_ENABLE_FAST_MATH` | bool; `OFF` | `-ffast-math` / `/fp:fast`. Breaks IEEE-754 compliance (`qbConfig.cmake:92`, `qbCompiler.cmake:216,232`). |

### Diagnostics

| Option | Type / default | Effect |
|---|---|---|
| `QB_SANITIZE` | string; empty (off) | Comma-separated sanitizer list applied to every qb/qbm/test target and its link step, e.g. `address,undefined`, `thread`, `memory`, `leak`. Use the `sanitize` / `sanitize-thread` presets. Incompatible with `QB_WITH_PROFILING` (`qbConfig.cmake:115`, `qbCompiler.cmake:312-344`). |
| `QB_DEBUG_MEMORY` | bool; `OFF` | Legacy alias: when `QB_SANITIZE` is empty, turns on `QB_SANITIZE=address,undefined` (`qbConfig.cmake:108,117-118`). |
| `QB_BUILD_COVERAGE` | bool; `OFF` | gcov/lcov coverage instrumentation. Debug and non-Windows only; sets up `qb-coverage`, `qb-coverage-xml`, and `qb-coverage-html` targets when `lcov`/`gcov` are found (`qbConfig.cmake:95`, `CMakeLists.txt:126-195`). |
| `QB_DEBUG_ACTOR` | bool; `OFF` | Extra actor-system debug instrumentation; defines `QB_DEBUG_ACTOR=1` (`qbConfig.cmake:109,331`). |

### Dependency resolution

These three are summarized here and documented in full, with the pinned tags and offline-build guidance, in [cmake_dependencies.md](./cmake_dependencies.md).

| Option | Type / default | Effect |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK` | bool; `ON` | Build fetchable deps (GoogleTest, Google Benchmark, zlib) from source when not found on the system. `OFF` means system-only for those (`qbConfig.cmake:70`). |
| `QB_USE_SYSTEM_GTEST` / `QB_USE_SYSTEM_BENCHMARK` | bool; `OFF` | Force `find_package(... CONFIG REQUIRED)`; never fetch (`qbConfig.cmake:72-73`). |
| `QB_GOOGLETEST_GIT_TAG` / `QB_GOOGLEBENCHMARK_GIT_TAG` / `QB_ZLIB_GIT_TAG` | string (advanced cache); `v1.15.2` / `v1.9.2` / `v1.3.1` | Pin the fetched source revision (`qbConfig.cmake:75-77`). |

Always check the root `CMakeLists.txt` and `qb/cmake/` for the most current and complete option list for your checkout.

## Generators

qb does not pin a generator; it uses whatever CMake selects or you request. `cmake --build` drives the chosen generator uniformly, so the build commands above are generator-agnostic.

- **Ninja** (single-config): `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build`. Fast incremental builds; recommended for day-to-day work.
- **Unix Makefiles** (single-config): the default on Linux/macOS when Ninja is not requested.
- **Visual Studio** (multi-config, e.g. `-G "Visual Studio 17 2022"`): pick the configuration at build time with `cmake --build build --config Release`. With a multi-config generator, `CMAKE_BUILD_TYPE` has no effect — pass `--config`.
- **Ninja Multi-Config**: also multi-config; select with `--config` at build time.

For multi-config generators, qb routes per-configuration outputs into the same `bin`/`lib` layout described below (`qbConfig.cmake:191-203`).

## Build the code and run tests

```bash
# Single-config generators (Ninja, Make): build type was fixed at configure time
cmake --build build --parallel

# Multi-config generators (Visual Studio, Ninja Multi-Config): pick config now
cmake --build build --config Release --parallel
```

Run the test suite with CTest; see [testing.md](./testing.md) for filtering, sanitizers, and writing new tests:

```bash
ctest --test-dir build --output-on-failure
```

## Targets and output layout

A successful build produces the two libraries and, when enabled, the example, test, and benchmark executables.

- **Libraries:** `qb-io` (asynchronous I/O and utilities) and `qb-core` (the actor engine, which depends on `qb-io`). Consumers link the namespaced aliases `qb::io` and `qb::core` (`CMakeLists.txt:97-107`). Shared builds carry the platform extension (`libqb-io.so`, `libqb-io.dylib`, `qb-io.dll`).
- **Output directories:** unless a parent project has already chosen them, runtime artifacts go under `${CMAKE_BINARY_DIR}/bin` and libraries/archives under `${CMAKE_BINARY_DIR}/lib` (`qbConfig.cmake:177-188`). When qb is embedded via `add_subdirectory`, it does not override an output tree the parent already set.
- **Coverage targets** (`QB_BUILD_COVERAGE=ON`, Debug, non-Windows): `qb-coverage`, `qb-coverage-xml`, `qb-coverage-html`.

## Install

With `QB_INSTALL=ON` (the default), the configure step generates install rules. Install after building:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DQB_INSTALL=ON -B build
cmake --build build --parallel
cmake --install build --prefix /your/prefix    # omit --prefix for the system default
```

The install (`CMakeLists.txt:207-314`) lays out:

- **Libraries** under `${CMAKE_INSTALL_LIBDIR}`, **runtime** under `${CMAKE_INSTALL_BINDIR}`, **headers** under `${CMAKE_INSTALL_INCLUDEDIR}` (GNU install dirs). The export set bundles `qb-io`, `qb-core`, and the bundled `ev`/`stduuid` targets so their names are rewritten under the `qb::` namespace in the dependency graph.
- **CMake package files** under `${CMAKE_INSTALL_LIBDIR}/cmake/qb`: `qbTargets.cmake` (namespaced `qb::`), `qbConfig.cmake`, and a `qbConfigVersion.cmake` written with `COMPATIBILITY SameMajorVersion` (`CMakeLists.txt:273-292`).
- **Find modules for consumers:** `FindArgon2.cmake` is installed when the build resolved Argon2 (`QB_HAS_ARGON2`), and `FindNgtcp2.cmake` when QUIC was enabled (`QB_HAS_QUIC`), so a downstream `find_package(qb)` of a QUIC- or Argon2-enabled build can recreate the imported targets `qb::io` links transitively (`CMakeLists.txt:295-307`).

Downstream then consumes the installed copy with `find_package`:

```cmake
find_package(qb CONFIG REQUIRED)   # provides qb::core and qb::io
target_link_libraries(my_app PRIVATE qb::core qb::io)
```

The two supported integration modes — embed via `add_subdirectory(qb)` or consume an installed copy via `find_package(qb)` — both expose `qb::core` and `qb::io` (`CMakeLists.txt:24-28`). The embed mode is covered in [INSTALL.md](../../INSTALL.md#embed-the-source-tree).

## Platform notes

- **Linux:** POSIX sockets. Use GCC or Clang with solid C++20 support. Install optional dependency headers when enabling features (`libssl-dev`, `libargon2-dev`, `zlib1g-dev` on Debian/Ubuntu; `openssl-devel`, `zlib-devel` on Fedora/RHEL). Install libngtcp2 packages when QUIC is required. qb links `dl` and `rt` (`qb/cmake/qbDependencies.cmake`).
- **macOS:** POSIX sockets. Recent Xcode / Apple Clang. Homebrew supplies optional dependencies (`brew install openssl argon2 zlib`); point CMake at them with `CMAKE_PREFIX_PATH` when needed. On Apple Silicon, native-arch tuning uses `-mcpu=native` because `-march=native` is rejected (`qbCompiler.cmake:243-249`); qb links the `Foundation` framework.
- **Windows:** Winsock2. Use a Visual Studio 2022 (or newer) MSVC toolset that supports `/std:c++23`. For optional features, put OpenSSL/zlib development libraries on `CMAKE_PREFIX_PATH` (or set `OPENSSL_ROOT_DIR`); CI uses vcpkg. qb links `ws2_32` and `mswsock`.

## Pitfalls

- **Bundled deps missing from a checkout.** libev and stduuid are vendored as committed files under `qb/include/qb/vendor/`, not submodules and not fetched; a normal clone always ships them. A `libev … not found` fatal error means they are missing — restore them with `git checkout -- include/qb/vendor` or re-clone. A `git submodule update` will not bring them back.
- **Host-tuned binary fails on another machine.** The default `QB_ENABLE_NATIVE_ARCH=ON` targets the build host's CPU. Rebuild with `-DQB_ENABLE_NATIVE_ARCH=OFF` (or the `release-portable` preset) for distributable artifacts.
- **`CMAKE_BUILD_TYPE` ignored.** With multi-config generators (Visual Studio, Ninja Multi-Config), the configuration is chosen at build time via `--config`, not at configure time.
- **Sanitizers and profiling collide.** `QB_SANITIZE` and `QB_WITH_PROFILING` intercept the same hooks; enabling both emits a warning. Pick one.
- **Network needed on first configure for a from-source fallback.** When a fetchable dependency is absent from the system, the first configure clones it from GitHub. For air-gapped builds, pre-populate `_deps` or force system packages — see [cmake_dependencies.md](./cmake_dependencies.md#offline-and-ci-builds).
- **CI quality gates are split by concern.** The default CMake workflow remains the
  cross-platform Release matrix. Dedicated GitHub Actions workflows cover ASan/UBSan
  (`sanitize`), TSan (`sanitize-thread`), coverage, and clang-format on changed C++
  files. clang-tidy is not run in CI — run it locally via `scripts/clang-tidy.sh`
  before submitting.

## See also

- [INSTALL.md](../../INSTALL.md) — adopter on-ramp: toolchains, optional system packages, integration modes.
- [CMake options reference](./cmake_options.md) — the complete `QB_*` option catalog with defaults.
- [CMake and dependencies](./cmake_dependencies.md) — GoogleTest / Google Benchmark / zlib resolution, pinned tags, offline builds.
- [Testing](./testing.md) — building, running, filtering, and writing tests; sanitizer runs.
- [Getting started](../6_guides/getting_started.md) · [Production checklist](../6_guides/production_checklist.md)
