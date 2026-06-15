@page ref_building_md Reference: Building the QB Actor Framework
@brief A comprehensive guide to building the QB Actor Framework from source using CMake, including key build options and dependencies.

# Reference: Building the QB Actor Framework

This guide provides detailed information on building the QB Actor Framework from its source code using the CMake build system. Understanding these steps and options will allow you to configure the framework according to your project's needs.

## 1. Prerequisites

Before you begin, ensure your development environment has the following:

*   **C++23-capable compiler:** The framework targets **C++23** (e.g. a recent GCC, Clang, or MSVC with C++23 support). The requirement propagates to consumers as a PUBLIC `cxx_std_23` usage requirement.
*   **CMake:** **3.24 or newer** is required (needed for the `FetchContent` + `find_package` integration; see [CMake and dependencies](./cmake_dependencies.md)).
*   **Git:** Required on the configure machine when a fetchable dependency (GoogleTest, Google Benchmark, Zlib) is not found on the system and is built from source.
*   **Optional Dependencies (for extended features):**
    *   **OpenSSL Development Libraries:** For SSL/TLS and cryptography (`QB_WITH_SSL=ON`). System-provided only (not fetchable); absent → SSL features disabled.
    *   **Zlib Development Libraries:** For compression (`QB_WITH_COMPRESSION=ON`). Found on the system, or built from source when absent and `QB_DEPS_FETCH_FALLBACK=ON` (default).
    *   **libngtcp2 (+ libnghttp3):** For QUIC / HTTP/3 (`QB_WITH_QUIC=AUTO`). System-provided only.
    *   **Google Test / Google Benchmark:** Resolved automatically — **system package if present, otherwise built from source** via FetchContent with pinned tags (when `QB_BUILD_TESTS` / `QB_BUILD_BENCHMARKS` are **ON**). Force a system package with **`QB_USE_SYSTEM_GTEST`** / **`QB_USE_SYSTEM_BENCHMARK`**.

## 2. Standard Build Process

The recommended way to build QB is an out-of-source build:

1.  **Clone the Repository:**
    ```bash
    git clone <your_repository_url> qb-framework
    cd qb-framework
    ```

2.  **Create a Build Directory & Configure with CMake:**
    ```bash
    # From the root of the qb-framework directory
    mkdir build
    cd build

    # Configure the build. Adjust options as needed.
    # Example: Release build, enable tests, disable SSL/Zlib for a minimal build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_TESTS=ON -DQB_WITH_SSL=OFF -DQB_WITH_COMPRESSION=OFF

    # Example: Debug build with tests, SSL, and Zlib enabled
    # cmake .. -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_TESTS=ON -DQB_WITH_SSL=ON -DQB_WITH_COMPRESSION=ON
    ```

3.  **Compile the Code:**
    ```bash
    # From within the 'build' directory
    cmake --build . --config Release  # Or --config Debug, etc.

    # Alternatively, on Linux/macOS, you can often use make for parallel builds:
    # make -j$(nproc) # (or make -j<number_of_cores>)
    # On Windows with MSVC, you might open the generated .sln file in Visual Studio or use msbuild.
    ```

4.  **(Optional) Install the Framework:**
    If you want to install the compiled libraries and headers to a system location or a custom prefix for use by other projects:
    ```bash
    # From within the 'build' directory
    # Installs to default location (e.g., /usr/local on Linux)
    cmake --install . --config Release 

    # Install to a custom location
    # cmake --install . --prefix /path/to/your/custom/install --config Release
    ```

## 3. Understanding the CMake Structure

The QB Framework's CMake build system is organized as follows:

*   **Root `CMakeLists.txt`:** Located at the top level of the framework. It sets up the main project, defines global build options, and includes the `CMakeLists.txt` files of subdirectories (like `qb/source`, `example`, `cmake`).
*   **Module `CMakeLists.txt` (e.g., `qb/source/io/CMakeLists.txt`, `qb/source/core/CMakeLists.txt`):** Each core library (`qb-io`, `qb-core`) has its own CMake file that defines its specific source files, dependencies, and build targets.
*   **Examples & Tests `CMakeLists.txt`:** Directories for examples (`example/`) and tests (`qb/source/*/tests/`) also have their own `CMakeLists.txt` files to define their respective executable targets and link them against the QB libraries.
*   **`cmake/` Directory:** Helper modules (`qbConfig.cmake`, `qbFetchGoogleDeps.cmake`, find modules, etc.). See [CMake and dependencies](./cmake_dependencies.md) for GoogleTest / Google Benchmark behavior.

## 4. Key CMake Build Options

You can customize the build by passing options to CMake during the configuration step (e.g., `cmake -DOPTION_NAME=VALUE ..`). Here are some of the most important ones for the QB Framework:

### Build configuration

*   **`CMAKE_BUILD_TYPE`**: (String: `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`; default `Release`) Standard CMake build configuration.
*   **`BUILD_SHARED_LIBS`** / **`QB_BUILD_SHARED_LIBS`**: (Boolean, Default: `OFF`) Build `qb-io`/`qb-core` (and modules) as shared objects instead of static. `QB_BUILD_SHARED_LIBS` defaults to the value of the standard `BUILD_SHARED_LIBS`.
*   **`QB_BUILD_TESTS`**: (Boolean, Default: `ON`) Build the unit/system tests (Google Test).
*   **`QB_BUILD_BENCHMARKS`**: (Boolean, Default: `OFF`) Build performance benchmarks (Google Benchmark).
*   **`QB_BUILD_EXAMPLES`**: (Boolean, Default: `ON`) Build the example applications.
*   **`QB_BUILD_DOCS`**: (Boolean, Default: `OFF`) Add the Doxygen documentation subdirectory. (`QB_BUILD_DOC` then gates the actual `docs` target.)
*   **`QB_INSTALL`**: (Boolean, Default: `ON`) Generate installation rules (`cmake --install .`).
*   **`CMAKE_INSTALL_PREFIX`**: (Path) Standard install root.

### Optional features

*   **`QB_WITH_SSL`**: (Boolean, Default: `ON`) SSL/TLS + crypto in `qb-io` via OpenSSL. Auto-disables (with a warning) if OpenSSL is absent. Enables Argon2 password hashing when libargon2 is also found.
*   **`QB_WITH_COMPRESSION`**: (Boolean, Default: `ON`) Compression in `qb-io` via Zlib (system, or fetched when `QB_DEPS_FETCH_FALLBACK=ON`).
*   **`QB_WITH_QUIC`**: (Tri-state: `AUTO`/`ON`/`OFF`, Default: `AUTO`) QUIC transport via libngtcp2. `AUTO` enables it iff libngtcp2 is found; `ON` requires it; `OFF` disables it. Requires `QB_WITH_SSL`.
*   **`QB_WITH_LOGGING`**: (Boolean, Default: `ON`) Enable the logging subsystem (nanolog).
*   **`QB_STDOUT_LOGGING`**: (Boolean, Default: `OFF`) Stdout logging fallback when full logging is off.
*   **`QB_WITH_PROFILING`**: (Boolean, Default: `OFF`) Link gperftools (tcmalloc/profiler) when found. Incompatible with `QB_SANITIZE`.

### Performance

*   **`QB_ENABLE_OPTIMIZATIONS`**: (Boolean, Default: `ON`) Extra Release optimization flags (vectorization, loop unrolling, function/data sections).
*   **`QB_ENABLE_NATIVE_ARCH`**: (Boolean, Default: `ON`) Tune codegen for the build-host CPU (`-march=native` / `-mcpu=native`, validated per compiler). **Turn OFF for portable/distributable binaries** (use the `release-portable` preset).
*   **`QB_ENABLE_LTO`**: (Boolean, Default: `OFF`) Link Time Optimization for Release.
*   **`QB_ENABLE_FAST_MATH`**: (Boolean, Default: `OFF`) `-ffast-math` / `/fp:fast` (breaks IEEE-754).

### Diagnostics

*   **`QB_SANITIZE`**: (String, Default: empty) Comma-separated sanitizer list applied to every qb/qbm/test target and its link step, e.g. `address,undefined`, `thread`, `memory`, `leak`. Use the `sanitize` / `sanitize-thread` presets. (Legacy: `QB_DEBUG_MEMORY=ON` ⇒ `QB_SANITIZE=address,undefined`.)
*   **`QB_BUILD_COVERAGE`**: (Boolean, Default: `OFF`) gcov/lcov coverage instrumentation (Debug, non-Windows).
*   **`QB_DEBUG_ACTOR`**: (Boolean, Default: `OFF`) Extra actor-system debug instrumentation.

### Dependency resolution

*   **`QB_DEPS_FETCH_FALLBACK`**: (Boolean, Default: `ON`) Build fetchable deps (GoogleTest, Google Benchmark, Zlib) from source when not found on the system. OFF = system-only for those.
*   **`QB_USE_SYSTEM_GTEST`** / **`QB_USE_SYSTEM_BENCHMARK`**: (Boolean, Default: `OFF`) Force `find_package(... CONFIG REQUIRED)` and never fetch.
*   **`QB_GOOGLETEST_GIT_TAG`** / **`QB_GOOGLEBENCHMARK_GIT_TAG`** / **`QB_ZLIB_GIT_TAG`**: (String, advanced cache) Pin the fetched source revision.

*Always check the root `CMakeLists.txt` and `cmake/` directory for the most up-to-date and complete list of options specific to your version of QB.*

## 5. Build Targets

Successfully building the framework will produce several targets:

*   **Libraries:**
    *   `qb-io`: The core asynchronous I/O and utilities library.
    *   `qb-core`: The actor model engine (depends on `qb-io`).
    *   Shared library versions might have platform-specific extensions (e.g., `libqb-io.so`, `qb-io.dll`, `libqb-io.dylib`).
*   **Executables (if enabled via CMake options):**
    *   **Examples:** Located in `build/bin/example/<module_category>/<example_name>` (or similar path depending on CMake setup).
    *   **Tests:** Located in `build/bin/qb/source/<module>/tests/<test_type>/<test_name>`.
    *   **Benchmarks:** If `QB_BUILD_BENCHMARKS=ON`.

## 6. Dependencies Overview

*   **Core Required by QB:**
    *   C++23 standard library (as configured by the project)
    *   `libev` (event loop library - QB likely bundles this or provides a CMake script to find/fetch it)
    *   `ska_hash` (for `qb::unordered_map/set` - likely bundled)
    *   `stduuid` (for `qb::uuid` - likely bundled)
    *   `nlohmann/json` (for `qb::protocol::json` - likely bundled)
*   **Optional External Libraries (enabled via CMake options):**
    *   OpenSSL (for `QB_WITH_SSL=ON`) — system-provided
    *   Argon2 (auto-enabled with SSL when libargon2 is found) — system-provided
    *   Zlib (for `QB_WITH_COMPRESSION=ON`) — system, or fetched as fallback
    *   libngtcp2 / libnghttp3 (for `QB_WITH_QUIC`, HTTP/3) — system-provided
    *   Google Test (for `QB_BUILD_TESTS=ON` — system if present, else fetched; see [cmake_dependencies.md](./cmake_dependencies.md))
    *   Google Benchmark (for `QB_BUILD_BENCHMARKS=ON` — same)
    *   `nanolog` (bundled, used when `QB_WITH_LOGGING=ON`)

## 7. Platform-Specific Notes

*   **Windows:** Uses Winsock2. Use a **Visual Studio 2022** (or newer) installation whose MSVC toolset supports **`/std:c++23`** for the features qb relies on. If enabling OpenSSL/Zlib, ensure development libraries (headers, `.lib` files) are on `CMAKE_PREFIX_PATH` or via variables such as `OPENSSL_ROOT_DIR`.
*   **Linux:** Uses POSIX sockets. Prefer **GCC 12+** or **Clang 16+** for solid **C++23** support. Install development packages for optional libraries (e.g., `libssl-dev`, `zlib1g-dev` on Debian/Ubuntu; `openssl-devel`, `zlib-devel` on Fedora/RHEL derivatives).
*   **macOS:** Uses POSIX sockets. Recent **Xcode** / Apple Clang with C++23 support is recommended. Optional dependencies such as OpenSSL and Zlib are often installed via Homebrew (`brew install openssl zlib`); point CMake with `CMAKE_PREFIX_PATH` if needed.

This guide should provide a solid understanding of how to build and configure the QB Actor Framework to suit your development and deployment needs.

**(Next:** [CMake and dependencies](./cmake_dependencies.md) · [Testing](./testing.md) · [Getting Started](../6_guides/getting_started.md).)** 