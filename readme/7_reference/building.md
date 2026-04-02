@page ref_building_md Reference: Building the QB Actor Framework
@brief A comprehensive guide to building the QB Actor Framework from source using CMake, including key build options and dependencies.

# Reference: Building the QB Actor Framework

This guide provides detailed information on building the QB Actor Framework from its source code using the CMake build system. Understanding these steps and options will allow you to configure the framework according to your project's needs.

## 1. Prerequisites

Before you begin, ensure your development environment has the following:

*   **C++23-capable compiler:** The framework targets **C++23** (e.g. a recent GCC, Clang, or MSVC with C++23 support).
*   **CMake:** **3.22 or newer** is required (see also [CMake and dependencies](./cmake_dependencies.md)).
*   **Git:** Required on the configure machine if you use **FetchContent** for GoogleTest / Google Benchmark (default when tests or benchmarks are enabled).
*   **Optional Dependencies (for extended features):**
    *   **OpenSSL Development Libraries:** For SSL/TLS and cryptography (`QB_WITH_SSL=ON`).
    *   **Zlib Development Libraries:** For compression (`QB_WITH_COMPRESSION=ON`).
    *   **Google Test / Google Benchmark:** Not a manual install by default — qb uses **CMake FetchContent** with pinned tags when `QB_BUILD_TESTS` / `QB_BUILD_BENCHMARKS` are **ON**. Use **`QB_USE_SYSTEM_GTEST`** / **`QB_USE_SYSTEM_BENCHMARK`** with **`find_package(... CONFIG)`** if you prefer vcpkg, Conan, or distro packages.

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

*   **`CMAKE_BUILD_TYPE`**: (String: `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`) Standard CMake option to set the build configuration. Impacts optimization levels and debug information.
*   **`BUILD_SHARED_LIBS`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Standard CMake option. If `ON`, libraries will be built as shared objects (.so, .dll, .dylib). QB might also use a custom `QB_DYNAMIC` option for this.
*   **`QB_DYNAMIC`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Specific QB option to build `qb-io` and `qb-core` as shared/dynamic libraries rather than static ones. This is often the primary control for shared vs. static builds in QB.
*   **`QB_BUILD_TESTS`**: (Boolean: `ON`/`OFF`, Default: Often `ON`) Controls whether to build the unit and system tests (typically using Google Test).
*   **`QB_BUILD_BENCHMARKS`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Controls whether to build performance benchmarks (may require Google Benchmark).
*   **`QB_BUILD_DOC`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Enables CMake targets related to generating Doxygen API documentation.
*   **`QB_BUILD_EXAMPLES`**: (Boolean: `ON`/`OFF`, Default: Often `ON`) Controls whether to build the example applications provided with the framework.
*   **`QB_INSTALL`**: (Boolean: `ON`/`OFF`, Default: `ON`) If `ON`, CMake will generate installation rules. This allows you to use `cmake --install .`.
*   **`QB_WITH_SSL`**: (Boolean: `ON`/`OFF`, default often `ON` with auto-detect) Enables SSL/TLS in `qb-io`. Requires OpenSSL development libraries when `ON`.
*   **`QB_WITH_COMPRESSION`**: (Boolean: `ON`/`OFF`, default often `ON` with auto-detect) Enables compression in `qb-io`. Requires Zlib when `ON`.
*   **`QB_USE_SYSTEM_GTEST`**: (Boolean: `OFF` by default) If `ON`, uses **`find_package(GTest CONFIG REQUIRED)`** instead of **FetchContent** for tests.
*   **`QB_USE_SYSTEM_BENCHMARK`**: (Boolean: `OFF` by default) If `ON`, uses **`find_package(benchmark CONFIG REQUIRED)`** instead of **FetchContent** for benchmarks.
*   **`QB_GOOGLETEST_GIT_TAG`** / **`QB_GOOGLEBENCHMARK_GIT_TAG`**: (String, advanced cache) Pin FetchContent to a tag or full commit hash.
*   **`QB_LOGGER`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Enables integration with the `nanolog` high-performance logging library. Requires `nanolog` to be available (e.g., as a submodule) and typically also `QB_WITH_LOG=ON`.
*   **`QB_WITH_LOG`**: (Boolean: `ON`/`OFF`, Default: `ON`) A general switch that might enable logging infrastructure, often a prerequisite for `QB_LOGGER`.
*   **`QB_STDOUT_LOG`**: (Boolean: `ON`/`OFF`, Default: Often `ON` if `QB_LOGGER` is `OFF`) Enables simple diagnostic logging to `stdout` via `qb::io::cout()` when the full `nanolog` system is not active.
*   **`QB_WITH_TCMALLOC`**: (Boolean: `ON`/`OFF`, Default: `OFF`) If `ON` (Linux only), attempts to link the application with TCMalloc (from Google Performance Tools) as the memory allocator, which can sometimes improve performance for memory-intensive applications.
*   **`QB_BUILD_COVERAGE`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Enables code coverage reporting flags (e.g., for gcov/lcov). Typically used with `Debug` builds on non-Windows platforms.
*   **`QB_BUILD_ARCH`**: (String, Default: `native` on GCC/Clang) Allows specifying CPU architecture-specific optimization flags (e.g., `native`, `avx2`).
*   **`CMAKE_INSTALL_PREFIX`**: (Path) Standard CMake variable. Specifies the root directory where libraries, headers, and CMake package configuration files will be installed when you run `cmake --install .`.

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
    *   OpenSSL (for `QB_IO_WITH_SSL=ON`)
    *   Zlib (for `QB_IO_WITH_ZLIB=ON`)
    *   Google Test (for `QB_BUILD_TESTS=ON` — **FetchContent** by default; see [cmake_dependencies.md](./cmake_dependencies.md))
    *   Google Benchmark (for `QB_BUILD_BENCHMARKS=ON` — same)
    *   `nanolog` (for `QB_LOGGER=ON` - may be a submodule or fetched)

## 7. Platform-Specific Notes

*   **Windows:** Uses Winsock2. Use a **Visual Studio 2022** (or newer) installation whose MSVC toolset supports **`/std:c++23`** for the features qb relies on. If enabling OpenSSL/Zlib, ensure development libraries (headers, `.lib` files) are on `CMAKE_PREFIX_PATH` or via variables such as `OPENSSL_ROOT_DIR`.
*   **Linux:** Uses POSIX sockets. Prefer **GCC 12+** or **Clang 16+** for solid **C++23** support. Install development packages for optional libraries (e.g., `libssl-dev`, `zlib1g-dev` on Debian/Ubuntu; `openssl-devel`, `zlib-devel` on Fedora/RHEL derivatives).
*   **macOS:** Uses POSIX sockets. Recent **Xcode** / Apple Clang with C++23 support is recommended. Optional dependencies such as OpenSSL and Zlib are often installed via Homebrew (`brew install openssl zlib`); point CMake with `CMAKE_PREFIX_PATH` if needed.

This guide should provide a solid understanding of how to build and configure the QB Actor Framework to suit your development and deployment needs.

**(Next:** [CMake and dependencies](./cmake_dependencies.md) · [Testing](./testing.md) · [Getting Started](../6_guides/getting_started.md).)** 