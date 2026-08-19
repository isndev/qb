<!-- Verified-against: qb 3.0.0 (C++20 default, C++23 supported) -->
# CMake options reference

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Every `QB_*` CMake variable that configures a qb build, its default, and what it controls.

**Prerequisites:** [Building the framework](./building.md) — **See also:** [CMake and dependencies](./cmake_dependencies.md), [Testing](./testing.md)

## Summary

qb's build is configured through a fixed set of cache variables, all prefixed `QB_`. They are
declared in [`qb/cmake/qbConfig.cmake`](../../cmake/qbConfig.cmake); the compiler-facing options are
applied in [`qb/cmake/qbCompiler.cmake`](../../cmake/qbCompiler.cmake) and the dependency-facing ones in
[`qb/cmake/qbDependencies.cmake`](../../cmake/qbDependencies.cmake). Pass any of them on the configure
line, for example:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DQB_WITH_SSL=ON -DQB_BUILD_TESTS=OFF
```

The tables below group the options by purpose. Defaults are taken from `qbConfig.cmake` — verbatim
where the declaration writes a literal, and spelled out as a rule where it does not: `QB_BUILD_TESTS`,
`QB_BUILD_EXAMPLES` and `QB_INSTALL` are **computed** from whether qb is the top-level project
(`qb/cmake/qbConfig.cmake:87-97,130-134`), so a single value would be wrong for one of the two cases.

## Concepts

- **`QB_WITH_*` vs `QB_HAS_*`** — `QB_WITH_*` are *requests* (the options you set). `QB_HAS_*` are the
  *resolved capabilities* after dependency probing: a request can be downgraded if its dependency is
  absent. For example, `QB_WITH_SSL=ON` is forced off and `QB_HAS_SSL` is set to `FALSE` when OpenSSL is
  not found (`qbDependencies.cmake`). Set the `QB_WITH_*` option; read the `QB_HAS_*` result.
- **Auto-detected features** — `QB_WITH_SSL` and `QB_WITH_COMPRESSION` request a feature but degrade
  gracefully when the dependency is missing. `QB_WITH_QUIC` is tri-state (see its row below).
- **Fetchable vs system-only dependencies** — GoogleTest, Google Benchmark, Zlib **and nlohmann/json**
  can be built from source via `FetchContent`; the four `QB_*_GIT_TAG` variables pin them.
  OpenSSL, Argon2, and libngtcp2 are never fetched. nlohmann is the odd one out: it is required by
  *every* build (`qb::json` **is** `nlohmann::json`), so an absent system copy plus
  `QB_DEPS_FETCH_FALLBACK=OFF` is a fatal configure error, not a lost feature. The policy is owned by
  [CMake and dependencies](./cmake_dependencies.md).
- **Cache types** — most options are CMake `option()` booleans (`ON`/`OFF`). Eight are `STRING` cache
  variables: `QB_CXX_STANDARD` (`20`/`23`), `QB_WITH_QUIC` (`AUTO`/`ON`/`OFF`), `QB_SANITIZE`,
  `QB_USE_SYSTEM_NLOHMANN` (`AUTO`/`ON`/`OFF`), and the four `QB_*_GIT_TAG` pins.

## Build configuration

| Option | Default | Purpose |
|---|---|---|
| `QB_CXX_STANDARD` | `20` | C++ standard required by qb targets. `STRING` cache variable accepting `20` or `23` (configure fails otherwise); pass `-DQB_CXX_STANDARD=23` for the modern path, as the `debug-cxx23`/`dev-cxx23` presets do. |
| `QB_BUILD_TESTS` | `ON` standalone / `${BUILD_TESTING}` (else `OFF`) embedded | Build the qb GoogleTest suites. Gates GoogleTest resolution and the `qb_add_test` helper. The default is **computed**, not fixed: `ON` only when qb is the top-level project; under `add_subdirectory` it follows `BUILD_TESTING` when the parent defined it, otherwise `OFF`. |
| `QB_BUILD_EXAMPLES` | `ON` standalone / `OFF` embedded | Build the examples. Same computed default as `QB_BUILD_TESTS`, except that an embedded qb is flatly `OFF` — `BUILD_TESTING` does not apply to examples. **This repository ships no `examples/` tree**: the examples are a separate submodule owned by the qb-dev superproject, and only that submodule's `examples/CMakeLists.txt` reads this option, so in a standalone `qb` checkout `ON` builds nothing extra. The configure summary says so when it cannot find the tree (`qb/cmake/qbConfig.cmake:552-555`). |
| `QB_BUILD_BENCHMARKS` | `OFF` | Build the Google Benchmark suites. Gates Google Benchmark resolution. |
| `QB_BUILD_DOCS` | `OFF` | Build the documentation target (`add_subdirectory(docs)`). |
| `QB_BUILD_SHARED_LIBS` | `${BUILD_SHARED_LIBS}` | Build the qb libraries as shared objects instead of static. Defaults to the standard `BUILD_SHARED_LIBS`, so `-DBUILD_SHARED_LIBS=ON` also switches qb to shared, while still allowing a qb-only override. |
| `QB_INSTALL` | `ON` standalone / `OFF` embedded | Install the framework: export `qbTargets`/`qbConfig` and headers (see `find_package(qb)` integration). Computed the same way: an embedded qb must not inject its headers and package files into the parent's `cmake --install`, so a superproject that genuinely wants them passes `-DQB_INSTALL=ON`. |

Building qb standalone uses the defaults above. Building it as part of the **qb-dev superproject** is
different, and not uniformly: the repository root FORCE-enables `QB_BUILD_TESTS=ON` and
`QB_BUILD_EXAMPLES=ON` (`qb-dev/CMakeLists.txt:38,40`), so no preset and no `-D` can turn those two
off — but `QB_BUILD_BENCHMARKS=ON`, on the line between them, carries **no** `FORCE`
(`qb-dev/CMakeLists.txt:39`), and the superproject's hidden `base` preset, which every visible preset
inherits, has already put `QB_BUILD_BENCHMARKS=OFF` into the cache (`qb-dev/CMakePresets.json:21`). A
plain cache default cannot overwrite an existing cache entry, so that root line is a no-op under every
preset: `dev`, `release`, `sanitize`, `sanitize-thread`, `coverage`, `feature-gates` and
`relwithdebinfo` all build **zero** benchmarks. `coverage` is not an exception that switches them off —
it only restates the `OFF` it already inherited. The single preset that turns them back on is
`benchmarks` (`qb-dev/CMakePresets.json:128-135`). `-DQB_BUILD_BENCHMARKS=ON` on the configure line
also works, because a command-line `-D` overrides a preset's `cacheVariables`; and in a preset-free
superproject configure `-DQB_BUILD_BENCHMARKS=OFF` is honoured as well — that is what the missing
`FORCE` buys, and it is exactly what `QB_BUILD_TESTS` and `QB_BUILD_EXAMPLES` do **not** grant you.
The `package` preset takes a third path: `QB_DEV_PACKAGING=ON`
drops both `FORCE`s so all three switches become plain defaults, and sets all three `OFF`
(`qb-dev/CMakeLists.txt:33-41`). See [Benchmarks](./benchmarks.md#building-the-benchmarks). Note that
the `dev` row in [Presets](#presets) below describes qb's **own** `CMakePresets.json`, where `dev`
does enable benchmarks — the two files are not the same file.

## Dependency resolution

| Option | Default | Purpose |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK` | `ON` | For fetchable dependencies (GoogleTest, Google Benchmark, Zlib, nlohmann/json), use the system package if `find_package` locates it, otherwise build the pinned tag from source via `FetchContent` ("system if present, else git"). With **no** system nlohmann this being `OFF` is a **fatal** configure error — there is no qb without nlohmann. |
| `QB_USE_SYSTEM_GTEST` | `OFF` | Require a system GoogleTest (`find_package(GTest CONFIG REQUIRED)`); never fetch. |
| `QB_USE_SYSTEM_BENCHMARK` | `OFF` | Require a system Google Benchmark (`find_package(benchmark CONFIG REQUIRED)`); never fetch. |
| `QB_GOOGLETEST_GIT_TAG` | `v1.15.2` | Git tag (or SHA) for the `FetchContent` googletest build. Advanced. |
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.2` | Git tag (or SHA) for the `FetchContent` googlebenchmark build. Advanced. |
| `QB_ZLIB_GIT_TAG` | `v1.3.1` | Git tag (or SHA) for the `FetchContent` zlib fallback build. Advanced. |
| `QB_USE_SYSTEM_NLOHMANN` | `AUTO` | Tri-state. `AUTO` probes for a system nlohmann_json (>= 3.11) then fetches; `ON` requires one and fails early if absent; `OFF` always fetches. An **installable** build (`QB_INSTALL=ON`, which is the standalone default) needs a real system copy either way — a fetched target is in no export set, so that combination is a configure-time error. |
| `QB_NLOHMANN_GIT_TAG` | `v3.12.0` | Git tag (or SHA) for the `FetchContent` nlohmann_json fallback. Advanced. |

The four `QB_*_GIT_TAG` variables are marked advanced (`mark_as_advanced`); they are visible in
`ccmake`/CMake-GUI under the advanced view. The full resolution policy lives in
[CMake and dependencies](./cmake_dependencies.md).

## Feature toggles

| Option | Default | Purpose |
|---|---|---|
| `QB_WITH_LOGGING` | `ON` | Enable logging support; defines `QB_WITH_LOGGING=1`. |
| `QB_WITH_SSL` | `ON` | Enable SSL/TLS via OpenSSL; defines `QB_WITH_SSL=1`. Forced off (and `QB_HAS_SSL` set to `FALSE`) when OpenSSL is not found. |
| `QB_WITH_COMPRESSION` | `ON` | Enable compression via Zlib; defines `QB_WITH_COMPRESSION=1`. |
| `QB_WITH_QUIC` | `AUTO` | Tri-state QUIC transport via libngtcp2. `AUTO`: enable if libngtcp2 is found, stay quiet when absent. `ON`: require it, warn if missing. `OFF`: disabled. Requires SSL. |
| `QB_WITH_PROFILING` | `OFF` | Enable profiling. On GCC/Clang adds the gprof flags `-pg` and `-fno-omit-frame-pointer` (compile and link); also links gperftools (tcmalloc/profiler) when `find_package(Gperftools)` succeeds, otherwise the option is forced off. Incompatible with `QB_SANITIZE`. |

`QB_WITH_QUIC` is a `STRING` cache variable whose accepted values (`AUTO`, `ON`, `OFF`) are enforced via
`set_property(CACHE QB_WITH_QUIC PROPERTY STRINGS ...)`. OpenSSL, Argon2, and libngtcp2 are system-only;
when absent their features degrade silently (see [CMake and dependencies](./cmake_dependencies.md)).

## Performance and codegen

| Option | Default | Purpose |
|---|---|---|
| `QB_ENABLE_OPTIMIZATIONS` | `ON` | Enable the high-performance optimization flags (GCC/Clang: `-funroll-loops`, `-ftree-vectorize`; MSVC: `/Ot`). |
| `QB_ENABLE_LTO` | `OFF` | Enable Link Time Optimization (GCC/Clang: `-flto`; MSVC: `/GL` + `/LTCG`). |
| `QB_ENABLE_NATIVE_ARCH` | `OFF` | Tune codegen for the build-host CPU (`-march=native`, falling back to `-mcpu=native`; `/arch:AVX2` on MSVC). Off by default so the artefact stays portable; turn it **on** only for a binary that will run on the machine that built it. |
| `QB_ENABLE_FAST_MATH` | `OFF` | Enable `-ffast-math` / `/fp:fast`. Breaks IEEE-754 compliance; off by default. |

Because `QB_ENABLE_NATIVE_ARCH` defaults to `OFF`, a binary built with the defaults is portable: qb
targets a conservative baseline and no host-specific instruction set is baked in. Set
`-DQB_ENABLE_NATIVE_ARCH=ON` (or use the `release-native` preset) only when the artifact will run on
the machine that built it, or on hardware you know matches it — never for something you distribute.

## Coverage, sanitizers, and debug

| Option | Default | Purpose |
|---|---|---|
| `QB_BUILD_COVERAGE` | `OFF` | Code coverage instrumentation (Debug builds, non-Windows only); sets up the `qb-coverage`, `qb-coverage-xml`, and `qb-coverage-html` targets via lcov/gcovr — those three read `.gcno`/`.gcda`, so they are real targets only on a gcov toolchain and fail-fast stubs under clang's LLVM instrumentation. |
| `QB_SANITIZE` | `""` (empty/off) | Comma-separated sanitizer list (for example `address,undefined`, `thread`, `memory`, `leak`) applied to every qb, qbm, and test target plus their link step (GCC/Clang). MSVC supports only `/fsanitize=address`. |
| `QB_DEBUG_MEMORY` | `OFF` | Legacy alias: when set and `QB_SANITIZE` is empty, turns on `QB_SANITIZE=address,undefined`. Also defines `QB_DEBUG_MEMORY=1`. |
| `QB_DEBUG_ACTOR` | `OFF` | Enable actor debugging; defines `QB_DEBUG_ACTOR=1`. |
| `QB_STDOUT_LOGGING` | `OFF` | Enable the stdout logging fallback; defines `QB_STDOUT_LOGGING=1`. |

`QB_SANITIZE` is applied to all targets regardless of `CMAKE_BUILD_TYPE`, so the instrumented set stays
consistent. The flags include `-fno-omit-frame-pointer`, `-fno-sanitize-recover=all` (abort on first
error), and `-g`. The `sanitize` and `sanitize-thread` presets provide ready-made configurations.

## Pitfalls

- **`QB_SANITIZE` and `QB_WITH_PROFILING` collide.** Both intercept the same allocator/runtime hooks
  (sanitizer runtime vs tcmalloc/gperftools). Configuring with both set emits a warning; enable only one.
- **`QB_WITH_SSL` can be silently downgraded.** Requesting `-DQB_WITH_SSL=ON` does not guarantee SSL: if
  OpenSSL is not found, the option is forced off. Read the resolved `QB_HAS_SSL` (or the configure
  summary) rather than assuming the request was honored. The same request-vs-result split applies to
  `QB_WITH_COMPRESSION`/`QB_HAS_COMPRESSION` and `QB_WITH_QUIC`/`QB_HAS_QUIC`.
- **`QB_WITH_QUIC` requires SSL.** QUIC builds on the OpenSSL crypto path; with SSL absent, QUIC cannot
  enable even when set to `ON`.
- **`QB_ENABLE_NATIVE_ARCH=ON` is not portable.** It bakes the build host's instruction set into the
  artifact, so a binary built on a newer CPU dies with SIGILL on an older one. It is `OFF` by default
  and every preset here except `release-native` keeps it off (the qb-dev superproject adds `benchmarks`) — turn it on deliberately, and
  never for something you distribute.
- **`QB_ENABLE_FAST_MATH` changes numeric results.** It relaxes IEEE-754 guarantees; leave it off unless
  the workload tolerates that.
- **`QB_BUILD_COVERAGE` is Debug-only and non-Windows.** Coverage instrumentation and its targets are set
  up only for Debug builds on non-Windows toolchains.

## Presets

The shipped [`CMakePresets.json`](../../CMakePresets.json) bundles common option combinations so you do
not have to pass them by hand. Configure with `cmake --preset <name>`:

| Preset | Option highlights |
|---|---|
| `debug` | `CMAKE_BUILD_TYPE=Debug`, tests and examples on. |
| `release` | `CMAKE_BUILD_TYPE=Release`; inherits `base`, so `QB_BUILD_TESTS=ON`. For a tests-off release, pass `-DQB_BUILD_TESTS=OFF` manually. |
| `relwithdebinfo` | `CMAKE_BUILD_TYPE=RelWithDebInfo`. |
| `dev` | Debug with `QB_BUILD_TESTS=ON`, `QB_BUILD_BENCHMARKS=ON`, `QB_BUILD_EXAMPLES=ON`. |
| `sanitize` | Debug with `QB_SANITIZE=address,undefined`. |
| `sanitize-thread` | Debug with `QB_SANITIZE=thread`. |
| `coverage` | Debug with `QB_BUILD_COVERAGE=ON`. |
| `release-lto` | Release with `QB_ENABLE_LTO=ON`. |
| `release-native` | Release with `QB_ENABLE_NATIVE_ARCH=ON` — the only preset that turns native arch back ON (the `base` preset all presets inherit pins it OFF). |
| `release-portable` | Release with `QB_ENABLE_NATIVE_ARCH=OFF` for distributable binaries. |

```bash
cmake --preset sanitize
cmake --build build/sanitize
```

## See also

- [Building the framework](./building.md) — the full configure/build/install walkthrough.
- [CMake and dependencies](./cmake_dependencies.md) — fetch-vs-system policy and version pinning.
- [Testing](./testing.md) — running the suites that `QB_BUILD_TESTS` enables.
