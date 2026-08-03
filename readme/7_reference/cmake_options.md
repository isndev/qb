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

The tables below group the options by purpose. Defaults are taken verbatim from `qbConfig.cmake`.

## Concepts

- **`QB_WITH_*` vs `QB_HAS_*`** — `QB_WITH_*` are *requests* (the options you set). `QB_HAS_*` are the
  *resolved capabilities* after dependency probing: a request can be downgraded if its dependency is
  absent. For example, `QB_WITH_SSL=ON` is forced off and `QB_HAS_SSL` is set to `FALSE` when OpenSSL is
  not found (`qbDependencies.cmake`). Set the `QB_WITH_*` option; read the `QB_HAS_*` result.
- **Auto-detected features** — `QB_WITH_SSL` and `QB_WITH_COMPRESSION` request a feature but degrade
  gracefully when the dependency is missing. `QB_WITH_QUIC` is tri-state (see its row below).
- **Fetchable vs system-only dependencies** — only the CMake-native dependencies (GoogleTest, Google
  Benchmark, Zlib) can be built from source via `FetchContent`; the `QB_*_GIT_TAG` variables pin them.
  OpenSSL, Argon2, and libngtcp2 are never fetched. The policy is owned by
  [CMake and dependencies](./cmake_dependencies.md).
- **Cache types** — most options are CMake `option()` booleans (`ON`/`OFF`). Six are `STRING` cache
  variables: `QB_CXX_STANDARD` (`20`/`23`), `QB_WITH_QUIC` (`AUTO`/`ON`/`OFF`), `QB_SANITIZE`, and the
  three `QB_*_GIT_TAG` pins.

## Build configuration

| Option | Default | Purpose |
|---|---|---|
| `QB_CXX_STANDARD` | `20` | C++ standard required by qb targets. `STRING` cache variable accepting `20` or `23` (configure fails otherwise); pass `-DQB_CXX_STANDARD=23` for the modern path, as the `debug-cxx23`/`dev-cxx23` presets do. |
| `QB_BUILD_TESTS` | `ON` | Build the qb GoogleTest suites. Gates GoogleTest resolution and the `qb_add_test` helper. |
| `QB_BUILD_EXAMPLES` | `ON` | Build the bundled examples. |
| `QB_BUILD_BENCHMARKS` | `OFF` | Build the Google Benchmark suites. Gates Google Benchmark resolution. |
| `QB_BUILD_DOCS` | `OFF` | Build the documentation target (`add_subdirectory(docs)`). |
| `QB_BUILD_SHARED_LIBS` | `${BUILD_SHARED_LIBS}` | Build the qb libraries as shared objects instead of static. Defaults to the standard `BUILD_SHARED_LIBS`, so `-DBUILD_SHARED_LIBS=ON` also switches qb to shared, while still allowing a qb-only override. |
| `QB_INSTALL` | `ON` | Install the framework: export `qbTargets`/`qbConfig` and headers (see `find_package(qb)` integration). |

The repository-root `CMakeLists.txt` forces `QB_BUILD_TESTS=ON` and `QB_BUILD_EXAMPLES=ON`, and
defaults `QB_BUILD_BENCHMARKS=ON` (overridable, e.g. the `coverage` preset turns it off) when building
the whole workspace; building qb standalone uses the defaults above.

## Dependency resolution

| Option | Default | Purpose |
|---|---|---|
| `QB_DEPS_FETCH_FALLBACK` | `ON` | For fetchable dependencies (GoogleTest, Google Benchmark, Zlib), use the system package if `find_package` locates it, otherwise build the pinned tag from source via `FetchContent` ("system if present, else git"). |
| `QB_USE_SYSTEM_GTEST` | `OFF` | Require a system GoogleTest (`find_package(GTest CONFIG REQUIRED)`); never fetch. |
| `QB_USE_SYSTEM_BENCHMARK` | `OFF` | Require a system Google Benchmark (`find_package(benchmark CONFIG REQUIRED)`); never fetch. |
| `QB_GOOGLETEST_GIT_TAG` | `v1.15.2` | Git tag (or SHA) for the `FetchContent` googletest build. Advanced. |
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.2` | Git tag (or SHA) for the `FetchContent` googlebenchmark build. Advanced. |
| `QB_ZLIB_GIT_TAG` | `v1.3.1` | Git tag (or SHA) for the `FetchContent` zlib fallback build. Advanced. |

The three `QB_*_GIT_TAG` variables are marked advanced (`mark_as_advanced`); they are visible in
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
| `QB_ENABLE_NATIVE_ARCH` | `ON` | Tune codegen for the build-host CPU (`-march=native`, falling back to `-mcpu=native`; `/arch:AVX2` on MSVC). Turn **off** for portable, distributable binaries that must run on a different CPU. |
| `QB_ENABLE_FAST_MATH` | `OFF` | Enable `-ffast-math` / `/fp:fast`. Breaks IEEE-754 compliance; off by default. |

Because `QB_ENABLE_NATIVE_ARCH` defaults to `ON`, a binary built with the defaults is tuned for the
machine that built it. Set `-DQB_ENABLE_NATIVE_ARCH=OFF` (or use the `release-portable` preset) when the
artifact will run on other hosts.

## Coverage, sanitizers, and debug

| Option | Default | Purpose |
|---|---|---|
| `QB_BUILD_COVERAGE` | `OFF` | Code coverage instrumentation (Debug builds, non-Windows only); sets up the `qb-coverage`, `qb-coverage-xml`, and `qb-coverage-html` targets via lcov/gcovr. |
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
- **`QB_ENABLE_NATIVE_ARCH=ON` is not portable.** The default optimizes for the build host. Distributable
  artifacts need `QB_ENABLE_NATIVE_ARCH=OFF`.
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
