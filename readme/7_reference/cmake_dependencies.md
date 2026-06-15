@page ref_cmake_dependencies_md Reference: CMake and third-party dependencies
@brief How qb resolves CMake version, GoogleTest, and Google Benchmark (FetchContent vs system packages).

# Reference: CMake and dependencies (2026)

## CMake version

The framework requires **CMake 3.24 or newer**. 3.24 is the floor because qb uses the `FetchContent` + `find_package` integration (`FIND_PACKAGE_ARGS`) to resolve fetchable dependencies system-first with a from-source fallback.

**Useful modern CMake practices used in qb:**

- **Target-based** usage: `target_link_libraries`, `target_compile_definitions`, `INTERFACE`/`PRIVATE` propagation instead of global `include_directories` for public API (some legacy `include_directories` remain for bundled headers).
- **`FetchContent`** for optional dev dependencies (GoogleTest, Google Benchmark) with **pinned `GIT_TAG`** values for reproducible configures.
- **`cmake -B` / `-S`** and **`cmake --build`** for out-of-source builds (see [building.md](./building.md)).

Official references: [CMake documentation](https://cmake.org/cmake/help/latest/) — especially the [FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html) module.

## Dependency resolution policy

qb resolves **fetchable** dependencies (those that build cleanly from source: GoogleTest, Google Benchmark, Zlib) with a single, consistent rule:

| Setting | Behavior |
|---------|----------|
| `QB_DEPS_FETCH_FALLBACK=ON` (default) | Use the **system** package if `find_package` locates it, otherwise **build the pinned tag from source** (FetchContent). |
| `QB_DEPS_FETCH_FALLBACK=OFF` | Never fetch — use the system package if present, otherwise the feature is disabled (Zlib) or configuration fails (forced deps). |
| `QB_USE_SYSTEM_GTEST` / `QB_USE_SYSTEM_BENCHMARK = ON` | Force `find_package(... CONFIG REQUIRED)` for that dependency — never fetch. |

**Not fetchable** (no clean CMake source build): **OpenSSL**, **Argon2**, **libngtcp2/libnghttp3**. These must be provided by the system (Homebrew, apt/dnf, vcpkg, …); when absent the corresponding optional feature (SSL, Argon2 hashing, QUIC/HTTP3) is disabled.

When a dependency is built from source, its sources land under the build tree, e.g. `_deps/googletest-src`, `_deps/googlebenchmark-src`, `_deps/zlib-src` (exact path depends on the generator).

## GoogleTest, Google Benchmark and Zlib

When **`QB_BUILD_TESTS`** is **ON**, qb resolves **GoogleTest** by the policy above; **`QB_BUILD_BENCHMARKS`** does the same for **Google Benchmark**; **`QB_WITH_COMPRESSION`** does the same for **Zlib**.

### Pinning versions

Cache variables (advanced) control the Git tag or commit:

| Variable | Default | Purpose |
|----------|---------|---------|
| `QB_GOOGLETEST_GIT_TAG` | `v1.15.2` | googletest revision |
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.2` | benchmark revision |
| `QB_ZLIB_GIT_TAG` | `v1.3.1` | zlib revision (fallback build only) |

Example:

```bash
cmake -B build -S . -DQB_GOOGLETEST_GIT_TAG=v1.14.0
```

### Forcing system packages

The default already prefers a system package when present. To **require** one (and fail if missing, never fetch):

```bash
cmake -B build -S . \
  -DQB_USE_SYSTEM_GTEST=ON \
  -DQB_USE_SYSTEM_BENCHMARK=ON
```

That enables **`find_package(GTest CONFIG REQUIRED)`** and **`find_package(benchmark CONFIG REQUIRED)`** (e.g. vcpkg, Conan, distro `-dev` packages with CMake config files).

### Offline / CI caches

- A from-source fallback needs **network** access to GitHub on first configure (unless sources are pre-populated or a system package is found).
- For air-gapped builds: pre-populate `_deps`, set `QB_DEPS_FETCH_FALLBACK=OFF` with system packages on `CMAKE_PREFIX_PATH`, or use `QB_USE_SYSTEM_*=ON`.

### Submodule removal

The framework **no longer** vendors googletest/googlebenchmark as **git submodules**. Existing clones may still have `modules/googletest` or `modules/googlebenchmark` directories from an older checkout; they are **ignored** by CMake — you can delete those folders and remove stale submodule entries from `.git/config` if needed.

**(Next:** [Building](./building.md) · [Testing](./testing.md).)**
