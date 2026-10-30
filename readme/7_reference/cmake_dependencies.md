<!-- Verified-against: qb 3.3.0 (C++20 default, C++23 supported) -->
# CMake and third-party dependencies

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.3.0 (C++20 default, C++23 supported)

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
| zstd | system only | opt-in (`QB_WITH_ZSTD`, 3.3) | the zstd codec | `FindZstd.cmake` (installed with qb) |
| brotli | system only | opt-in (`QB_WITH_BROTLI`, 3.3) | the brotli codec | `FindBrotli.cmake` (installed with qb) |
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

**The two builds are the same source and different products, and they never share a path.** `qev` standalone is the full library — all fourteen watcher families, `libqev.a`, `<qev/ev.h>`, `find_package(qev)`. qb embeds a **reduced** profile of it: the six families qb-io never uses are compiled out (`QB_EV_WATCHERS_FULL=OFF`; `async` stays on since 3.2, because a core that owns io watchers parks inside its loop and a producer ends that park with `ev_async_send`), which is 13 fewer exported symbols and a `struct ev_loop` 96 bytes smaller — measured on Linux/x86-64, 61 `ev_*` against 74 and 496 bytes against 592 — so qb ships `libqb-ev.a` under `<qb/ev/ev.h>`, reached as `qb::ev`. Same-named files with different content is exactly the collision this fork exists to close, so the two install sets are disjoint by construction and a matrix cell (`shared-prefix-install`) proves it on every CI run.

The C **API** is libev's, unchanged — `ev_run`, `ev_io_start`, `struct ev_loop`. Porting a libev program is one include-path edit. What is ours is everything that reaches a filesystem, so an installed qev never overwrites an installed libev. Embedded in qb, the link names are ours too: every function and variable the archive exports links as `qb_` + its libev name — `qb_ev_run` in `libqb-ev.a` — through one `#define` per name in the generated `ev_config.h`, which every translation unit reads before `ev.h` declares anything, so source still writes `ev_run`. A standalone qev keeps libev's own link names.

That path is not cosmetic. Their headers used to be published by bare name, so an installed qb dropped `ev.h`, `ev++.h`, `event.h`, `event_compat.h`, `ev_config.h`, `uuid.h` and the directories `ev/`, `uuid/`, `nanolog/`, `ska_hash/` straight into the consumer's include root — 12 top-level names, every one of them able to shadow, or be shadowed by, a header the consumer already owned. Living under `qb/vendor/` makes that collision structurally impossible. Being physically inside the include root (`src/`) also means one include root serves the build tree and the installed tree, with no separate `BUILD_INTERFACE`/`INSTALL_INTERFACE` pair to drift apart.

- **libev** — REQUIRED. `qbDependencies.cmake:105-117` checks that `src/qb/ev` exists and sets `QB_HAS_LIBEV`; if it is missing, configuration fails with a fatal error. The tree is compiled by `add_subdirectory("${QB_VENDOR_DIR}/qev")` (`qb/CMakeLists.txt:100`), producing the static `qev` target (`src/qb/ev/CMakeLists.txt:425`). Resolving libev defines `QB_HAS_LIBEV=1` on every qb target (`qbDependencies.cmake:590-592`).
  The fork's generated configuration header is reached through `-DEV_CONFIG_H=<qb/ev/ev_config.h>`, a `PUBLIC` compile definition on the `qev` target, because `ev.h`'s own fallback lookup for `ev_config.h` is `__has_include`-guarded and would fail *silently* — flipping `EV_MULTIPLICITY` from 1 to 4 and desynchronising every `ev_*` prototype from the compiled library. `qb/io/async/event/base.h` and `qb/io/async/coroutine/scheduler.h` carry an `#error` guard on `EV_MULTIPLICITY` so that miss is a compile error rather than a runtime mystery.
- **stduuid** — REQUIRED in practice. `qbDependencies.cmake:44-47` detects `src/qb/vendor/uuid` and sets `QB_HAS_UUID`. It is added by `add_subdirectory("${QB_VENDOR_DIR}/uuid")` (`qb/CMakeLists.txt:99`), which declares the header-only `stduuid` `INTERFACE` target (`src/qb/vendor/uuid/CMakeLists.txt:24`). The framework pins the fork's two options before adding it (`qb/CMakeLists.txt:84-85`): `UUID_SYSTEM_GENERATOR` and `UUID_TIME_GENERATOR` are forced **off**. The fork has no other switch: it always uses `std::span` -- its `gsl` fallback, whose directory the C++20 migration deleted and which made a standalone `cmake --install` of the fork fail, is gone -- and it carries no test suite (Huly QB-375, QB-378). Only when the vendored directory is absent does `qbDependencies.cmake:50-95` fall back to a system UUID (pkg-config `uuid`, then `find_path`/`find_library`); if neither is found, the build emits a warning and clears `QB_HAS_UUID` rather than failing.
- **nanolog, ska_hash** — header-only, no CMake target at all. They are ordinary files under `src/qb/vendor/`, reached through qb's single include root like any other qb header. `src/qb/io/io.cpp` compiles `nanolog.cpp` by textual inclusion.

Both compiled forks are part of the install export: `qev` and `stduuid` are added to the `qbTargets` export set so their names are rewritten under the `qb::` namespace in the transitive link list of `qb::io`/`qb::core` (`qb/CMakeLists.txt:327-344`). Their headers need no install rule of their own — qb's ordinary public-header rule (`qb/cmake/qbPackage.cmake:157-166`) already covers them, which is precisely why the two trees cannot diverge. When embedded, each fork's own standalone install/package-config block is skipped, so an installed qb ships no `lib/cmake/qev/` (`src/qb/ev/CMakeLists.txt:623-629`) or `lib/cmake/stduuid/` alongside its own package.

#### Using qb alongside a system libev or libevent

The `qev` fork keeps libev's `ev_*` **spelling** deliberately — the C API is not reformed — and
namespaces everything else: its headers under `qb/ev/`, its include guards and, embedded in qb, its
link names. Three overlaps with an upstream libev used to exist; all three are closed.

**Link names — no longer shared (Huly QB-1139).** qb's copy used to export libev's own names, so a
program that also linked a real libev, or a library that bundles one, held two definitions of
`ev_run`, `ev_loop_new` and every other exported name, and the link line chose between them.
Measured on ld64 against a stand-in: the foreign archive first on the line made qb's loop call the
foreign `ev_loop_new` ("libev event loop cannot be initialized"), behind a `duplicate symbol` warning
and a linker exit of 0; GNU ld and MSVC refuse the second definition. A real libev's `struct ev_loop`
is not qev's, so where such a link went through, the first `ev_run` corrupted memory. The embedded
copy now links as `qb_` + libev's name (above), and the rule for a program that uses both follows:
a translation unit that includes qb speaks qb's loop — there `ev_run` names `qb_ev_run` — and one that
includes nothing of qb's reaches the other libev. `qb-io-test-unit-embedded-loop-prefix` links such a
stand-in before qb and checks both sides; `qb-io-test-unit-embedded-loop-symbols` reads the built
archive and fails on any exported `ev_*` left unrenamed.

**Include guards — no longer collide.** Every header of the fork carries a guard named after the
fork rather than after upstream: `QB_EV_H_` (`src/qb/ev/ev.h`), `QB_EVPP_H_` (`ev++.h`),
`QB_EV_EVENT_H_` (`event.h`), `QB_EV_EVENT_COMPAT_H_` (`event_compat.h`), `QB_EV_WRAP_H`
(`ev_wrap.h`), `QB_EV_WEPOLL_H_` (`wepoll.h`) and `QB_EV_GENERATED_CONFIG_H_` (the generated
`ev_config.h`), so neither header's guard swallows the other's in a translation unit that includes
both. Including both buys nothing since the link names were separated, though: the renaming
`#define`s apply to everything that follows them, so in that unit every `ev_*` name is qb's. Drive
the other libev from a translation unit that includes nothing of qb's.

> Previously these guards kept upstream's spellings (`EV_H_`, `EVPP_H__`, `EVENT_H_`, …), so
> whichever header came second in a translation unit was swallowed by the other's guard and its
> declarations were silently absent. Because `<qb/main.h>` pulls `ev.h` in transitively
> (`core/Actor.h` → `io/async/coroutine.h` → `coroutine/scheduler.h` → `ev++.h`), any consumer
> that also used a real libev hit it — as a compile error (`ev_default_loop` undeclared with qb
> first, errors inside qb's own `ev++.h` with libev first), never as silent misbehaviour. The
> only workaround was to keep the two APIs in separate `.cpp` files. Separate files are again the
> rule, and now they are also correct at run time: before the link names were separated, even two
> separate files shared one `ev_run` at link time.

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
`QB_EV_LIBEVENT_COMPAT` is declared at `qb/src/qb/ev/CMakeLists.txt:93-95`.

Note that `qb/readme/` is **not** installed, so for a `find_package(qb)` consumer the authoritative
copy of both caveats is the comment block at the top of the installed header itself
(`<prefix>/include/qb/ev/ev.h`). Keep the two in sync.

### Third-party: nlohmann/json

nlohmann is the one genuine upstream dependency, and it is handled the opposite way. `nlohmann::json` crosses qb's API boundary (`qb::json` is an alias for it, and `qb/json.h` defines `to_json`/`from_json` for `qb::uuid`), so a consumer compiling against *their* copy while qb was compiled against a private one is an ODR violation on the type — something no include-path rename can fix. `qbDependencies.cmake:405` therefore does `find_package(nlohmann_json 3.11 QUIET)` first, and falls back to `FetchContent` at the pinned `QB_NLOHMANN_GIT_TAG` (`qbDependencies.cmake:529`). Either way the result is the `qb-nlohmann` `INTERFACE` target (`qbDependencies.cmake:424-425`, exported as `qb::nlohmann`), linked `PUBLIC` by `qb-io`.

**qb does not vendor nlohmann.** It did until 3.0 — `modules/nlohmann/json.hpp`, an untagged post-3.12.0 snapshot that nonetheless declared `NLOHMANN_JSON_VERSION_* = 3/12/0`. Because nlohmann encodes the version in an inline namespace (`nlohmann::json_abi_v3_12_0`), that copy presented the *same* namespace tag as a genuine 3.12.0 over a *different* set of definitions, so a program linking both got one namespace spanning two definition sets with no linker diagnostic. It is deleted; see the qb [CHANGELOG](../../CHANGELOG.md) for the migration.

Consequences worth knowing:

- A consumer must have nlohmann/json. `qbConfig.cmake` calls `find_dependency(nlohmann_json 3.11)` **unconditionally**, and qb installs no copy of its own — a consumer without the package fails at configure time, loudly.
- `<prefix>/include` is exactly `qb` on every host (`qb qbm` for the workspace package). It no longer gains an `nlohmann/` directory depending on what the *build* machine had installed.
- An **installable** qb requires a real system nlohmann. A fetched one belongs to no export set, so `install(EXPORT qbTargets)` cannot name it, and installing its headers would restore the `nlohmann/` include-root entry; `QB_INSTALL` with no system copy is therefore a configure-time error naming both ways out. Builds that only compile and test are unaffected — they fetch.
- nlohmann encodes its version and a few options in an inline namespace (`json_abi_v3_12_0`), so a version mismatch surfaces as a link error. `JSON_NOEXCEPTION` and `JSON_USE_IMPLICIT_CONVERSIONS` change the class but not that tag, so they can still mismatch silently. Building qb in your own tree (`add_subdirectory` / `FetchContent`) is the only configuration that closes this completely.

### Fetched dependencies

Fetched dependencies build cleanly from source with CMake, so qb can resolve them system-first and fall back to a pinned-tag source build. Three dependencies are fetchable: **GoogleTest**, **Google Benchmark**, and **zlib**.

- **GoogleTest** — resolved in `qbFetchGoogleDeps.cmake:62-147` when tests **or benchmarks** are built: benchmark fixtures also need GoogleTest. Under `QB_SANITIZE` the system package is not a candidate unless explicitly forced: a GoogleTest compiled without the sanitizer disagrees with instrumented TUs on libc++'s container annotations, so the pinned tag is built with the same flags as every other target (`qbFetchGoogleDeps.cmake:53-61,109-112,198-206`); `QB_USE_SYSTEM_GTEST=ON` still forces the system package, with a warning saying what that buys (`qbFetchGoogleDeps.cmake:65-67`). Cache options are forced before the fetch: `BUILD_GMOCK=ON`, `INSTALL_GTEST=OFF`, and the gtest/gmock self-tests off (`qbFetchGoogleDeps.cmake:73-76`). On MSVC, `gtest_force_shared_crt=ON` (`qbFetchGoogleDeps.cmake:70-72`). When built from source under Clang/AppleClang, qb adds `-Wno-character-conversion` to the `gtest` target to silence a third-party `char8_t` warning (`qbFetchGoogleDeps.cmake:137-142`).
- **Google Benchmark** — resolved in `qbFetchGoogleDeps.cmake:152-194`, only when `QB_BUILD_BENCHMARKS` is ON (off by default). Cache options forced before the fetch: `BENCHMARK_ENABLE_TESTING=OFF`, `BENCHMARK_DOWNLOAD_DEPENDENCIES=OFF` (`qbFetchGoogleDeps.cmake:162-163`).
- **zlib** — resolved in `qbDependencies.cmake:169-234`, only when `QB_WITH_COMPRESSION` is ON (the default). zlib is searched with `find_package(ZLIB QUIET)` (through `_qb_find_zlib()`) first; if absent and `QB_DEPS_FETCH_FALLBACK` is ON, it is built from `madler/zlib` at `QB_ZLIB_GIT_TAG`. Because `madler/zlib` exposes `zlib`/`zlibstatic` but no `ZLIB::ZLIB` target, qb normalizes an `ZLIB::ZLIB` alias (`qbDependencies.cmake:186-194`). Resolving zlib defines `QB_HAS_COMPRESSION=1`; if it is requested but cannot be found or built, the build warns and forces `QB_WITH_COMPRESSION` off (`qbDependencies.cmake:231-233`) -- unless this build directory had found one that neither a build from source nor a parent project replaces: lost, it stops the configure (below).

> Note: GoogleTest and Google Benchmark integrate `find_package` into the fetch through `FIND_PACKAGE_ARGS` (`qbFetchGoogleDeps.cmake:108-121,170-184`), so `QB_DEPS_FETCH_FALLBACK=OFF` makes them ignore the system entirely and always build from the pinned tag. zlib uses an explicit `find_package`-then-`FetchContent` sequence, so for zlib, `QB_DEPS_FETCH_FALLBACK=OFF` means "use the system package if present, otherwise disable compression" — it never reaches the source build. See [Resolution policy](#resolution-policy-qb_deps_fetch_fallback) for the exact behavior.

### System-only dependencies

System-only dependencies have no clean CMake source build, so qb never fetches them. Each is located with `find_package` and gates an optional feature; when absent, the feature is disabled rather than the build failing.

- **OpenSSL** — `find_package(OpenSSL QUIET)`, only when `QB_WITH_SSL` is ON (the default), at `qbDependencies.cmake:130-165`. On success it links `OpenSSL::SSL` and `OpenSSL::Crypto` and sets `QB_HAS_SSL`. If absent, it warns, clears `QB_HAS_SSL`, and forces `QB_WITH_SSL` off (`qbDependencies.cmake:157-161`). `QB_HAS_SSL=1` is defined on qb targets when present. Before that, its headers, `OPENSSL_SSL_LIBRARY` and `OPENSSL_CRYPTO_LIBRARY` must be one major -- the number each library's file name carries (`libssl.so.3`, `libssl.4.dylib`, `libssl.so.1.1`), and the one the headers name their libraries with (`OPENSSL_SHLIB_VERSION`, `SHLIB_VERSION_NUMBER` on 1.1, else the release's major from `OPENSSL_VERSION`: a build may number its libraries apart, as quictls names 3.1's `libssl.so.81.3`) -- or the configure stops -- QUIC or not, whatever `QB_REQUIRE_FEATURES` says -- naming the three entries, their majors and the remedy: remove all three from the cache, or configure with `--fresh` (`_qb_check_openssl()`, `qbOpenSSL.cmake:185-269`). FindOpenSSL caches the three apart, and a cache cleared in part -- one entry removed, `OPENSSL_ROOT_DIR` moved to another OpenSSL -- configured and built a TLS that crashed (Huly QB-1190). The libraries are read as the QUIC check below reads libngtcp2's; a static archive, or a library no tool reads, takes its headers' major. Compared on Linux and macOS, where a library's file name carries that number -- not on other systems (FreeBSD names OpenSSL 3's `libssl.so.30`), not on Windows (nothing is read there), not for LibreSSL (its `libssl` and `libcrypto` are numbered apart) -- and the files read are inputs of the configure, so a package update is checked again at the next build. Its targets come first in qb-io's dependencies and `qb-nlohmann` last, and every qb test and benchmark links the found OpenSSL, then GoogleTest or Google Benchmark, then its dependencies: the found OpenSSL's include directory precedes a prefix-wide one that holds another installation's headers (`/opt/homebrew/include`), and a fetched GoogleTest's precedes that prefix too (Huly QB-1180; checked by `qb-io-test-unit-openssl-headers` and `qb-io-test-unit-openssl-link-order`). An OpenSSL in a directory the compiler searches by itself (a distribution's `/usr/include`) gets no flag and is read after every `-isystem` directory: no order can put it first there, and the headers' version check is what holds.
- **Argon2** — `find_package(Argon2 QUIET)` through `_qb_find_argon2()`, searched **only inside the OpenSSL-found branch** (`qbDependencies.cmake:147-156`). A build without OpenSSL can therefore never have `QB_HAS_ARGON2`. The bundled `cmake/FindArgon2.cmake` creates the `Argon2::Argon2` imported target (`FindArgon2.cmake:135-139`); on success qb links it and defines `QB_HAS_ARGON2=1`. If Argon2 is missing, qb falls back to its non-Argon2 crypto paths (`qbDependencies.cmake:154`) -- unless this build directory had found it: lost, it stops the configure (below).
- **libngtcp2 (+ ngtcp2_crypto_ossl)** — the QUIC transport stack, governed by the tri-state `QB_WITH_QUIC` (`qbDependencies.cmake:276-303`, the decision in `qbOpenSSL.cmake:416-540`). QUIC **requires SSL**: if `QB_HAS_SSL` is false, QUIC is disabled regardless of the request. When SSL is present, `find_package(Ngtcp2 QUIET)` (bundled `cmake/FindNgtcp2.cmake`) creates the `Ngtcp2::ngtcp2` and `Ngtcp2::crypto_ossl` imported targets (`FindNgtcp2.cmake:353-369`); the crypto helper target propagates ngtcp2 and OpenSSL dependencies so static archives link in order (`FindNgtcp2.cmake:361-368`); on success qb links both and defines `QB_HAS_QUIC=1` -- unless the helper links another OpenSSL major than qb's own (the module reports the helper's `libssl` as `Ngtcp2_OPENSSL_MAJOR` and `Ngtcp2_OPENSSL_LIBRARY` and its `libcrypto` as `Ngtcp2_OPENSSL_CRYPTO_MAJOR` and `Ngtcp2_OPENSSL_CRYPTO_LIBRARY`, from its dependency list -- on Mach-O the list of the file its install name names, the one dyld loads; `qbOpenSSL.cmake` reads qb's `libssl` and `libcrypto` the same way): two majors in one process crash at the first handshake, so QUIC is then disabled with a warning naming the libraries and, where the listing gives a path, the `OPENSSL_ROOT_DIR` to pass -- every `libssl` and `libcrypto` read must be one major, so a half-cleared cache that left qb's `libcrypto` on another major is refused too; two copies of one major (a static `libssl` in qb, or one major from two prefixes where Mach-O names them) keep QUIC with a warning (Huly QB-1179, QB-1185). The files read -- the libraries, the files they resolve to, on Mach-O their install names, and qb's `opensslv.h` -- are inputs of the configure (`CMAKE_CONFIGURE_DEPENDS`): an update that rewrites, moves or removes one re-runs the check at the next build, and a cached path the search itself had found whose file is gone -- `FindNgtcp2.cmake`'s entries, and the `OPENSSL_*` ones of qb's `find_package(OpenSSL)` (`_qb_find_openssl()`, `qbOpenSSL.cmake`), pkg-config asked again with them -- is searched again, and a library found nowhere else stops the configure, naming it (libngtcp2 where this build's QUIC linked it, or in a build directory QB-1185's code configured, which recorded nothing of it: one QUIC was off for, two OpenSSL majors, is no loss), until a search finds one, its feature is turned off (`QB_WITH_QUIC=OFF` for libngtcp2, `QB_WITH_SSL=OFF` for OpenSSL) or the build directory starts afresh; one set by hand or by a parent project, or found by a parent's search before qb's, is kept as given. qb's OpenSSL files are inputs with or without libngtcp2. Distro packages may also provide `libngtcp2-crypto-gnutls-dev`, but that is not a drop-in replacement for qb's current native backend because `src/qb/io/quic.cpp` calls the ngtcp2 OpenSSL helper APIs. See [QUIC tri-state](#quic-tri-state-qb_with_quic) for the AUTO/ON/OFF semantics.
- **gperftools** — `find_package(Gperftools QUIET COMPONENTS PROFILER)`, only when `QB_WITH_PROFILING` is ON (off by default), at `qbDependencies.cmake:314-336`. The bundled `cmake/FindGperftools.cmake` reports the package found only with the headers AND `libprofiler` or `tcmalloc`, and sets `Gperftools_<C>_FOUND` per component, so asking for `PROFILER` requires the CPU profiler itself (before 3.3 the headers alone counted, and qb reported profiling ON while linking nothing). It creates `Gperftools::Profiler` and `Gperftools::TCMalloc` (among other targets); qb links whichever exist and sets `QB_HAS_PROFILING`. If the profiler is absent, it warns and forces `QB_WITH_PROFILING` off.

A cached path any of these searches found -- OpenSSL's, Argon2's, zlib's, zstd's and brotli's through `_qb_find_again()` and its kin (`qbFoundLibraries.cmake:186-228`), libngtcp2's inside `FindNgtcp2.cmake` -- is recorded; when an update removes its file (a Homebrew cleanup removes the versioned Cellar keg the search cached), the path is searched again, pkg-config asked again with it, and a library this build directory had and finds nowhere else stops the configure, naming each entry and the switch that turns its feature off (Huly QB-1185, QB-1192): kept, the next build would stop for good on "missing and no known rule to make it", and a reconfigure would fail at generate. A package this build directory never found -- a stray header, a library without the rest -- stops nothing, libngtcp2 included, nor a zlib that qb builds from source instead (`QB_DEPS_FETCH_FALLBACK` in a build that installs nothing, `QB_INSTALL=OFF`) or a parent project provides, nor a package turned off (`CMAKE_DISABLE_FIND_PACKAGE_<Package>`, where `find_package()` searches nothing: the configure goes on as for a package never found, so a codec still asked for, `QB_WITH_ZSTD` or `QB_WITH_BROTLI`, stops on its own error). An include directory counts as gone when the header its search looked for is (a shared prefix the headers were removed from; under `Headers` where the search found an Apple framework), libngtcp2's included; every file the Argon2, zlib, zstd and brotli searches produced -- a library, that header -- is an input of the configure (`CMAKE_CONFIGURE_DEPENDS`), so a removal re-runs it at the next `cmake --build` (OpenSSL's and libngtcp2's inputs are the files their checks read, above, and libngtcp2's core library and headers where its search produced them, off Windows as those checks are; a zlib that FindOpenSSL's own search found for a static OpenSSL -- CMake 3.26 and later search it -- is one too, recorded from the first configure, told by the `ZLIB::ZLIB` that OpenSSL links; with compression off, lost and found nowhere else, it stops the configure, where the generate would fail on that `ZLIB::ZLIB` naming neither, and the way out is another zlib, `QB_WITH_SSL=OFF`, or another OpenSSL with OpenSSL's three entries removed from the cache). A path set by hand or by a parent project is kept as given; for Argon2, zlib, zstd and brotli it is no input either (OpenSSL's files are inputs whoever set them: the configure reads them). Argon2 has no switch of its own: a build directory that had it turns its search off (`-DCMAKE_DISABLE_FIND_PACKAGE_Argon2=ON`) to build with the fallback.

The bundled find-modules for Argon2 and ngtcp2 are installed alongside the package config so that `find_package(qb)` consumers of an Argon2- or QUIC-enabled build can recreate the same imported targets (`qb/CMakeLists.txt:353-362`).

## QB_HAS_* versus QB_WITH_*

The two prefixes are not interchangeable.

- `QB_WITH_*` are **user-facing requests** — options you set on the command line (`QB_WITH_SSL`, `QB_WITH_COMPRESSION`, `QB_WITH_QUIC`, `QB_WITH_PROFILING`).
- `QB_HAS_*` are **resolved results** — what qb actually found after probing (`QB_HAS_SSL`, `QB_HAS_COMPRESSION`, `QB_HAS_QUIC`, `QB_HAS_ARGON2`, `QB_HAS_PROFILING`, `QB_HAS_UUID`, `QB_HAS_LIBEV`).

A `QB_HAS_*` flag drives the corresponding `QB_HAS_*=1` compile definition on qb targets (`qbDependencies.cmake:566-592`). When a requested feature's dependency is missing, qb forces the `QB_WITH_*` option back off so the recorded request matches reality.

## Resolution policy: QB_DEPS_FETCH_FALLBACK

`QB_DEPS_FETCH_FALLBACK` (default **ON**, `qbConfig.cmake:111`) selects how the fetchable dependencies behave. The fetched-via-`FIND_PACKAGE_ARGS` dependencies (GoogleTest, Google Benchmark) and zlib differ at the OFF setting.

| Setting | GoogleTest / Google Benchmark | zlib |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK=ON` (default) | Use the system package if `find_package` locates it; otherwise build the pinned tag from source. | Same: system if present, else build the pinned tag from source. |
| `QB_DEPS_FETCH_FALLBACK=OFF` | **Always build the pinned tag from source** — the system package is ignored (no `FIND_PACKAGE_ARGS`). | Use the system package if present; otherwise compression is disabled (no source build is attempted). |
| `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` | Force `find_package(... CONFIG REQUIRED)` — require a system package, never fetch; configuration fails if missing. | (not applicable) |

The mechanism: for GoogleTest and Google Benchmark, qb appends `FIND_PACKAGE_ARGS QUIET GLOBAL NAMES <pkg>` to the `FetchContent_Declare` **only when `QB_DEPS_FETCH_FALLBACK` is ON** (`qbFetchGoogleDeps.cmake:108-113,170-175`). With the argument present, `FetchContent_MakeAvailable` tries `find_package` first and falls back to the source build; without it, it always builds from source. The `QB_USE_SYSTEM_*` switches short-circuit this entirely with an explicit `find_package(... CONFIG REQUIRED)` (`qbFetchGoogleDeps.cmake:63-68,158-160`).

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
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.5` | Google Benchmark source build |
| `QB_ZLIB_GIT_TAG` | `v1.3.1` | zlib fallback source build |

Override at configure time:

```bash
# src: derived from qb/cmake/qbConfig.cmake:116-121 + qbFetchGoogleDeps.cmake:115-121
cmake -B build -S . -DQB_GOOGLETEST_GIT_TAG=v1.14.0
```

## Forcing system packages

The default already prefers a system package when present. To **require** a system GoogleTest/Benchmark and fail (never fetch) when it is absent:

```bash
# src: derived from qb/cmake/qbFetchGoogleDeps.cmake:63-68,158-160
cmake -B build -S . \
  -DQB_USE_SYSTEM_GTEST=ON \
  -DQB_USE_SYSTEM_BENCHMARK=ON
```

This issues `find_package(GTest CONFIG REQUIRED)` and `find_package(benchmark CONFIG REQUIRED)`, resolving packages that ship CMake config files (vcpkg, Conan, distribution `-dev` packages).

## QUIC tri-state: QB_WITH_QUIC

`QB_WITH_QUIC` is a three-valued cache string with default **AUTO** (`qbConfig.cmake:168`), resolved at `qbDependencies.cmake:281-288`:

The value is matched case-insensitively (`qbDependencies.cmake:281`). The OFF set is matched explicitly; AUTO is matched explicitly; any other value is treated as a required ON.

| Value | Behavior when libngtcp2 is missing |
|---|---|
| `AUTO` (default) | Enable QUIC if libngtcp2 is found; stay silent (no warning) when absent. |
| Any value not in the OFF set and not `AUTO` (for example `ON`, `TRUE`, `1`, `YES`, `Y`) | Require libngtcp2; warn and disable QUIC if it (or SSL) is missing. |
| `OFF`, `FALSE`, `0`, `NO`, or `N` | Disabled outright; no search performed (`qbDependencies.cmake:282-283`). |

In every case, QUIC additionally requires `QB_HAS_SSL` — without OpenSSL, QUIC is disabled regardless of `QB_WITH_QUIC` (and a required ON warns about it). AUTO mirrors how SSL and compression silently auto-detect. One exception, in AUTO and ON alike: a libngtcp2 (or, off Windows, an OpenSSL) this build directory found and later lost stops the configure, naming it, until a search finds one, its feature is turned off (`QB_WITH_QUIC=OFF` for libngtcp2, `QB_WITH_SSL=OFF` for OpenSSL -- turning QUIC off does not clear a lost OpenSSL) or the build directory starts afresh (Huly QB-1185).

## Source layout of fetched dependencies

When a fetchable dependency is built from source, `FetchContent` places its tree under the build directory, for example `build/_deps/googletest-src`, `build/_deps/googlebenchmark-src`, and `build/_deps/zlib-src` (the exact path follows the `FetchContent` naming convention). A system package, by contrast, leaves no `_deps` entry — the per-dependency status message states which path was taken (`qbFetchGoogleDeps.cmake:124-128,186-190`).

## Offline and CI builds

- A source fallback needs **network access to GitHub** on first configure (`GIT_REPOSITORY` with `GIT_SHALLOW TRUE`), unless the sources are pre-populated or a system package is found. [Building](./building.md) notes that Git is required on the configure machine for this reason.
- For air-gapped or reproducible builds: provide the dependencies as system packages on `CMAKE_PREFIX_PATH` and set `QB_USE_SYSTEM_GTEST=ON` / `QB_USE_SYSTEM_BENCHMARK=ON` (require system, never fetch), or pre-populate the `_deps` directory before configuring.
- Leaving `QB_DEPS_FETCH_FALLBACK=ON` with no system packages present is the most convenient default for a fresh machine, but it makes the first configure depend on GitHub reachability.

## Pitfalls

- **`QB_DEPS_FETCH_FALLBACK=OFF` does not mean "never fetch."** For GoogleTest and Google Benchmark it means the opposite — *always* build from source, ignoring the system. To require a system package and never fetch, use `QB_USE_SYSTEM_GTEST` / `QB_USE_SYSTEM_BENCHMARK` instead.
- **Argon2 is invisible without OpenSSL.** Its `find_package` is nested inside the OpenSSL-found branch, so a non-SSL build silently has `QB_HAS_ARGON2=OFF` even if libargon2 is installed.
- **QUIC silently disables without SSL.** With `QB_WITH_QUIC=AUTO` and no OpenSSL, QUIC is off with no warning. Use `QB_WITH_QUIC=ON` to make the missing prerequisite surface as a warning.
- **A missing optional dependency does not fail the build.** SSL, compression, QUIC, and profiling each warn and force their `QB_WITH_*` option off when their dependency is absent. Only **qev** (the in-tree libev fork, `qbDependencies.cmake:114`) and the **Threads** package (`find_package(Threads REQUIRED)`, `qbCompiler.cmake:590-593`) are hard-required; their absence is fatal.
- **Stale googletest/googlebenchmark submodule folders.** The framework no longer vendors these as Git submodules. Older clones may retain `modules/googletest`/`modules/googlebenchmark` directories; CMake ignores them, and you can delete the folders and any stale `.git/config` submodule entries.

## See also

- [Building the QB Actor Framework](./building.md) — full configure/build walkthrough and the complete option matrix.
- [Testing](./testing.md) — how `QB_BUILD_TESTS` and the resolved GoogleTest drive `qb_add_test` / ctest.
- [FAQ](./faq.md) — common configure-time questions.
- [CMake FetchContent module](https://cmake.org/cmake/help/latest/module/FetchContent.html) — upstream reference for `FIND_PACKAGE_ARGS` and `MakeAvailable`.
