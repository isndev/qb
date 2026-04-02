@page ref_cmake_dependencies_md Reference: CMake and third-party dependencies
@brief How qb resolves CMake version, GoogleTest, and Google Benchmark (FetchContent vs system packages).

# Reference: CMake and dependencies (2026)

## CMake version

The framework requires **CMake 3.22 or newer**. This matches common LTS images, improves C++23 ergonomics, and aligns `qb`, bundled modules (`ev`, `uuid`), and consumer projects such as `qb-dev`.

**Useful modern CMake practices used in qb:**

- **Target-based** usage: `target_link_libraries`, `target_compile_definitions`, `INTERFACE`/`PRIVATE` propagation instead of global `include_directories` for public API (some legacy `include_directories` remain for bundled headers).
- **`FetchContent`** for optional dev dependencies (GoogleTest, Google Benchmark) with **pinned `GIT_TAG`** values for reproducible configures.
- **`cmake -B` / `-S`** and **`cmake --build`** for out-of-source builds (see [building.md](./building.md)).

Official references: [CMake documentation](https://cmake.org/cmake/help/latest/) — especially the [FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html) module.

## GoogleTest and Google Benchmark

When **`QB_BUILD_TESTS`** is **ON**, qb pulls **GoogleTest** via **`FetchContent`** unless you opt into a system install.

When **`QB_BUILD_BENCHMARKS`** is **ON**, qb pulls **Google Benchmark** the same way.

Default clone locations (first configure): under your build tree, e.g. `_deps/googletest-src` and `_deps/googlebenchmark-src` (exact path depends on the generator).

### Pinning versions

Cache variables (advanced) control the Git tag or commit:

| Variable | Default | Purpose |
|----------|---------|---------|
| `QB_GOOGLETEST_GIT_TAG` | `v1.15.2` | googletest revision |
| `QB_GOOGLEBENCHMARK_GIT_TAG` | `v1.9.2` | benchmark revision |

Example:

```bash
cmake -B build -S . -DQB_GOOGLETEST_GIT_TAG=v1.14.0
```

### System packages instead of FetchContent

If the toolchain already provides **Config** packages:

```bash
cmake -B build -S . \
  -DQB_USE_SYSTEM_GTEST=ON \
  -DQB_USE_SYSTEM_BENCHMARK=ON
```

That enables **`find_package(GTest CONFIG REQUIRED)`** and **`find_package(benchmark CONFIG REQUIRED)`** (e.g. vcpkg, Conan, distro `-dev` packages with CMake config files).

### Offline / CI caches

- First configure with FetchContent needs **network** access to GitHub (unless sources are pre-populated).
- For air-gapped or cached builds, vendor the tags you need or use **`QB_USE_SYSTEM_*`** with a local prefix (`CMAKE_PREFIX_PATH`).

### Submodule removal

The framework **no longer** vendors googletest/googlebenchmark as **git submodules**. Existing clones may still have `modules/googletest` or `modules/googlebenchmark` directories from an older checkout; they are **ignored** by CMake — you can delete those folders and remove stale submodule entries from `.git/config` if needed.

**(Next:** [Building](./building.md) · [Testing](./testing.md).)**
