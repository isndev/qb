<!-- Verified-against: qb 3.0.0 (C++20 default, C++23 supported) -->
# Building from source

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

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

Architectures: x86_64 and ARM64 (including Apple Silicon). The continuous integration matrix builds and tests every change on Linux (GCC, Clang / libstdc++) and macOS (Apple Clang / libc++). **Windows (MSVC / MSVC STL) is supported source but its CI job is currently disabled** — it is validated out of band before each release, so treat a Windows build as verified by you, not by this project's CI. See [INSTALL.md](../../INSTALL.md#supported-toolchains) for the matrix.

Dependencies are resolved automatically: most builds need nothing installed beyond a compiler and CMake. libev and stduuid are qb forks, vendored and built from `qb/src/qb/vendor/`; OpenSSL, Argon2, and ngtcp2 are system-only and gate optional features when absent. The full policy lives in [cmake_dependencies.md](./cmake_dependencies.md).

## Quick build

An out-of-source build. libev and stduuid are vendored as committed files under `qb/src/qb/vendor/`, so they arrive with any clone — the `--recursive` flag is not needed for them:

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
git checkout -- src/qb/vendor
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

If `CMAKE_BUILD_TYPE` is not set, qb defaults it to `Release` (`qb/cmake/qbConfig.cmake:209-213`). qb also enables `CMAKE_EXPORT_COMPILE_COMMANDS` by default (for clangd / IDE tooling) unless a parent project already set it (`qbConfig.cmake:227-228`).

## Build options

Pass these at configure time (`cmake -D<NAME>=<VALUE> ...`). Defaults and source locations are taken from `qb/cmake/qbConfig.cmake`. The tables below cover the options you reach for during a build; [cmake_options.md](./cmake_options.md) is the canonical catalog of every `QB_*` variable, and the dependency-resolution options are documented in depth in [cmake_dependencies.md](./cmake_dependencies.md).

### Build configuration

| Option | Type / default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` \| `Release` \| `RelWithDebInfo` \| `MinSizeRel`; default `Release` | Standard CMake build configuration (`qbConfig.cmake:209-223`). |
| `BUILD_SHARED_LIBS` / `QB_BUILD_SHARED_LIBS` | bool; `QB_BUILD_SHARED_LIBS` defaults to the value of `BUILD_SHARED_LIBS` (itself `OFF` unless set) | Build `qb-io`/`qb-core` (and modules) as shared objects instead of static. Setting `BUILD_SHARED_LIBS=ON` switches qb to shared; `QB_BUILD_SHARED_LIBS` is an explicit qb-only override (`qbConfig.cmake:113`). The whole tree is built position-independent regardless (`CMAKE_POSITION_INDEPENDENT_CODE ON`, `qbConfig.cmake:275`). |
| `QB_BUILD_TESTS` | bool; `ON` | Build the unit and system tests (GoogleTest). Gates GoogleTest resolution (`qbConfig.cmake:86`). |
| `QB_BUILD_BENCHMARKS` | bool; `OFF` | Build performance benchmarks (Google Benchmark) (`qbConfig.cmake:90`). |
| `QB_BUILD_EXAMPLES` | bool; `ON` | Build the example applications (`qbConfig.cmake:87`). |
| `QB_BUILD_DOCS` | bool; `OFF` | Add the Doxygen documentation subdirectory (`qbConfig.cmake:110`, `CMakeLists.txt:225-227`). |
| `QB_INSTALL` | bool; `ON` | Generate installation rules (`cmake --install`) (`qbConfig.cmake:118-122`, `CMakeLists.txt:232`). |
| `CMAKE_INSTALL_PREFIX` | path | Standard install root. |

### Optional features

| Option | Type / default | Effect |
|---|---|---|
| `QB_WITH_SSL` | bool; `ON` | SSL/TLS and crypto in `qb-io` via OpenSSL. Forced `OFF` (with the feature disabled rather than a build failure) when OpenSSL is not found. Argon2 password hashing is enabled when libargon2 is also present (`qbConfig.cmake:148`). |
| `QB_WITH_COMPRESSION` | bool; `ON` | Compression in `qb-io` via zlib — system first, fetched as a fallback when `QB_DEPS_FETCH_FALLBACK=ON` (`qbConfig.cmake:149`). |
| `QB_WITH_QUIC` | `AUTO` \| `ON` \| `OFF`; `AUTO` | QUIC transport via libngtcp2. `AUTO` enables it iff libngtcp2 is found (quiet when absent); `ON` requires it (warns if missing); `OFF` disables it. Requires `QB_WITH_SSL` (`qbConfig.cmake:152-153`). |
| `QB_WITH_LOGGING` | bool; `ON` | Logging subsystem (nanolog); defines `QB_WITH_LOGGING=1` (`qbConfig.cmake:147`). |
| `QB_STDOUT_LOGGING` | bool; `OFF` | Stdout logging fallback; defines `QB_STDOUT_LOGGING=1` (`qbConfig.cmake:183,455`). |
| `QB_WITH_PROFILING` | bool; `OFF` | Link gperftools (tcmalloc/profiler) when found. Incompatible with `QB_SANITIZE` (`qbConfig.cmake:178`). |

### Performance

| Option | Type / default | Effect |
|---|---|---|
| `QB_ENABLE_OPTIMIZATIONS` | bool; `ON` | Extra Release optimization flags — `-funroll-loops`, `-ftree-vectorize`, `-ffunction-sections`/`-fdata-sections` on GCC/Clang, `/Ot`/`/Gy` on MSVC (`qbConfig.cmake:130`, `qbCompiler.cmake`). |
| `QB_ENABLE_NATIVE_ARCH` | bool; `OFF` | Tune codegen for the build-host CPU: `-march=native`, falling back to `-mcpu=native` (validated per compiler; `/arch:AVX2` on MSVC). **Turn `OFF` for portable / distributable binaries** — see the `release-portable` preset (`qbConfig.cmake:140`, `qbCompiler.cmake:274-298`). |
| `QB_ENABLE_LTO` | bool; `OFF` | Link-time optimization for Release (`-flto`, or `/GL` + `/LTCG` on MSVC) (`qbConfig.cmake:131`, `qbCompiler.cmake:306-331`). |
| `QB_ENABLE_FAST_MATH` | bool; `OFF` | `-ffast-math` / `/fp:fast`. Breaks IEEE-754 compliance (`qbConfig.cmake:141`, `qbCompiler.cmake:250,265`). |

### Diagnostics

| Option | Type / default | Effect |
|---|---|---|
| `QB_SANITIZE` | string; empty (off) | Comma-separated sanitizer list applied to every qb/qbm/test target and its link step, e.g. `address,undefined`, `thread`, `memory`, `leak`. Use the `sanitize` / `sanitize-thread` presets. Incompatible with `QB_WITH_PROFILING`. **MSVC ships only AddressSanitizer**: `address` is honoured (build-wide, because MSVC cannot link mixed ASan/non-ASan objects), every other component is dropped with a warning naming it — so the `sanitize` preset's `undefined` half does not run there, and `sanitize-thread` / `coverage` are disabled by preset condition on Windows altogether (`qbConfig.cmake:188`, `qbCompiler.cmake:358-466`). |
| `QB_DEBUG_MEMORY` | bool; `OFF` | Legacy alias: when `QB_SANITIZE` is empty, turns on `QB_SANITIZE=address,undefined` (`qbConfig.cmake:181,180-181`). |
| `QB_BUILD_COVERAGE` | bool; `OFF` | gcov/lcov coverage instrumentation. Debug and non-Windows only; sets up `qb-coverage`, `qb-coverage-xml`, and `qb-coverage-html` targets when `lcov`/`gcov` are found (`qbConfig.cmake:144`, `CMakeLists.txt:126-195`). |
| `QB_DEBUG_ACTOR` | bool; `OFF` | Extra actor-system debug instrumentation; defines `QB_DEBUG_ACTOR=1` (`qbConfig.cmake:182,452`). |

### Dependency resolution

These three are summarized here and documented in full, with the pinned tags and offline-build guidance, in [cmake_dependencies.md](./cmake_dependencies.md).

| Option | Type / default | Effect |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK` | bool; `ON` | Build fetchable deps (GoogleTest, Google Benchmark, zlib) from source when not found on the system. `OFF` means system-only for those (`qbConfig.cmake:99`). |
| `QB_USE_SYSTEM_GTEST` / `QB_USE_SYSTEM_BENCHMARK` | bool; `OFF` | Force `find_package(... CONFIG REQUIRED)`; never fetch (`qbConfig.cmake:101-102`). |
| `QB_GOOGLETEST_GIT_TAG` / `QB_GOOGLEBENCHMARK_GIT_TAG` / `QB_ZLIB_GIT_TAG` | string (advanced cache); `v1.15.2` / `v1.9.2` / `v1.3.1` | Pin the fetched source revision (`qbConfig.cmake:104-106`). |

Always check the root `CMakeLists.txt` and `qb/cmake/` for the most current and complete option list for your checkout.

## Generators

qb does not pin a generator; it uses whatever CMake selects or you request. `cmake --build` drives the chosen generator uniformly, so the build commands above are generator-agnostic.

- **Ninja** (single-config): `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build`. Fast incremental builds; recommended for day-to-day work.
- **Unix Makefiles** (single-config): the default on Linux/macOS when Ninja is not requested.
- **Visual Studio** (multi-config, e.g. `-G "Visual Studio 17 2022"`): pick the configuration at build time with `cmake --build build --config Release`. With a multi-config generator, `CMAKE_BUILD_TYPE` has no effect — pass `--config`.
- **Ninja Multi-Config**: also multi-config; select with `--config` at build time.

For multi-config generators, qb routes per-configuration outputs into the same `bin`/`lib` layout described below (`qbConfig.cmake:237-249`).

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

- **Libraries:** `qb-io` (asynchronous I/O and utilities) and `qb-core` (the actor engine, which depends on `qb-io`). Consumers link the namespaced aliases `qb::io` and `qb::core` (`CMakeLists.txt:98-108`). Shared builds carry the platform extension (`libqb-io.so`, `libqb-io.dylib`, `qb-io.dll`).
- **Output directories:** unless a parent project has already chosen them, runtime artifacts go under `${CMAKE_BINARY_DIR}/bin` and libraries/archives under `${CMAKE_BINARY_DIR}/lib` (`qbConfig.cmake:298-300`). When qb is embedded via `add_subdirectory`, it does not override an output tree the parent already set.
- **Coverage targets** (`QB_BUILD_COVERAGE=ON`, Debug, non-Windows): `qb-coverage`, `qb-coverage-xml`, `qb-coverage-html`.

## Install

With `QB_INSTALL=ON` (the default), the configure step generates install rules. Install after building:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DQB_INSTALL=ON -B build
cmake --build build --parallel
cmake --install build --prefix /your/prefix    # omit --prefix for the system default
```

The install (`CMakeLists.txt:232-325`) lays out. Every rule below is emitted by the one shared
helper `qb_install_package()` (`cmake/qbPackage.cmake:92-234`), which each qbm module calls with
the same arguments shape:

- **Libraries** under `${CMAKE_INSTALL_LIBDIR}`, **runtime** under `${CMAKE_INSTALL_BINDIR}`, **headers** under `${CMAKE_INSTALL_INCLUDEDIR}` (GNU install dirs). The export set bundles `qb-io`, `qb-core`, and the bundled `qev`/`stduuid` targets so their names are rewritten under the `qb::` namespace in the dependency graph.
- **CMake package files** under `${CMAKE_INSTALL_LIBDIR}/cmake/qb`: `qbTargets.cmake` (namespaced `qb::`), `qbConfig.cmake`, and a `qbConfigVersion.cmake` written with `COMPATIBILITY SameMajorVersion` (`CMakeLists.txt:295-299`, generated at `cmake/qbPackage.cmake:212-228`).
- **Find modules for consumers:** `FindArgon2.cmake` is installed when the build resolved Argon2 (`QB_HAS_ARGON2`), and `FindNgtcp2.cmake` when QUIC was enabled (`QB_HAS_QUIC`), so a downstream `find_package(qb)` of a QUIC- or Argon2-enabled build can recreate the imported targets `qb::io` links transitively (`CMakeLists.txt:265-274`).

Downstream then consumes the installed copy with `find_package`:

```cmake
find_package(qb CONFIG REQUIRED)   # provides qb::core and qb::io
target_link_libraries(my_app PRIVATE qb::core qb::io)
```

The two supported integration modes — embed via `add_subdirectory(qb)` or consume an installed copy via `find_package(qb)` — both expose `qb::core` and `qb::io` (`CMakeLists.txt:24-28`). The embed mode is covered in [INSTALL.md](../../INSTALL.md#embed-the-source-tree).

### Link-time configuration fingerprint

qb ships public headers plus a compiled archive, and a handful of macros change the **layout** of
public types in those headers. If an application sets one of them and the archive was not built
with it, `sizeof(qb::Event)` and friends differ between the two halves of the same program — with
no diagnostic from the compiler and none from the linker. Since 3.0.0 that mismatch is a **link
error** instead: `qb/src/qb/utility/abi.h` makes every translation unit that parses a qb header
reference one symbol per axis, named after the value *it* was compiled with, and `libqb-io`
defines those symbols with the values *the archive* was compiled with.

| axis | symbol | set by |
|---|---|---|
| qb version | `qb_abi_version_3_0_0` | `QB_VERSION_*`, published by qb's CMake usage requirements |
| cache line | `qb_abi_cacheline_64` | `KNOWN_L1_CACHE_LINE_SIZE` |
| exceptions | `qb_abi_exceptions_1` | `-fno-exceptions` |
| coroutine tracing | `qb_abi_coroutine_debug_0` | `QB_DEBUG_COROUTINES` |
| jthread source | `qb_abi_std_jthread_1` | `QB_COMPAT_FORCE_THREAD_FALLBACK`, or a standard library without `__cpp_lib_jthread` |

`NDEBUG` is deliberately **not** an axis: a Debug application against a Release qb is supported and
CI-tested. Neither are the feature flags (`QB_HAS_SSL`, `QB_HAS_QUIC`, …) — those arrive through
the imported target's usage requirements, and the qbm package configs check them separately.

#### Do not mix `NDEBUG` across translation units in one program

This is the one configuration hazard qb does **not** turn into a link error, and the reason is that
turning it into one would break the supported case above. It is written down here because it is
live, not theoretical.

qb's public headers contain 39 `assert(` sites and 7 `#if`/`#ifndef NDEBUG` blocks inside `inline`
and template bodies. An `inline` function has *vague linkage*: every translation unit that uses it
emits its own copy and the linker keeps exactly one. When two translation units in the same program
disagree about `NDEBUG`, they emit **two different bodies under one symbol**, and which one survives
is decided by the order the objects reach the linker. Measured, on macOS/ld-prime and on
Linux/GNU ld 2.44, with the same two object files both times:

```
$ c++ main.o tu_dbg.o -o prog && ./prog     # main.o = -DNDEBUG, tu_dbg.o = -UNDEBUG
returned normally (NO assert fired)                                          exit=0

$ c++ tu_dbg.o main.o -o prog && ./prog     # same objects, order swapped
Assertion failed: (_locked && "async_mutex::unlock called on an unlocked mutex"),
function unlock, file sync.h, line 535.                                      exit=134
```

Both outcomes are wrong in the same way: the program's behaviour is not a property of its source.
A release build can abort on a check it never compiled, and a debug build can lose a check it
asked for. This is the standard C++ mixed-`NDEBUG` ODR hazard rather than anything specific to qb,
but qb's headers are large enough that you will hit it.

**The rule: compile every translation unit in one program with the same `NDEBUG` setting.** With
CMake that is automatic — `CMAKE_BUILD_TYPE` applies per target and `NDEBUG` comes from the build
type — so you have to go out of your way to break it: a hand-written `-DNDEBUG` on one file, an
object copied in from another build, or a prebuilt third-party static library compiled the other
way and linked into the same executable.

**A Debug application against a Release qb archive is not this case, and remains supported.** The
archive's own out-of-line code is fixed at build time; only vague-linkage bodies that *both* halves
emit can collide, and the consumer's uniform setting decides those consistently.

Three remedies were considered and rejected, and the reasoning is recorded here so it is not
re-litigated from scratch:

| candidate | why not |
|---|---|
| Add `NDEBUG` to the fingerprint | Makes a Debug-or-default consumer against a Release archive a hard link failure. An unset `CMAKE_BUILD_TYPE` is CMake's **default**, so this would break the most common consumer configuration to fix a rarer one. |
| Compile the 39 asserts unconditionally (`if (…) [[unlikely]]`) | Removes the divergence, but puts a branch in `schedule_via_current`, `generator<T>::iterator::operator*` and the mpsc ring buffer — per-resume and per-element paths — and silently changes what `-DNDEBUG` means for every qb user. |
| An `inline namespace` ABI tag keyed on `NDEBUG` | Correct in isolation, and exactly what makes the two bodies distinct symbols — but it re-mangles every qb symbol per build mode, which is the supported Debug-consumer/Release-archive case broken again, more thoroughly. |

The remaining option — keying the header asserts off the *archive's* build mode via a generated,
installed configuration header — is the only one that removes the divergence at zero release cost.
It is not implemented: it adds an installed header and changes what a Debug consumer sees inside
qb's own inline code, so it is a deliberate design change rather than a patch.

Read the archive's own side with either of:

```bash
nm -g <prefix>/lib/libqb-io.a | grep qb_abi          # one symbol per axis
strings <prefix>/lib/libqb-io.a | grep '^qb-abi '    # qb-abi qb=3.0.0 cacheline=64 exceptions=1 …
```

### One instance per process

A handful of qb entities must exist **exactly once per process**: the event type-id counter and
the per-type id drawn from it, the router's disposer table, the `ServiceActor` registry, the
`no_protocol()` sentinel (compared by *address*), the coroutine frame pool, and the per-thread
`listener` / `VirtualCore` / coroutine scheduler. All of them are vague-linkage entities, which
means "N definitions, one kept" — and *who* keeps it matters: the static linker folds copies within
one image, the dynamic linker coalesces the survivors **across** images.

Two things used to break that, both silently, and both are closed in 3.0.0.

**An out-of-line definition is private to its image.** A `thread_local` defined in a `.cpp` emits
its TLS descriptor as `non-external` (Mach-O) — it can never be shared. A host executable and a
`dlopen`ed plugin that each statically link qb therefore held two `listener::current` on the same
thread, with no unusual flags at all, and everything the plugin registered went into a loop nobody
runs. Those definitions now live `inline` in their headers, which emits a *weak-external*
descriptor that the dynamic linker coalesces. Nothing about how you spell them changed.

**`-fvisibility=hidden` in a consumer stops the coalescing.** Every anchor above carries
`QB_ABI_ANCHOR` (`visibility("default")`) so it keeps merging. One case that annotation cannot
reach: the per-type magic static inside `type_id_for<T>()` is *block-scope*, and a block-scope
static cannot be put back in the export trie. So type identity no longer lives there — the magic
static caches an id owned by `qb::detail::_type_id_registry`, one shared list keyed by
`typeid(T).name()`. That also covers two separate copies of `libqb-core.a` in one process.

Unloading an image that has used qb (`dlclose`) remains unsupported: its watchers live in
`listener::current`, its actors in the engine's routers, and its type-id slots in the registry.

### Macro hygiene

A public header must not take an unprefixed common name. Since 3.0.0 qb's own macros are
`QB_`-prefixed and the unprefixed spellings are either guarded aliases or opt-in:

| what | prefixed spelling | unprefixed spelling |
|---|---|---|
| logging | `QB_LOG_DEBUG` `QB_LOG_VERB` `QB_LOG_INFO` `QB_LOG_WARN` `QB_LOG_CRIT` | **on by default**, each behind its own `#ifndef`; suppress with `QB_NO_LEGACY_LOG_MACROS` |
| sockets | `QB_CLOSESOCKET` `QB_IOCTLSOCKET` `QB_SD_RECEIVE` `QB_SD_SEND` `QB_SD_BOTH` `QB_SD_NONE` `QB_FD_TO_SOCKET` `QB_OPEN_FD_FROM_SOCKET` | **off by default**; restore with `QB_LEGACY_SOCKET_MACROS` |
| qev feature flags | — | the 35 `HAVE_*` are private to `libqev.a` and no longer reach a consumer |
| libevent compat | — | the 24 `event_*` C symbols are built only under `QB_EV_LIBEVENT_COMPAT=ON` |

The two defaults differ because the guard works for one set and not the other. `LOG_INFO` is only
ever a macro, so `#ifndef` fully protects a consumer who defines it first. `closesocket` and
`ioctlsocket` are *function* names: a consumer who writes `static int closesocket(int)` sails
through `#ifndef closesocket`, and an object-like macro then rewrites every one of their calls.

### Which header may I include first?

Every installed header compiles **alone**, and every entry point in the table below **links**
on its own — both are gated in CI (`scripts/check-installed-headers.sh`, run over the `qb` tree by
`install-consume.yml` and over the `qbm` tree by the superproject's `package-consume.yml`).

The distinction that matters is between **umbrellas** and **class headers**:

| include | complete class | member templates (`push<E>`, `spawn`, …) |
|---|---|---|
| `<qb/actor.h>` `<qb/main.h>` `<qb/patterns.h>` `<qb/core/patterns.h>` | yes | **yes** |
| `<qb/core/Actor.h>` `<qb/core/VirtualCore.h>` | yes | **no** |

A class header gives you a complete `qb::Actor` with every member template *declared*, so a TU that
calls `push<E>()` compiles clean and fails at **link**. That is by design and it is not going to
change: the bodies live at the tail of `VirtualCore.h` (they were `Actor.tpp` through
2.6.0), because they need a complete `qb::VirtualCore` — and
`VirtualCore.h` is what drags `<windows.h>`, `WIN32_LEAN_AND_MEAN` and `NOMINMAX` into a TU. Making
every actor TU pay that is the worse trade. **Include an umbrella.**

`<qb/main.h>` was on the wrong side of that table until 3.0.0.

## Platform notes

- **Linux:** POSIX sockets. Use GCC or Clang with solid C++20 support. Install optional dependency headers when enabling features (`libssl-dev`, `libargon2-dev`, `zlib1g-dev` on Debian/Ubuntu; `openssl-devel`, `zlib-devel` on Fedora/RHEL). Install libngtcp2 packages when QUIC is required. qb links `dl` and `rt` (`qb/cmake/qbDependencies.cmake`).
- **macOS:** POSIX sockets. Recent Xcode / Apple Clang. Homebrew supplies optional dependencies (`brew install openssl argon2 zlib`); point CMake at them with `CMAKE_PREFIX_PATH` when needed. On Apple Silicon, native-arch tuning uses `-mcpu=native` because `-march=native` is rejected (`qbCompiler.cmake:274-288`); qb links the `Foundation` framework.
- **Windows:** Winsock2. Use a Visual Studio 2022 (or newer) MSVC toolset that supports `/std:c++23`. For optional features, put OpenSSL/zlib development libraries on `CMAKE_PREFIX_PATH` (or set `OPENSSL_ROOT_DIR`); CI uses vcpkg. qb links `ws2_32` and `mswsock`.

## Pitfalls

- **Bundled deps missing from a checkout.** libev and stduuid are vendored as committed files under `qb/src/qb/vendor/`, not submodules and not fetched; a normal clone always ships them. A `libev … not found` fatal error means they are missing — restore them with `git checkout -- src/qb/vendor` or re-clone. A `git submodule update` will not bring them back.
- **Host-tuned binary fails on another machine.** Only if the build asked for it: `QB_ENABLE_NATIVE_ARCH` defaults to `OFF`, and the only presets that turn it on are `release-native` and `benchmarks`. Check the cache (`cmake -L build/... | grep NATIVE_ARCH`) before blaming codegen — if it reads `OFF`, a SIGILL on another machine is not native-arch and `-DQB_ENABLE_NATIVE_ARCH=OFF` will change nothing.
- **`undefined symbol: qb_abi_…` when linking an application.** Not a missing library — the
  [link-time configuration fingerprint](#link-time-configuration-fingerprint) reporting that the
  application and the archive were compiled with different ABI-relevant settings. The symbol name
  is the application's side; `nm -g <prefix>/lib/libqb-io.a | grep qb_abi` is the archive's.
  Rebuild qb with the same setting (there is no opt-out — the two are unsound together).
  `qb_abi_version_unknown__compile_with_qb_s_cmake_usage_requirements` means the application was
  compiled from a hand-written `-I`/`-l` line, so it is also missing `QB_HAS_SSL` / `QB_HAS_QUIC` /
  `QB_HAS_COMPRESSION` and its inline feature answers contradict the archive's; use
  `find_package(qb)` or reproduce the definitions the imported target carries.
- **`use of undeclared identifier 'closesocket'` (or `SD_BOTH`, `ioctlsocket`, …).** Those macros
  are off by default since 3.0.0 — see [Macro hygiene](#macro-hygiene). Use `QB_CLOSESOCKET` and
  friends, or compile with `-DQB_LEGACY_SOCKET_MACROS`.
- **A plugin's `qb::io` work never runs, or two event types route to the same handler.** Both are
  symptoms of qb existing twice in one process; see [One instance per process](#one-instance-per-process).
  Check that neither image is compiled `-fvisibility=hidden` against a qb older than 3.0.0, and
  prefer one shared qb over two statically linked copies.
- **`undefined symbol: qb::Actor::push<MyEvent>(qb::ActorId const&) const`.** The TU entered through
  a class header (`<qb/core/Actor.h>`, `<qb/core/VirtualCore.h>`) rather than an umbrella. Include
  `<qb/actor.h>`, `<qb/main.h>` or `<qb/patterns.h>` instead — see
  [Which header may I include first?](#which-header-may-i-include-first). The same source can link at
  `-O0` and fail at `-O3`, because a sibling TU may emit a weak out-of-line copy at `-O0` and not at
  `-O3`; a Debug build that links is not evidence.
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
