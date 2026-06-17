<!-- Verified-against: qb 2.0.0 (C++20 default, C++23 supported) -->
# CMake and third-party dependencies

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

qb groups every third-party dependency into one of three resolution classes — bundled in-tree, fetched from source on demand, or supplied by the system — and resolves each class with a single, predictable rule driven by `QB_DEPS_FETCH_FALLBACK` and the `QB_USE_SYSTEM_*` switches.

**Prerequisites:** [Building from source](./building.md) — **See also:** [CMake options](./cmake_options.md), [Testing](./testing.md), [FAQ](./faq.md)

## Summary

| Dependency | Class | Required? | Feature gate | Resolved in |
|---|---|---|---|---|
| libev | bundled (`modules/ev`) | required | event loop (core/io) | `qbDependencies.cmake` |
| stduuid | bundled (`modules/uuid`) | required | UUID generation | `qbDependencies.cmake` |
| nanolog, nlohmann, ska_hash | bundled, header-only | required | logging / JSON / hashing | `qbDependencies.cmake` |
| GoogleTest | fetched or system | dev-only (`QB_BUILD_TESTS`) | test suite | `qbFetchGoogleDeps.cmake` |
| Google Benchmark | fetched or system | dev-only (`QB_BUILD_BENCHMARKS`) | benchmarks | `qbFetchGoogleDeps.cmake` |
| zlib | fetched or system | optional (`QB_WITH_COMPRESSION`) | compression | `qbDependencies.cmake` |
| OpenSSL | system only | optional (`QB_WITH_SSL`) | SSL/TLS, crypto | `qbDependencies.cmake` |
| Argon2 | system only | optional (under SSL) | password hashing | `qbDependencies.cmake` |
| libngtcp2 (+ crypto_ossl) | system only | optional (`QB_WITH_QUIC`) | QUIC / HTTP/3 | `qbDependencies.cmake` |
| gperftools | system only | optional (`QB_WITH_PROFILING`) | profiling | `qbDependencies.cmake` |

All resolution logic lives in two CMake modules: `cmake/qbDependencies.cmake` (bundled, system-only, and zlib) and `cmake/qbFetchGoogleDeps.cmake` (GoogleTest and Google Benchmark).

## CMake version floor

The framework requires **CMake 3.24 or newer** (`qb/CMakeLists.txt:31`). 3.24 is the floor because the fetched dependencies use `FetchContent_Declare(... FIND_PACKAGE_ARGS ...)`, which integrates `find_package` into `FetchContent_MakeAvailable` and is only available from 3.24 (`cmake/qbFetchGoogleDeps.cmake:17`). Two integration modes are supported (`qb/CMakeLists.txt:25-28`): `add_subdirectory(path/to/qb)` for embedding, and `find_package(qb)` for an installed copy. Both yield the `qb::core` and `qb::io` aliases.

## Resolution classes

### Bundled dependencies

Bundled dependencies ship inside the qb source tree under `modules/` and are resolved with the framework — never fetched, never searched on the system in the normal path. Only libev is compiled; the others are header-only or `INTERFACE` targets.

- **libev** — REQUIRED. qb vendors a customized libev in `modules/ev`. `qbDependencies.cmake:104-116` checks that the directory exists and sets `QB_HAS_LIBEV`; if the directory is missing, configuration fails with a fatal error. The bundled tree is compiled by `add_subdirectory("${QB_MODULES_DIR}/ev")` (`qb/CMakeLists.txt:73`), producing the static `ev` target (`modules/ev/CMakeLists.txt:208`). Resolving libev defines `QB_HAS_LIBEV=1` on every qb target (`qbDependencies.cmake:351-353`).
- **stduuid** — REQUIRED in practice. The bundled UUID library lives in `modules/uuid`; `qbDependencies.cmake:44-47` detects it and sets `QB_HAS_UUID`. It is added by `add_subdirectory("${QB_MODULES_DIR}/uuid")` (`qb/CMakeLists.txt:72`), which declares the header-only `stduuid` `INTERFACE` target (`modules/uuid/CMakeLists.txt:11`). The framework forces its options off before adding it (`qb/CMakeLists.txt:66-69`): `UUID_BUILD_TESTS`, `UUID_SYSTEM_GENERATOR`, `UUID_TIME_GENERATOR`, and `UUID_USING_CXX20_SPAN`. Only when the bundled directory is absent does `qbDependencies.cmake:50-95` fall back to a system UUID (pkg-config `uuid`, then `find_path`/`find_library`); if neither bundled nor system UUID is found, the build emits a warning and clears `QB_HAS_UUID` rather than failing.
- **nanolog, nlohmann, ska_hash** — header-only bundled modules listed in `QB_HEADER_ONLY_MODULES` (`qbDependencies.cmake:311`). Their include directories are propagated as `PUBLIC` build-interface paths on every qb target, so consumers compiling against `qb::io`/`qb::core` see them transitively.

Bundled targets are part of the install export. When `QB_INSTALL=ON`, `ev` and `stduuid` are added to the `qbTargets` export set so their names are rewritten under the `qb::` namespace in the transitive link list of `qb::io`/`qb::core` (`qb/CMakeLists.txt:174-208`). The header-only modules are installed as plain header trees (`qb/CMakeLists.txt:220-232`).

### Fetched dependencies

Fetched dependencies build cleanly from source with CMake, so qb can resolve them system-first and fall back to a pinned-tag source build. Three dependencies are fetchable: **GoogleTest**, **Google Benchmark**, and **zlib**.

- **GoogleTest** — resolved in `qbFetchGoogleDeps.cmake:39-82`, only when `QB_BUILD_TESTS` is ON (the default). Cache options are forced before the fetch: `BUILD_GMOCK=ON`, `INSTALL_GTEST=OFF`, and the gtest/gmock self-tests off (`qbFetchGoogleDeps.cmake:47-50`). On MSVC, `gtest_force_shared_crt=ON` (`qbFetchGoogleDeps.cmake:44-46`). When built from source under Clang/AppleClang, qb adds `-Wno-character-conversion` to the `gtest` target to silence a third-party `char8_t` warning (`qbFetchGoogleDeps.cmake:74-77`).
- **Google Benchmark** — resolved in `qbFetchGoogleDeps.cmake:87-117`, only when `QB_BUILD_BENCHMARKS` is ON (off by default). Cache options forced before the fetch: `BENCHMARK_ENABLE_TESTING=OFF`, `BENCHMARK_DOWNLOAD_DEPENDENCIES=OFF` (`qbFetchGoogleDeps.cmake:92-93`).
- **zlib** — resolved in `qbDependencies.cmake:154-192`, only when `QB_WITH_COMPRESSION` is ON (the default). zlib is searched with `find_package(ZLIB QUIET)` first; if absent and `QB_DEPS_FETCH_FALLBACK` is ON, it is built from `madler/zlib` at `QB_ZLIB_GIT_TAG`. Because `madler/zlib` exposes `zlib`/`zlibstatic` but no `ZLIB::ZLIB` target, qb normalizes an `ZLIB::ZLIB` alias (`qbDependencies.cmake:171-179`). Resolving zlib defines `QB_HAS_COMPRESSION=1`; if it is requested but cannot be found or built, the build warns and forces `QB_WITH_COMPRESSION` off (`qbDependencies.cmake:189-192`).

> Note: GoogleTest and Google Benchmark integrate `find_package` into the fetch through `FIND_PACKAGE_ARGS` (`qbFetchGoogleDeps.cmake:52-63, 95-106`), so `QB_DEPS_FETCH_FALLBACK=OFF` makes them ignore the system entirely and always build from the pinned tag. zlib uses an explicit `find_package`-then-`FetchContent` sequence, so for zlib, `QB_DEPS_FETCH_FALLBACK=OFF` means "use the system package if present, otherwise disable compression" — it never reaches the source build. See [Resolution policy](#resolution-policy-qb_deps_fetch_fallback) for the exact behavior.

### System-only dependencies

System-only dependencies have no clean CMake source build, so qb never fetches them. Each is located with `find_package` and gates an optional feature; when absent, the feature is disabled rather than the build failing.

- **OpenSSL** — `find_package(OpenSSL QUIET)`, only when `QB_WITH_SSL` is ON (the default), at `qbDependencies.cmake:124-150`. On success it links `OpenSSL::SSL` and `OpenSSL::Crypto` and sets `QB_HAS_SSL`. If absent, it warns, clears `QB_HAS_SSL`, and forces `QB_WITH_SSL` off (`qbDependencies.cmake:142-146`). `QB_HAS_SSL=1` is defined on qb targets when present.
- **Argon2** — `find_package(Argon2 QUIET)`, searched **only inside the OpenSSL-found branch** (`qbDependencies.cmake:132-141`). A build without OpenSSL can therefore never have `QB_HAS_ARGON2`. The bundled `cmake/FindArgon2.cmake` creates the `Argon2::Argon2` imported target (`FindArgon2.cmake:59-61`); on success qb links it and defines `QB_HAS_ARGON2=1`. If Argon2 is missing, qb falls back to its non-Argon2 crypto paths (`qbDependencies.cmake:139`).
- **libngtcp2 (+ ngtcp2_crypto_ossl)** — the QUIC transport stack, governed by the tri-state `QB_WITH_QUIC` (`qbDependencies.cmake:198-236`). QUIC **requires SSL**: if `QB_HAS_SSL` is false, QUIC is disabled regardless of the request. When SSL is present, `find_package(Ngtcp2 QUIET)` (bundled `cmake/FindNgtcp2.cmake`) creates the `Ngtcp2::ngtcp2` and `Ngtcp2::crypto_ossl` imported targets (`FindNgtcp2.cmake:50-60`); on success qb links both and defines `QB_HAS_QUIC=1`. See [QUIC tri-state](#quic-tri-state-qb_with_quic) for the AUTO/ON/OFF semantics.
- **gperftools** — `find_package(Gperftools QUIET)`, only when `QB_WITH_PROFILING` is ON (off by default), at `qbDependencies.cmake:248-268`. The bundled `cmake/FindGperftools.cmake` creates `Gperftools::Profiler` and `Gperftools::TCMalloc` (among other targets); qb links whichever exist and sets `QB_HAS_PROFILING`. If absent, it warns and forces `QB_WITH_PROFILING` off.

The bundled find-modules for Argon2 and ngtcp2 are installed alongside the package config so that `find_package(qb)` consumers of an Argon2- or QUIC-enabled build can recreate the same imported targets (`qb/CMakeLists.txt:256-269`).

## QB_HAS_* versus QB_WITH_*

The two prefixes are not interchangeable.

- `QB_WITH_*` are **user-facing requests** — options you set on the command line (`QB_WITH_SSL`, `QB_WITH_COMPRESSION`, `QB_WITH_QUIC`, `QB_WITH_PROFILING`).
- `QB_HAS_*` are **resolved results** — what qb actually found after probing (`QB_HAS_SSL`, `QB_HAS_COMPRESSION`, `QB_HAS_QUIC`, `QB_HAS_ARGON2`, `QB_HAS_PROFILING`, `QB_HAS_UUID`, `QB_HAS_LIBEV`).

A `QB_HAS_*` flag drives the corresponding `QB_HAS_*=1` compile definition on qb targets (`qbDependencies.cmake:331-353`). When a requested feature's dependency is missing, qb forces the `QB_WITH_*` option back off so the recorded request matches reality.

## Resolution policy: QB_DEPS_FETCH_FALLBACK

`QB_DEPS_FETCH_FALLBACK` (default **ON**, `qbConfig.cmake:72`) selects how the fetchable dependencies behave. The fetched-via-`FIND_PACKAGE_ARGS` dependencies (GoogleTest, Google Benchmark) and zlib differ at the OFF setting.

| Setting | GoogleTest / Google Benchmark | zlib |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK=ON` (default) | Use the system package if `find_package` locates it; otherwise build the pinned tag from source. | Same: system if present, else build the pinned tag from source. |
| `QB_DEPS_FETCH_FALLBACK=OFF` | **Always build the pinned tag from source** — the system package is ignored (no `FIND_PACKAGE_ARGS`). | Use the system package if present; otherwise compression is disabled (no source build is attempted). |
| `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` | Force `find_package(... CONFIG REQUIRED)` — require a system package, never fetch; configuration fails if missing. | (not applicable) |

The mechanism: for GoogleTest and Google Benchmark, qb appends `FIND_PACKAGE_ARGS NAMES <pkg>` to the `FetchContent_Declare` **only when `QB_DEPS_FETCH_FALLBACK` is ON** (`qbFetchGoogleDeps.cmake:52-55, 95-98`). With the argument present, `FetchContent_MakeAvailable` tries `find_package` first and falls back to the source build; without it, it always builds from source. The `QB_USE_SYSTEM_*` switches short-circuit this entirely with an explicit `find_package(... CONFIG REQUIRED)` (`qbFetchGoogleDeps.cmake:40-42, 88-90`).

System-only dependencies (OpenSSL, Argon2, libngtcp2, gperftools) are unaffected by `QB_DEPS_FETCH_FALLBACK`: they are never fetched under any setting.

## Pinning fetched versions

Three advanced cache variables pin the Git tag (or SHA) used for source builds (`qbConfig.cmake:77-80`; `mark_as_advanced`):

| Variable | Default | Applies to |
|---|---|---|
| `QB_GOOGLETEST_GIT_TAG` | `v1.15.2` | GoogleTest source build |
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.2` | Google Benchmark source build |
| `QB_ZLIB_GIT_TAG` | `v1.3.1` | zlib fallback source build |

Override at configure time:

```bash
# src: derived from qb/cmake/qbConfig.cmake:77-80 + qbFetchGoogleDeps.cmake:57-63
cmake -B build -S . -DQB_GOOGLETEST_GIT_TAG=v1.14.0
```

## Forcing system packages

The default already prefers a system package when present. To **require** a system GoogleTest/Benchmark and fail (never fetch) when it is absent:

```bash
# src: derived from qb/cmake/qbFetchGoogleDeps.cmake:40-42,88-90
cmake -B build -S . \
  -DQB_USE_SYSTEM_GTEST=ON \
  -DQB_USE_SYSTEM_BENCHMARK=ON
```

This issues `find_package(GTest CONFIG REQUIRED)` and `find_package(benchmark CONFIG REQUIRED)`, resolving packages that ship CMake config files (vcpkg, Conan, distribution `-dev` packages).

## QUIC tri-state: QB_WITH_QUIC

`QB_WITH_QUIC` is a three-valued cache string with default **AUTO** (`qbConfig.cmake:105-106`), resolved at `qbDependencies.cmake:202-236`:

The value is matched case-insensitively (`qbDependencies.cmake:202`). The OFF set is matched explicitly; AUTO is matched explicitly; any other value is treated as a required ON.

| Value | Behavior when libngtcp2 is missing |
|---|---|
| `AUTO` (default) | Enable QUIC if libngtcp2 is found; stay silent (no warning) when absent. |
| Any value not in the OFF set and not `AUTO` (for example `ON`, `TRUE`, `1`, `YES`, `Y`) | Require libngtcp2; warn and disable QUIC if it (or SSL) is missing. |
| `OFF`, `FALSE`, `0`, `NO`, or `N` | Disabled outright; no search performed (`qbDependencies.cmake:203-204`). |

In every case, QUIC additionally requires `QB_HAS_SSL` — without OpenSSL, QUIC is disabled regardless of `QB_WITH_QUIC` (and a required ON warns about it). AUTO mirrors how SSL and compression silently auto-detect.

## Source layout of fetched dependencies

When a fetchable dependency is built from source, `FetchContent` places its tree under the build directory, for example `build/_deps/googletest-src`, `build/_deps/googlebenchmark-src`, and `build/_deps/zlib-src` (the exact path follows the `FetchContent` naming convention). A system package, by contrast, leaves no `_deps` entry — the per-dependency status message states which path was taken (`qbFetchGoogleDeps.cmake:66-70, 109-113`).

## Offline and CI builds

- A source fallback needs **network access to GitHub** on first configure (`GIT_REPOSITORY` with `GIT_SHALLOW TRUE`), unless the sources are pre-populated or a system package is found. [Building](./building.md) notes that Git is required on the configure machine for this reason.
- For air-gapped or reproducible builds: provide the dependencies as system packages on `CMAKE_PREFIX_PATH` and set `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` (require system, never fetch), or pre-populate the `_deps` directory before configuring.
- Leaving `QB_DEPS_FETCH_FALLBACK=ON` with no system packages present is the most convenient default for a fresh machine, but it makes the first configure depend on GitHub reachability.

## Pitfalls

- **`QB_DEPS_FETCH_FALLBACK=OFF` does not mean "never fetch."** For GoogleTest and Google Benchmark it means the opposite — *always* build from source, ignoring the system. To require a system package and never fetch, use `QB_USE_SYSTEM_GTEST` / `QB_USE_SYSTEM_BENCHMARK` instead.
- **Argon2 is invisible without OpenSSL.** Its `find_package` is nested inside the OpenSSL-found branch, so a non-SSL build silently has `QB_HAS_ARGON2=OFF` even if libargon2 is installed.
- **QUIC silently disables without SSL.** With `QB_WITH_QUIC=AUTO` and no OpenSSL, QUIC is off with no warning. Use `QB_WITH_QUIC=ON` to make the missing prerequisite surface as a warning.
- **A missing optional dependency does not fail the build.** SSL, compression, QUIC, and profiling each warn and force their `QB_WITH_*` option off when their dependency is absent. Only **libev** (bundled, `qbDependencies.cmake:113`) and the **Threads** package (`find_package(Threads REQUIRED)`, `qbCompiler.cmake:344`) are hard-required; their absence is fatal.
- **Stale googletest/googlebenchmark submodule folders.** The framework no longer vendors these as Git submodules. Older clones may retain `modules/googletest`/`modules/googlebenchmark` directories; CMake ignores them, and you can delete the folders and any stale `.git/config` submodule entries.

## See also

- [Building the QB Actor Framework](./building.md) — full configure/build walkthrough and the complete option matrix.
- [Testing](./testing.md) — how `QB_BUILD_TESTS` and the resolved GoogleTest drive `qb_add_test` / ctest.
- [FAQ](./faq.md) — common configure-time questions.
- [CMake FetchContent module](https://cmake.org/cmake/help/latest/module/FetchContent.html) — upstream reference for `FIND_PACKAGE_ARGS` and `MakeAvailable`.
