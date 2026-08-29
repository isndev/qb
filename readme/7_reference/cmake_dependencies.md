<!-- Verified-against: qb 3.0.1 (C++20 default, C++23 supported) -->
# CMake and third-party dependencies

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.1 (C++20 default, C++23 supported)

qb groups its dependencies into resolution classes — vendored forks that are qb's own code, third-party trees bundled in-tree, fetched from source on demand, or supplied by the system — and resolves each class with a single, predictable rule driven by `QB_DEPS_FETCH_FALLBACK` and the `QB_USE_SYSTEM_*` switches.

**Prerequisites:** [Building from source](./building.md) — **See also:** [CMake options](./cmake_options.md), [Testing](./testing.md), [FAQ](./faq.md)

## Summary

| Dependency | Class | Required? | Feature gate | Resolved in |
|---|---|---|---|---|
| qev (libev fork) | qb fork, in-tree (`src/qb/ev`) | required | event loop (core/io) | `qbDependencies.cmake` |
| stduuid | qb fork, vendored (`src/qb/vendor/uuid`) | required | UUID generation | `qbDependencies.cmake` |
| nanolog, ska_hash | qb forks, vendored, header-only (`src/qb/vendor/`) | required | logging / hashing | — (no CMake target) |
| nlohmann/json | system (`find_package`) with pinned `FetchContent` fallback | required | JSON | `qbDependencies.cmake` |
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

`qev`, `uuid`, `nanolog` and `ska_hash` are not third-party dependencies that qb happens to bundle — they are **qb forks**: qb's own source, diverged from upstream, never swappable for a system copy. Each is reached by a qb-owned include prefix — `<qb/ev/ev++.h>`, `<qb/vendor/ska_hash/unordered_map.hpp>`, and so on — so an installed qb owns every top-level name it puts in a consumer's include root.

`uuid`, `nanolog` and `ska_hash` sit under `src/qb/vendor/`. **The event loop does not, and that is deliberate.** `vendor/` means "a copy we re-pull from upstream"; upstream libev is unmaintained at 4.33/4.35, and this tree carries qb's own fixes to the Windows, wepoll and kqueue paths. Filing it as vendored would tell the next maintainer not to touch the engine of qb-io. It lives at `src/qb/ev/` and is published standalone as **[qev](https://github.com/isndev/qev)**; this tree is the source of truth for that repository. None of which weakens the attribution: it derives from libev and from wepoll, both BSD-2, and `scripts/check-vendor-attribution.py` still covers it at the new path.

**The two builds are the same source and different products, and they never share a path.** `qev` standalone is the full library — all fourteen watcher families, `libqev.a`, `<qev/ev.h>`, `find_package(qev)`. qb embeds a **reduced** profile of it: the seven families qb-io never uses are compiled out (`QB_EV_WATCHERS_FULL=OFF`), which is 16 fewer symbols and a `struct ev_loop` 296 bytes smaller — so qb ships `libqb-ev.a` under `<qb/ev/ev.h>`, reached as `qb::ev`. Same-named files with different content is exactly the collision this fork exists to close, so the two install sets are disjoint by construction and a matrix cell (`shared-prefix-install`) proves it on every CI run.

The C **API** is libev's, unchanged — `ev_run`, `ev_io_start`, `struct ev_loop`. Porting a libev program is one include-path edit. What is ours is everything that reaches a filesystem, so an installed qev never overwrites an installed libev.

That path is not cosmetic. Their headers used to be published by bare name, so an installed qb dropped `ev.h`, `ev++.h`, `event.h`, `event_compat.h`, `ev_config.h`, `uuid.h` and the directories `ev/`, `uuid/`, `nanolog/`, `ska_hash/` straight into the consumer's include root — 12 top-level names, every one of them able to shadow, or be shadowed by, a header the consumer already owned. Living under `qb/vendor/` makes that collision structurally impossible. Being physically inside the include root (`src/`) also means one include root serves the build tree and the installed tree, with no separate `BUILD_INTERFACE`/`INSTALL_INTERFACE` pair to drift apart.

- **libev** — REQUIRED. `qbDependencies.cmake:105-117` checks that `src/qb/ev` exists and sets `QB_HAS_LIBEV`; if it is missing, configuration fails with a fatal error. The tree is compiled by `add_subdirectory("${QB_VENDOR_DIR}/qev")` (`qb/CMakeLists.txt:106`), producing the static `qev` target (`src/qb/ev/CMakeLists.txt:365`). Resolving libev defines `QB_HAS_LIBEV=1` on every qb target (`qbDependencies.cmake:542-544`).
  The fork's generated configuration header is reached through `-DEV_CONFIG_H=<qb/ev/ev_config.h>`, a `PUBLIC` compile definition on the `qev` target, because `ev.h`'s own fallback lookup for `ev_config.h` is `__has_include`-guarded and would fail *silently* — flipping `EV_MULTIPLICITY` from 1 to 4 and desynchronising every `ev_*` prototype from the compiled library. `qb/io/async/event/base.h` and `qb/io/async/coroutine/scheduler.h` carry an `#error` guard on `EV_MULTIPLICITY` so that miss is a compile error rather than a runtime mystery.
- **stduuid** — REQUIRED in practice. `qbDependencies.cmake:44-47` detects `src/qb/vendor/uuid` and sets `QB_HAS_UUID`. It is added by `add_subdirectory("${QB_VENDOR_DIR}/uuid")` (`qb/CMakeLists.txt:105`), which declares the header-only `stduuid` `INTERFACE` target (`src/qb/vendor/uuid/CMakeLists.txt:22`). The framework pins its options before adding it (`qb/CMakeLists.txt:83-91`): `UUID_BUILD_TESTS`, `UUID_SYSTEM_GENERATOR` and `UUID_TIME_GENERATOR` are forced **off**, while `UUID_USING_CXX20_SPAN` is forced **on**. That last one is load-bearing, not cosmetic: qb requires C++20 so `std::span` always exists, and with it off stduuid takes a `gsl` fallback branch whose directory was deleted in the C++20 migration -- which made `cmake --install` fail outright. Only when the vendored directory is absent does `qbDependencies.cmake:50-95` fall back to a system UUID (pkg-config `uuid`, then `find_path`/`find_library`); if neither is found, the build emits a warning and clears `QB_HAS_UUID` rather than failing.
- **nanolog, ska_hash** — header-only, no CMake target at all. They are ordinary files under `src/qb/vendor/`, reached through qb's single include root like any other qb header. `src/qb/io/io.cpp` compiles `nanolog.cpp` by textual inclusion.

Both compiled forks are part of the install export: `qev` and `stduuid` are added to the `qbTargets` export set so their names are rewritten under the `qb::` namespace in the transitive link list of `qb::io`/`qb::core` (`qb/CMakeLists.txt:322-339`). Their headers need no install rule of their own — qb's ordinary public-header rule (`qb/cmake/qbPackage.cmake:157-166`) already covers them, which is precisely why the two trees cannot diverge. When embedded, each fork's own standalone install/package-config block is skipped, so an installed qb ships no `lib/cmake/qev/` (`src/qb/ev/CMakeLists.txt:518-524`) or `lib/cmake/stduuid/` alongside its own package.

#### Using qb alongside a system libev or libevent

The `qev` fork keeps libev's `ev_*` symbol names **deliberately** — the C API is not reformed,
only namespaced by path — and moves its headers under `qb/ev/`. One overlap with an upstream
libev therefore remains on purpose; a second one used to exist and has been closed.

**Include guards — no longer collide.** Every header of the fork carries a guard named after the
fork rather than after upstream: `QB_EV_H_` (`src/qb/ev/ev.h`), `QB_EVPP_H_` (`ev++.h`),
`QB_EV_EVENT_H_` (`event.h`), `QB_EV_EVENT_COMPAT_H_` (`event_compat.h`), `QB_EV_WRAP_H`
(`ev_wrap.h`), `QB_EV_WEPOLL_H_` (`wepoll.h`) and `QB_EV_GENERATED_CONFIG_H_` (the generated
`ev_config.h`). A single
translation unit may include both `<qb/ev/ev.h>` and a system `<ev.h>`, in either order,
and get both sets of declarations.

> Previously these guards kept upstream's spellings (`EV_H_`, `EVPP_H__`, `EVENT_H_`, …), so
> whichever header came second in a translation unit was swallowed by the other's guard and its
> declarations were silently absent. Because `<qb/main.h>` pulls `ev.h` in transitively
> (`core/Actor.h` → `io/async/coroutine.h` → `coroutine/scheduler.h` → `ev++.h`), any consumer
> that also used a real libev hit it — as a compile error (`ev_default_loop` undeclared with qb
> first, errors inside qb's own `ev++.h` with libev first), never as silent misbehaviour. The
> only workaround was to keep the two APIs in separate `.cpp` files. That is no longer necessary.

**The 24 `event_*` symbols no longer ship.** They used to, and the collision was real: `libqev.a`
carried libev's libevent-compatibility layer (`event_init`, `event_add`, `event_base_loop`,
`event_del`, `event_dispatch`, …) under libevent's own unprefixed names, on every consumer's link
line — `qb::io` names `qb::ev` in its `INTERFACE_LINK_LIBRARIES`. A consumer that also linked the
real libevent got whichever implementation the archive order picked, with two unrelated
`struct event_base` layouts and no diagnostic. Measured against a stand-in: `-lfakeevent` first ran
real libevent, `libqev.a` first ran qb's fork, `rc=0`, silently.

Those names are not a fork artefact — they *are* libevent's published API, and any libev built with
its compat layer exports them too, so renaming them would destroy the one thing the layer exists to
provide. The fix was therefore to stop building it rather than to rename it: **`QB_EV_LIBEVENT_COMPAT`
defaults to `OFF`**, `event.c` is left out of the archive, and `event.h`/`event_compat.h` are excluded
from the install. Nothing in qb calls any of them (`nm -u libqb-io.a | grep -c '^ *_event_'` → 0), so
the default costs qb nothing and makes the collision impossible rather than merely unlikely. Confirm
on any build with `nm -g <prefix>/lib/libqev.a | grep -c '_event_'` → `0`.

Turn the option `ON` only if you need libevent's spelling from qb's fork — and then the paragraph
above applies again: link qb's fork or a real libevent, not both. Everything qb itself uses goes
through the renamed `ev_*` surface and is unaffected either way.
`QB_EV_LIBEVENT_COMPAT` is declared at `qb/src/qb/ev/CMakeLists.txt:97-99`.

Note that `qb/readme/` is **not** installed, so for a `find_package(qb)` consumer the authoritative
copy of both caveats is the comment block at the top of the installed header itself
(`<prefix>/include/qb/ev/ev.h`). Keep the two in sync.

### Third-party: nlohmann/json

nlohmann is the one genuine upstream dependency, and it is handled the opposite way. `nlohmann::json` crosses qb's API boundary (`qb::json` is an alias for it, and `qb/json.h` defines `to_json`/`from_json` for `qb::uuid`), so a consumer compiling against *their* copy while qb was compiled against a private one is an ODR violation on the type — something no include-path rename can fix. `qbDependencies.cmake:365` therefore does `find_package(nlohmann_json 3.11 QUIET)` first, and falls back to `FetchContent` at the pinned `QB_NLOHMANN_GIT_TAG` (`qbDependencies.cmake:489`). Either way the result is the `qb-nlohmann` `INTERFACE` target (`qbDependencies.cmake:384-385`, exported as `qb::nlohmann`), linked `PUBLIC` by `qb-io`.

**qb does not vendor nlohmann.** It did until 3.0 — `modules/nlohmann/json.hpp`, an untagged post-3.12.0 snapshot that nonetheless declared `NLOHMANN_JSON_VERSION_* = 3/12/0`. Because nlohmann encodes the version in an inline namespace (`nlohmann::json_abi_v3_12_0`), that copy presented the *same* namespace tag as a genuine 3.12.0 over a *different* set of definitions, so a program linking both got one namespace spanning two definition sets with no linker diagnostic. It is deleted; see the qb [CHANGELOG](../../CHANGELOG.md) for the migration.

Consequences worth knowing:

- A consumer must have nlohmann/json. `qbConfig.cmake` calls `find_dependency(nlohmann_json 3.11)` **unconditionally**, and qb installs no copy of its own — a consumer without the package fails at configure time, loudly.
- `<prefix>/include` is exactly `qb` on every host (`qb qbm` for the workspace package). It no longer gains an `nlohmann/` directory depending on what the *build* machine had installed.
- An **installable** qb requires a real system nlohmann. A fetched one belongs to no export set, so `install(EXPORT qbTargets)` cannot name it, and installing its headers would restore the `nlohmann/` include-root entry; `QB_INSTALL` with no system copy is therefore a configure-time error naming both ways out. Builds that only compile and test are unaffected — they fetch.
- nlohmann encodes its version and a few options in an inline namespace (`json_abi_v3_12_0`), so a version mismatch surfaces as a link error. `JSON_NOEXCEPTION` and `JSON_USE_IMPLICIT_CONVERSIONS` change the class but not that tag, so they can still mismatch silently. Building qb in your own tree (`add_subdirectory` / `FetchContent`) is the only configuration that closes this completely.

### Fetched dependencies

Fetched dependencies build cleanly from source with CMake, so qb can resolve them system-first and fall back to a pinned-tag source build. Three dependencies are fetchable: **GoogleTest**, **Google Benchmark**, and **zlib**.

- **GoogleTest** — resolved in `qbFetchGoogleDeps.cmake:53-128`, only when `QB_BUILD_TESTS` is ON (the default). Cache options are forced before the fetch: `BUILD_GMOCK=ON`, `INSTALL_GTEST=OFF`, and the gtest/gmock self-tests off (`qbFetchGoogleDeps.cmake:61-64`). On MSVC, `gtest_force_shared_crt=ON` (`qbFetchGoogleDeps.cmake:58-60`). When built from source under Clang/AppleClang, qb adds `-Wno-character-conversion` to the `gtest` target to silence a third-party `char8_t` warning (`qbFetchGoogleDeps.cmake:120-123`).
- **Google Benchmark** — resolved in `qbFetchGoogleDeps.cmake:133-168`, only when `QB_BUILD_BENCHMARKS` is ON (off by default). Cache options forced before the fetch: `BENCHMARK_ENABLE_TESTING=OFF`, `BENCHMARK_DOWNLOAD_DEPENDENCIES=OFF` (`qbFetchGoogleDeps.cmake:138-139`).
- **zlib** — resolved in `qbDependencies.cmake:155-220`, only when `QB_WITH_COMPRESSION` is ON (the default). zlib is searched with `find_package(ZLIB QUIET)` first; if absent and `QB_DEPS_FETCH_FALLBACK` is ON, it is built from `madler/zlib` at `QB_ZLIB_GIT_TAG`. Because `madler/zlib` exposes `zlib`/`zlibstatic` but no `ZLIB::ZLIB` target, qb normalizes an `ZLIB::ZLIB` alias (`qbDependencies.cmake:172-180`). Resolving zlib defines `QB_HAS_COMPRESSION=1`; if it is requested but cannot be found or built, the build warns and forces `QB_WITH_COMPRESSION` off (`qbDependencies.cmake:217-219`).

> Note: GoogleTest and Google Benchmark integrate `find_package` into the fetch through `FIND_PACKAGE_ARGS` (`qbFetchGoogleDeps.cmake:96-107,146-158`), so `QB_DEPS_FETCH_FALLBACK=OFF` makes them ignore the system entirely and always build from the pinned tag. zlib uses an explicit `find_package`-then-`FetchContent` sequence, so for zlib, `QB_DEPS_FETCH_FALLBACK=OFF` means "use the system package if present, otherwise disable compression" — it never reaches the source build. See [Resolution policy](#resolution-policy-qb_deps_fetch_fallback) for the exact behavior.

### System-only dependencies

System-only dependencies have no clean CMake source build, so qb never fetches them. Each is located with `find_package` and gates an optional feature; when absent, the feature is disabled rather than the build failing.

- **OpenSSL** — `find_package(OpenSSL QUIET)`, only when `QB_WITH_SSL` is ON (the default), at `qbDependencies.cmake:125-151`. On success it links `OpenSSL::SSL` and `OpenSSL::Crypto` and sets `QB_HAS_SSL`. If absent, it warns, clears `QB_HAS_SSL`, and forces `QB_WITH_SSL` off (`qbDependencies.cmake:143-147`). `QB_HAS_SSL=1` is defined on qb targets when present.
- **Argon2** — `find_package(Argon2 QUIET)`, searched **only inside the OpenSSL-found branch** (`qbDependencies.cmake:133-142`). A build without OpenSSL can therefore never have `QB_HAS_ARGON2`. The bundled `cmake/FindArgon2.cmake` creates the `Argon2::Argon2` imported target (`FindArgon2.cmake:135-139`); on success qb links it and defines `QB_HAS_ARGON2=1`. If Argon2 is missing, qb falls back to its non-Argon2 crypto paths (`qbDependencies.cmake:140`).
- **libngtcp2 (+ ngtcp2_crypto_ossl)** — the QUIC transport stack, governed by the tri-state `QB_WITH_QUIC` (`qbDependencies.cmake:226-264`). QUIC **requires SSL**: if `QB_HAS_SSL` is false, QUIC is disabled regardless of the request. When SSL is present, `find_package(Ngtcp2 QUIET)` (bundled `cmake/FindNgtcp2.cmake`) creates the `Ngtcp2::ngtcp2` and `Ngtcp2::crypto_ossl` imported targets (`FindNgtcp2.cmake:79-90`); on success qb links both and defines `QB_HAS_QUIC=1`. Distro packages may also provide `libngtcp2-crypto-gnutls-dev`, but that is not a drop-in replacement for qb's current native backend because `src/qb/io/quic.cpp` calls the ngtcp2 OpenSSL helper APIs. See [QUIC tri-state](#quic-tri-state-qb_with_quic) for the AUTO/ON/OFF semantics.
- **gperftools** — `find_package(Gperftools QUIET)`, only when `QB_WITH_PROFILING` is ON (off by default), at `qbDependencies.cmake:275-296`. The bundled `cmake/FindGperftools.cmake` creates `Gperftools::Profiler` and `Gperftools::TCMalloc` (among other targets); qb links whichever exist and sets `QB_HAS_PROFILING`. If absent, it warns and forces `QB_WITH_PROFILING` off.

The bundled find-modules for Argon2 and ngtcp2 are installed alongside the package config so that `find_package(qb)` consumers of an Argon2- or QUIC-enabled build can recreate the same imported targets (`qb/CMakeLists.txt:348-357`).

## QB_HAS_* versus QB_WITH_*

The two prefixes are not interchangeable.

- `QB_WITH_*` are **user-facing requests** — options you set on the command line (`QB_WITH_SSL`, `QB_WITH_COMPRESSION`, `QB_WITH_QUIC`, `QB_WITH_PROFILING`).
- `QB_HAS_*` are **resolved results** — what qb actually found after probing (`QB_HAS_SSL`, `QB_HAS_COMPRESSION`, `QB_HAS_QUIC`, `QB_HAS_ARGON2`, `QB_HAS_PROFILING`, `QB_HAS_UUID`, `QB_HAS_LIBEV`).

A `QB_HAS_*` flag drives the corresponding `QB_HAS_*=1` compile definition on qb targets (`qbDependencies.cmake:526-544`). When a requested feature's dependency is missing, qb forces the `QB_WITH_*` option back off so the recorded request matches reality.

## Resolution policy: QB_DEPS_FETCH_FALLBACK

`QB_DEPS_FETCH_FALLBACK` (default **ON**, `qbConfig.cmake:111`) selects how the fetchable dependencies behave. The fetched-via-`FIND_PACKAGE_ARGS` dependencies (GoogleTest, Google Benchmark) and zlib differ at the OFF setting.

| Setting | GoogleTest / Google Benchmark | zlib |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK=ON` (default) | Use the system package if `find_package` locates it; otherwise build the pinned tag from source. | Same: system if present, else build the pinned tag from source. |
| `QB_DEPS_FETCH_FALLBACK=OFF` | **Always build the pinned tag from source** — the system package is ignored (no `FIND_PACKAGE_ARGS`). | Use the system package if present; otherwise compression is disabled (no source build is attempted). |
| `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` | Force `find_package(... CONFIG REQUIRED)` — require a system package, never fetch; configuration fails if missing. | (not applicable) |

The mechanism: for GoogleTest and Google Benchmark, qb appends `FIND_PACKAGE_ARGS QUIET GLOBAL NAMES <pkg>` to the `FetchContent_Declare` **only when `QB_DEPS_FETCH_FALLBACK` is ON** (`qbFetchGoogleDeps.cmake:96-99,146-149`). With the argument present, `FetchContent_MakeAvailable` tries `find_package` first and falls back to the source build; without it, it always builds from source. The `QB_USE_SYSTEM_*` switches short-circuit this entirely with an explicit `find_package(... CONFIG REQUIRED)` (`qbFetchGoogleDeps.cmake:54-56,134-136`).

`QUIET` and `GLOBAL` are part of that argument list deliberately. CMake adds both by itself when they are absent — `QUIET` always, `GLOBAL` because qb sets `CMAKE_FIND_PACKAGE_TARGETS_GLOBAL` (`qbFetchGoogleDeps.cmake:48`) so that the imported targets stay visible inside the `qbm/*` subdirectories — but CMake 3.24.0 through 3.28.x splice them in with `list()` rather than `string()`, which joins them to the preceding bracket-quoted token with a `;` instead of a space and makes CMake fail to parse its own generated code:

```text
CMake Error at .../Modules/FetchContent.cmake:1202:EVAL:1:
  Syntax Error in cmake code at column 107
  Argument not separated from preceding token by whitespace.
```

Passing the two keywords explicitly stops CMake from adding them, so the string stays well-formed and the resulting `find_package()` call is identical. CMake 3.29.0 fixed the underlying bug (`string(PREPEND/APPEND)` instead of `list(INSERT/APPEND)`); the explicit keywords are what keep 3.24–3.28 — Ubuntu 24.04 LTS's stock 3.28.3, Debian 12's 3.25.1, RHEL 9's 3.26.5 — inside the supported range.

System-only dependencies (OpenSSL, Argon2, libngtcp2, gperftools) are unaffected by `QB_DEPS_FETCH_FALLBACK`: they are never fetched under any setting.

## Pinning fetched versions

Four advanced cache variables pin the Git tag (or SHA) used for source builds (`qbConfig.cmake:116-121`; `mark_as_advanced`):

| Variable | Default | Applies to |
|---|---|---|
| `QB_GOOGLETEST_GIT_TAG` | `v1.15.2` | GoogleTest source build |
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.2` | Google Benchmark source build |
| `QB_ZLIB_GIT_TAG` | `v1.3.1` | zlib fallback source build |

Override at configure time:

```bash
# src: derived from qb/cmake/qbConfig.cmake:116-121 + qbFetchGoogleDeps.cmake:101-107
cmake -B build -S . -DQB_GOOGLETEST_GIT_TAG=v1.14.0
```

## Forcing system packages

The default already prefers a system package when present. To **require** a system GoogleTest/Benchmark and fail (never fetch) when it is absent:

```bash
# src: derived from qb/cmake/qbFetchGoogleDeps.cmake:54-56,134-136
cmake -B build -S . \
  -DQB_USE_SYSTEM_GTEST=ON \
  -DQB_USE_SYSTEM_BENCHMARK=ON
```

This issues `find_package(GTest CONFIG REQUIRED)` and `find_package(benchmark CONFIG REQUIRED)`, resolving packages that ship CMake config files (vcpkg, Conan, distribution `-dev` packages).

## QUIC tri-state: QB_WITH_QUIC

`QB_WITH_QUIC` is a three-valued cache string with default **AUTO** (`qbConfig.cmake:164`), resolved at `qbDependencies.cmake:230-237`:

The value is matched case-insensitively (`qbDependencies.cmake:230`). The OFF set is matched explicitly; AUTO is matched explicitly; any other value is treated as a required ON.

| Value | Behavior when libngtcp2 is missing |
|---|---|
| `AUTO` (default) | Enable QUIC if libngtcp2 is found; stay silent (no warning) when absent. |
| Any value not in the OFF set and not `AUTO` (for example `ON`, `TRUE`, `1`, `YES`, `Y`) | Require libngtcp2; warn and disable QUIC if it (or SSL) is missing. |
| `OFF`, `FALSE`, `0`, `NO`, or `N` | Disabled outright; no search performed (`qbDependencies.cmake:231-232`). |

In every case, QUIC additionally requires `QB_HAS_SSL` — without OpenSSL, QUIC is disabled regardless of `QB_WITH_QUIC` (and a required ON warns about it). AUTO mirrors how SSL and compression silently auto-detect.

## Source layout of fetched dependencies

When a fetchable dependency is built from source, `FetchContent` places its tree under the build directory, for example `build/_deps/googletest-src`, `build/_deps/googlebenchmark-src`, and `build/_deps/zlib-src` (the exact path follows the `FetchContent` naming convention). A system package, by contrast, leaves no `_deps` entry — the per-dependency status message states which path was taken (`qbFetchGoogleDeps.cmake:110-114,160-164`).

## Offline and CI builds

- A source fallback needs **network access to GitHub** on first configure (`GIT_REPOSITORY` with `GIT_SHALLOW TRUE`), unless the sources are pre-populated or a system package is found. [Building](./building.md) notes that Git is required on the configure machine for this reason.
- For air-gapped or reproducible builds: provide the dependencies as system packages on `CMAKE_PREFIX_PATH` and set `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` (require system, never fetch), or pre-populate the `_deps` directory before configuring.
- Leaving `QB_DEPS_FETCH_FALLBACK=ON` with no system packages present is the most convenient default for a fresh machine, but it makes the first configure depend on GitHub reachability.

## Pitfalls

- **`QB_DEPS_FETCH_FALLBACK=OFF` does not mean "never fetch."** For GoogleTest and Google Benchmark it means the opposite — *always* build from source, ignoring the system. To require a system package and never fetch, use `QB_USE_SYSTEM_GTEST` / `QB_USE_SYSTEM_BENCHMARK` instead.
- **Argon2 is invisible without OpenSSL.** Its `find_package` is nested inside the OpenSSL-found branch, so a non-SSL build silently has `QB_HAS_ARGON2=OFF` even if libargon2 is installed.
- **QUIC silently disables without SSL.** With `QB_WITH_QUIC=AUTO` and no OpenSSL, QUIC is off with no warning. Use `QB_WITH_QUIC=ON` to make the missing prerequisite surface as a warning.
- **A missing optional dependency does not fail the build.** SSL, compression, QUIC, and profiling each warn and force their `QB_WITH_*` option off when their dependency is absent. Only **qev** (the in-tree libev fork, `qbDependencies.cmake:114`) and the **Threads** package (`find_package(Threads REQUIRED)`, `qbCompiler.cmake:546-549`) are hard-required; their absence is fatal.
- **Stale googletest/googlebenchmark submodule folders.** The framework no longer vendors these as Git submodules. Older clones may retain `modules/googletest`/`modules/googlebenchmark` directories; CMake ignores them, and you can delete the folders and any stale `.git/config` submodule entries.

## See also

- [Building the QB Actor Framework](./building.md) — full configure/build walkthrough and the complete option matrix.
- [Testing](./testing.md) — how `QB_BUILD_TESTS` and the resolved GoogleTest drive `qb_add_test` / ctest.
- [FAQ](./faq.md) — common configure-time questions.
- [CMake FetchContent module](https://cmake.org/cmake/help/latest/module/FetchContent.html) — upstream reference for `FIND_PACKAGE_ARGS` and `MakeAvailable`.
