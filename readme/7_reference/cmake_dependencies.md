<!-- Verified-against: qb 2.6.0 (C++20 default, C++23 supported) -->
# CMake and third-party dependencies

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.6.0 (C++20 default, C++23 supported)

qb groups its dependencies into resolution classes — vendored forks that are qb's own code, third-party trees bundled in-tree, fetched from source on demand, or supplied by the system — and resolves each class with a single, predictable rule driven by `QB_DEPS_FETCH_FALLBACK` and the `QB_USE_SYSTEM_*` switches.

**Prerequisites:** [Building from source](./building.md) — **See also:** [CMake options](./cmake_options.md), [Testing](./testing.md), [FAQ](./faq.md)

## Summary

| Dependency | Class | Required? | Feature gate | Resolved in |
|---|---|---|---|---|
| libev | qb fork, vendored (`include/qb/vendor/ev`) | required | event loop (core/io) | `qbDependencies.cmake` |
| stduuid | qb fork, vendored (`include/qb/vendor/uuid`) | required | UUID generation | `qbDependencies.cmake` |
| nanolog, ska_hash | qb forks, vendored, header-only (`include/qb/vendor/`) | required | logging / hashing | — (no CMake target) |
| nlohmann/json | system (`find_package`) with bundled fallback (`modules/nlohmann`) | required | JSON | `qbDependencies.cmake` |
| GoogleTest | fetched or system | dev-only (`QB_BUILD_TESTS`) | test suite | `qbFetchGoogleDeps.cmake` |
| Google Benchmark | fetched or system | dev-only (`QB_BUILD_BENCHMARKS`) | benchmarks | `qbFetchGoogleDeps.cmake` |
| zlib | fetched or system | optional (`QB_WITH_COMPRESSION`) | compression | `qbDependencies.cmake` |
| OpenSSL | system only | optional (`QB_WITH_SSL`) | SSL/TLS, crypto | `qbDependencies.cmake` |
| Argon2 | system only | optional (under SSL) | password hashing | `qbDependencies.cmake` |
| libngtcp2 (+ crypto_ossl) | system only | optional (`QB_WITH_QUIC`) | QUIC / HTTP/3 | `qbDependencies.cmake` |
| gperftools | system only | optional (`QB_WITH_PROFILING`) | profiling | `qbDependencies.cmake` |

All resolution logic lives in two CMake modules: `cmake/qbDependencies.cmake` (vendored forks, nlohmann, system-only, and zlib) and `cmake/qbFetchGoogleDeps.cmake` (GoogleTest and Google Benchmark).

## CMake version floor

The framework requires **CMake 3.24 or newer** (`qb/CMakeLists.txt:31`). 3.24 is the floor because the fetched dependencies use `FetchContent_Declare(... FIND_PACKAGE_ARGS ...)`, which integrates `find_package` into `FetchContent_MakeAvailable` and is only available from 3.24 (`cmake/qbFetchGoogleDeps.cmake:17`). Two integration modes are supported (`qb/CMakeLists.txt:25-28`): `add_subdirectory(path/to/qb)` for embedding, and `find_package(qb)` for an installed copy. Both yield the `qb::core` and `qb::io` aliases.

## Resolution classes

### Vendored forks

`ev`, `uuid`, `nanolog` and `ska_hash` are not third-party dependencies that qb happens to bundle — they are **qb forks**: qb's own source, diverged from upstream, never swappable for a system copy. They live under `include/qb/vendor/<fork>/` and are therefore reached by a qb-owned include prefix, `<qb/vendor/ev/ev++.h>`, `<qb/vendor/ska_hash/unordered_map.hpp>`, and so on.

That path is not cosmetic. Their headers used to be published by bare name, so an installed qb dropped `ev.h`, `ev++.h`, `event.h`, `event_compat.h`, `ev_config.h`, `uuid.h` and the directories `ev/`, `uuid/`, `nanolog/`, `ska_hash/` straight into the consumer's include root — 12 top-level names, every one of them able to shadow, or be shadowed by, a header the consumer already owned. Living under `qb/vendor/` makes that collision structurally impossible. Being physically inside `include/` also means one include root serves the build tree and the installed tree, with no separate `BUILD_INTERFACE`/`INSTALL_INTERFACE` pair to drift apart.

- **libev** — REQUIRED. `qbDependencies.cmake:104-116` checks that `include/qb/vendor/ev` exists and sets `QB_HAS_LIBEV`; if it is missing, configuration fails with a fatal error. The tree is compiled by `add_subdirectory("${QB_VENDOR_DIR}/ev")` (`qb/CMakeLists.txt:100`), producing the static `ev` target (`include/qb/vendor/ev/CMakeLists.txt:287`). Resolving libev defines `QB_HAS_LIBEV=1` on every qb target (`qbDependencies.cmake:390-392`).
  The fork's generated configuration header is reached through `-DEV_CONFIG_H=<qb/vendor/ev/ev_config.h>`, a `PUBLIC` compile definition on the `ev` target, because `ev.h`'s own fallback lookup for `ev_config.h` is `__has_include`-guarded and would fail *silently* — flipping `EV_MULTIPLICITY` from 1 to 4 and desynchronising every `ev_*` prototype from the compiled library. `qb/io/async/event/base.h` and `qb/io/async/coroutine/scheduler.h` carry an `#error` guard on `EV_MULTIPLICITY` so that miss is a compile error rather than a runtime mystery.
- **stduuid** — REQUIRED in practice. `qbDependencies.cmake:44-47` detects `include/qb/vendor/uuid` and sets `QB_HAS_UUID`. It is added by `add_subdirectory("${QB_VENDOR_DIR}/uuid")` (`qb/CMakeLists.txt:99`), which declares the header-only `stduuid` `INTERFACE` target (`include/qb/vendor/uuid/CMakeLists.txt:22`). The framework pins its options before adding it (`qb/CMakeLists.txt:83-91`): `UUID_BUILD_TESTS`, `UUID_SYSTEM_GENERATOR` and `UUID_TIME_GENERATOR` are forced **off**, while `UUID_USING_CXX20_SPAN` is forced **on**. That last one is load-bearing, not cosmetic: qb requires C++20 so `std::span` always exists, and with it off stduuid takes a `gsl` fallback branch whose directory was deleted in the C++20 migration -- which made `cmake --install` fail outright. Only when the vendored directory is absent does `qbDependencies.cmake:50-95` fall back to a system UUID (pkg-config `uuid`, then `find_path`/`find_library`); if neither is found, the build emits a warning and clears `QB_HAS_UUID` rather than failing.
- **nanolog, ska_hash** — header-only, no CMake target at all. They are ordinary files under `include/qb/vendor/`, reached through qb's single include root like any other qb header. `source/io/src/io.cpp` compiles `nanolog.cpp` by textual inclusion.

Both compiled forks are part of the install export: `ev` and `stduuid` are added to the `qbTargets` export set so their names are rewritten under the `qb::` namespace in the transitive link list of `qb::io`/`qb::core` (`qb/CMakeLists.txt:235-252`). Their headers need no install rule of their own — qb's ordinary public-header rule (`qb/CMakeLists.txt:287-295`) already covers them, which is precisely why the two trees cannot diverge. When embedded, each fork's own standalone install/package-config block is skipped, so an installed qb ships no `lib/cmake/libev/` or `lib/cmake/stduuid/` alongside its own package.

### Third-party: nlohmann/json

nlohmann is the one genuine upstream dependency, and it is handled the opposite way. `nlohmann::json` crosses qb's API boundary (`qb::json` is an alias for it, and `qb/json.h` defines `to_json`/`from_json` for `qb::uuid`), so a consumer compiling against *their* copy while qb was compiled against a private one is an ODR violation on the type — something no include-path rename can fix. `qbDependencies.cmake:320` therefore does `find_package(nlohmann_json 3.11 QUIET)` first and only falls back to the bundled `modules/nlohmann` copy. Either way the result is the `qb-nlohmann` `INTERFACE` target (`qbDependencies.cmake:322`, exported as `qb::nlohmann`), linked `PUBLIC` by `qb-io`.

Consequences worth knowing:

- If qb was built against a system nlohmann, `qbConfig.cmake` calls `find_dependency(nlohmann_json 3.11)` and qb installs **no** copy of its own — a consumer without the package fails at configure time, loudly.
- If qb fell back to the bundle, the bundle is installed at `<prefix>/include/nlohmann/` — the library's canonical spelling. Unlike the forks, a consumer's own copy winning the include race here is correct, not a bug.
- nlohmann encodes its version and a few options in an inline namespace (`json_abi_v3_12_0`), so a version mismatch surfaces as a link error. `JSON_NOEXCEPTION` and `JSON_USE_IMPLICIT_CONVERSIONS` change the class but not that tag, so they can still mismatch silently. Building qb in your own tree (`add_subdirectory` / `FetchContent`) is the only configuration that closes this completely.

### Fetched dependencies

Fetched dependencies build cleanly from source with CMake, so qb can resolve them system-first and fall back to a pinned-tag source build. Three dependencies are fetchable: **GoogleTest**, **Google Benchmark**, and **zlib**.

- **GoogleTest** — resolved in `qbFetchGoogleDeps.cmake:48-93`, only when `QB_BUILD_TESTS` is ON (the default). Cache options are forced before the fetch: `BUILD_GMOCK=ON`, `INSTALL_GTEST=OFF`, and the gtest/gmock self-tests off (`qbFetchGoogleDeps.cmake:56-59`). On MSVC, `gtest_force_shared_crt=ON` (`qbFetchGoogleDeps.cmake:53-55`). When built from source under Clang/AppleClang, qb adds `-Wno-character-conversion` to the `gtest` target to silence a third-party `char8_t` warning (`qbFetchGoogleDeps.cmake:85-88`).
- **Google Benchmark** — resolved in `qbFetchGoogleDeps.cmake:98-129`, only when `QB_BUILD_BENCHMARKS` is ON (off by default). Cache options forced before the fetch: `BENCHMARK_ENABLE_TESTING=OFF`, `BENCHMARK_DOWNLOAD_DEPENDENCIES=OFF` (`qbFetchGoogleDeps.cmake:103-104`).
- **zlib** — resolved in `qbDependencies.cmake:154-192`, only when `QB_WITH_COMPRESSION` is ON (the default). zlib is searched with `find_package(ZLIB QUIET)` first; if absent and `QB_DEPS_FETCH_FALLBACK` is ON, it is built from `madler/zlib` at `QB_ZLIB_GIT_TAG`. Because `madler/zlib` exposes `zlib`/`zlibstatic` but no `ZLIB::ZLIB` target, qb normalizes an `ZLIB::ZLIB` alias (`qbDependencies.cmake:171-179`). Resolving zlib defines `QB_HAS_COMPRESSION=1`; if it is requested but cannot be found or built, the build warns and forces `QB_WITH_COMPRESSION` off (`qbDependencies.cmake:189-192`).

> Note: GoogleTest and Google Benchmark integrate `find_package` into the fetch through `FIND_PACKAGE_ARGS` (`qbFetchGoogleDeps.cmake:61-72, 107-119`), so `QB_DEPS_FETCH_FALLBACK=OFF` makes them ignore the system entirely and always build from the pinned tag. zlib uses an explicit `find_package`-then-`FetchContent` sequence, so for zlib, `QB_DEPS_FETCH_FALLBACK=OFF` means "use the system package if present, otherwise disable compression" — it never reaches the source build. See [Resolution policy](#resolution-policy-qb_deps_fetch_fallback) for the exact behavior.

### System-only dependencies

System-only dependencies have no clean CMake source build, so qb never fetches them. Each is located with `find_package` and gates an optional feature; when absent, the feature is disabled rather than the build failing.

- **OpenSSL** — `find_package(OpenSSL QUIET)`, only when `QB_WITH_SSL` is ON (the default), at `qbDependencies.cmake:124-150`. On success it links `OpenSSL::SSL` and `OpenSSL::Crypto` and sets `QB_HAS_SSL`. If absent, it warns, clears `QB_HAS_SSL`, and forces `QB_WITH_SSL` off (`qbDependencies.cmake:142-146`). `QB_HAS_SSL=1` is defined on qb targets when present.
- **Argon2** — `find_package(Argon2 QUIET)`, searched **only inside the OpenSSL-found branch** (`qbDependencies.cmake:132-141`). A build without OpenSSL can therefore never have `QB_HAS_ARGON2`. The bundled `cmake/FindArgon2.cmake` creates the `Argon2::Argon2` imported target (`FindArgon2.cmake:59-61`); on success qb links it and defines `QB_HAS_ARGON2=1`. If Argon2 is missing, qb falls back to its non-Argon2 crypto paths (`qbDependencies.cmake:139`).
- **libngtcp2 (+ ngtcp2_crypto_ossl)** — the QUIC transport stack, governed by the tri-state `QB_WITH_QUIC` (`qbDependencies.cmake:198-236`). QUIC **requires SSL**: if `QB_HAS_SSL` is false, QUIC is disabled regardless of the request. When SSL is present, `find_package(Ngtcp2 QUIET)` (bundled `cmake/FindNgtcp2.cmake`) creates the `Ngtcp2::ngtcp2` and `Ngtcp2::crypto_ossl` imported targets (`FindNgtcp2.cmake:79-90`); on success qb links both and defines `QB_HAS_QUIC=1`. Distro packages may also provide `libngtcp2-crypto-gnutls-dev`, but that is not a drop-in replacement for qb's current native backend because `source/io/src/quic.cpp` calls the ngtcp2 OpenSSL helper APIs. See [QUIC tri-state](#quic-tri-state-qb_with_quic) for the AUTO/ON/OFF semantics.
- **gperftools** — `find_package(Gperftools QUIET)`, only when `QB_WITH_PROFILING` is ON (off by default), at `qbDependencies.cmake:248-268`. The bundled `cmake/FindGperftools.cmake` creates `Gperftools::Profiler` and `Gperftools::TCMalloc` (among other targets); qb links whichever exist and sets `QB_HAS_PROFILING`. If absent, it warns and forces `QB_WITH_PROFILING` off.

The bundled find-modules for Argon2 and ngtcp2 are installed alongside the package config so that `find_package(qb)` consumers of an Argon2- or QUIC-enabled build can recreate the same imported targets (`qb/CMakeLists.txt:343-356`).

## QB_HAS_* versus QB_WITH_*

The two prefixes are not interchangeable.

- `QB_WITH_*` are **user-facing requests** — options you set on the command line (`QB_WITH_SSL`, `QB_WITH_COMPRESSION`, `QB_WITH_QUIC`, `QB_WITH_PROFILING`).
- `QB_HAS_*` are **resolved results** — what qb actually found after probing (`QB_HAS_SSL`, `QB_HAS_COMPRESSION`, `QB_HAS_QUIC`, `QB_HAS_ARGON2`, `QB_HAS_PROFILING`, `QB_HAS_UUID`, `QB_HAS_LIBEV`).

A `QB_HAS_*` flag drives the corresponding `QB_HAS_*=1` compile definition on qb targets (`qbDependencies.cmake:331-353`). When a requested feature's dependency is missing, qb forces the `QB_WITH_*` option back off so the recorded request matches reality.

## Resolution policy: QB_DEPS_FETCH_FALLBACK

`QB_DEPS_FETCH_FALLBACK` (default **ON**, `qbConfig.cmake:70`) selects how the fetchable dependencies behave. The fetched-via-`FIND_PACKAGE_ARGS` dependencies (GoogleTest, Google Benchmark) and zlib differ at the OFF setting.

| Setting | GoogleTest / Google Benchmark | zlib |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK=ON` (default) | Use the system package if `find_package` locates it; otherwise build the pinned tag from source. | Same: system if present, else build the pinned tag from source. |
| `QB_DEPS_FETCH_FALLBACK=OFF` | **Always build the pinned tag from source** — the system package is ignored (no `FIND_PACKAGE_ARGS`). | Use the system package if present; otherwise compression is disabled (no source build is attempted). |
| `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` | Force `find_package(... CONFIG REQUIRED)` — require a system package, never fetch; configuration fails if missing. | (not applicable) |

The mechanism: for GoogleTest and Google Benchmark, qb appends `FIND_PACKAGE_ARGS NAMES <pkg>` to the `FetchContent_Declare` **only when `QB_DEPS_FETCH_FALLBACK` is ON** (`qbFetchGoogleDeps.cmake:61-64, 107-110`). With the argument present, `FetchContent_MakeAvailable` tries `find_package` first and falls back to the source build; without it, it always builds from source. The `QB_USE_SYSTEM_*` switches short-circuit this entirely with an explicit `find_package(... CONFIG REQUIRED)` (`qbFetchGoogleDeps.cmake:49-51, 99-101`).

System-only dependencies (OpenSSL, Argon2, libngtcp2, gperftools) are unaffected by `QB_DEPS_FETCH_FALLBACK`: they are never fetched under any setting.

## Pinning fetched versions

Three advanced cache variables pin the Git tag (or SHA) used for source builds (`qbConfig.cmake:75-78`; `mark_as_advanced`):

| Variable | Default | Applies to |
|---|---|---|
| `QB_GOOGLETEST_GIT_TAG` | `v1.15.2` | GoogleTest source build |
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.2` | Google Benchmark source build |
| `QB_ZLIB_GIT_TAG` | `v1.3.1` | zlib fallback source build |

Override at configure time:

```bash
# src: derived from qb/cmake/qbConfig.cmake:75-78 + qbFetchGoogleDeps.cmake:57-63
cmake -B build -S . -DQB_GOOGLETEST_GIT_TAG=v1.14.0
```

## Forcing system packages

The default already prefers a system package when present. To **require** a system GoogleTest/Benchmark and fail (never fetch) when it is absent:

```bash
# src: derived from qb/cmake/qbFetchGoogleDeps.cmake:49-51,99-101
cmake -B build -S . \
  -DQB_USE_SYSTEM_GTEST=ON \
  -DQB_USE_SYSTEM_BENCHMARK=ON
```

This issues `find_package(GTest CONFIG REQUIRED)` and `find_package(benchmark CONFIG REQUIRED)`, resolving packages that ship CMake config files (vcpkg, Conan, distribution `-dev` packages).

## QUIC tri-state: QB_WITH_QUIC

`QB_WITH_QUIC` is a three-valued cache string with default **AUTO** (`qbConfig.cmake:103-104`), resolved at `qbDependencies.cmake:202-236`:

The value is matched case-insensitively (`qbDependencies.cmake:202`). The OFF set is matched explicitly; AUTO is matched explicitly; any other value is treated as a required ON.

| Value | Behavior when libngtcp2 is missing |
|---|---|
| `AUTO` (default) | Enable QUIC if libngtcp2 is found; stay silent (no warning) when absent. |
| Any value not in the OFF set and not `AUTO` (for example `ON`, `TRUE`, `1`, `YES`, `Y`) | Require libngtcp2; warn and disable QUIC if it (or SSL) is missing. |
| `OFF`, `FALSE`, `0`, `NO`, or `N` | Disabled outright; no search performed (`qbDependencies.cmake:203-204`). |

In every case, QUIC additionally requires `QB_HAS_SSL` — without OpenSSL, QUIC is disabled regardless of `QB_WITH_QUIC` (and a required ON warns about it). AUTO mirrors how SSL and compression silently auto-detect.

## Source layout of fetched dependencies

When a fetchable dependency is built from source, `FetchContent` places its tree under the build directory, for example `build/_deps/googletest-src`, `build/_deps/googlebenchmark-src`, and `build/_deps/zlib-src` (the exact path follows the `FetchContent` naming convention). A system package, by contrast, leaves no `_deps` entry — the per-dependency status message states which path was taken (`qbFetchGoogleDeps.cmake:75-79, 121-125`).

## Offline and CI builds

- A source fallback needs **network access to GitHub** on first configure (`GIT_REPOSITORY` with `GIT_SHALLOW TRUE`), unless the sources are pre-populated or a system package is found. [Building](./building.md) notes that Git is required on the configure machine for this reason.
- For air-gapped or reproducible builds: provide the dependencies as system packages on `CMAKE_PREFIX_PATH` and set `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` (require system, never fetch), or pre-populate the `_deps` directory before configuring.
- Leaving `QB_DEPS_FETCH_FALLBACK=ON` with no system packages present is the most convenient default for a fresh machine, but it makes the first configure depend on GitHub reachability.

## Pitfalls

- **`QB_DEPS_FETCH_FALLBACK=OFF` does not mean "never fetch."** For GoogleTest and Google Benchmark it means the opposite — *always* build from source, ignoring the system. To require a system package and never fetch, use `QB_USE_SYSTEM_GTEST` / `QB_USE_SYSTEM_BENCHMARK` instead.
- **Argon2 is invisible without OpenSSL.** Its `find_package` is nested inside the OpenSSL-found branch, so a non-SSL build silently has `QB_HAS_ARGON2=OFF` even if libargon2 is installed.
- **QUIC silently disables without SSL.** With `QB_WITH_QUIC=AUTO` and no OpenSSL, QUIC is off with no warning. Use `QB_WITH_QUIC=ON` to make the missing prerequisite surface as a warning.
- **A missing optional dependency does not fail the build.** SSL, compression, QUIC, and profiling each warn and force their `QB_WITH_*` option off when their dependency is absent. Only **libev** (vendored fork, `qbDependencies.cmake:113`) and the **Threads** package (`find_package(Threads REQUIRED)`, `qbCompiler.cmake:389`) are hard-required; their absence is fatal.
- **Stale googletest/googlebenchmark submodule folders.** The framework no longer vendors these as Git submodules. Older clones may retain `modules/googletest`/`modules/googlebenchmark` directories; CMake ignores them, and you can delete the folders and any stale `.git/config` submodule entries.

## See also

- [Building the QB Actor Framework](./building.md) — full configure/build walkthrough and the complete option matrix.
- [Testing](./testing.md) — how `QB_BUILD_TESTS` and the resolved GoogleTest drive `qb_add_test` / ctest.
- [FAQ](./faq.md) — common configure-time questions.
- [CMake FetchContent module](https://cmake.org/cmake/help/latest/module/FetchContent.html) — upstream reference for `FIND_PACKAGE_ARGS` and `MakeAvailable`.
