# Reference: Building the QB Framework

This document provides details on the CMake build system used by the QB framework.

## Prerequisites

*   **CMake:** 3.14 or higher.
*   **C++17 Compiler:** GCC 7+, Clang 5+, MSVC 2017+.
*   **(Optional) OpenSSL:** Development libraries (e.g., `libssl-dev`, `openssl-devel`). Needed for `QB_IO_WITH_SSL=ON`.
*   **(Optional) Zlib:** Development libraries (e.g., `zlib1g-dev`, `zlib-devel`). Needed for `QB_IO_WITH_ZLIB=ON`.
*   **(Optional) Google Test:** For building and running tests (`QB_BUILD_TEST=ON`). The framework likely includes this as a submodule or uses `FetchContent`.
*   **(Optional) Google Benchmark:** For building and running benchmarks (`QB_BUILD_BENCHMARK=ON`).

## Standard Build Process

1.  **Clone:** `git clone <repo_url> qb-framework && cd qb-framework`
    *(If dependencies are submodules: `git submodule update --init --recursive`)*
2.  **Configure:**
    ```bash
    mkdir build && cd build
    # Basic Release build
    cmake ..
    # Debug build with tests and SSL
    # cmake .. -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_TEST=ON -DQB_IO_WITH_SSL=ON
    ```
3.  **Build:**
    ```bash
    cmake --build . --config Release # Or Debug
    # Or use make/msbuild directly
    # make -jN
    # msbuild QB_Framework.sln /p:Configuration=Release
    ```
4.  **(Optional) Install:**
    ```bash
    cmake --install . --prefix /path/to/install --config Release
    ```

## CMake Structure

*   **Root `CMakeLists.txt`:** Sets up the main project, defines global options, includes subdirectories.
*   **`qb/source/io/CMakeLists.txt`:** Defines the `qb-io` library target.
*   **`qb/source/core/CMakeLists.txt`:** Defines the `qb-core` library target, linking against `qb-io`.
*   **`example/CMakeLists.txt`:** Includes example subdirectories.
*   **`example/*/CMakeLists.txt`:** Defines executable targets for individual examples, linking against `qb-core` and/or `qb-io`.
*   **`qb/source/*/tests/CMakeLists.txt`:** Defines test executables using Google Test, linking against the relevant library (`qb-core` or `qb-io`).
*   **`cmake/`:** Contains helper CMake modules (e.g., for finding dependencies, configuring targets).

## Key CMake Options

These options are typically defined in the root `CMakeLists.txt` and can be set during the CMake configuration step (`cmake -DOPTION=VALUE ..`).

*   **`CMAKE_BUILD_TYPE`:** Standard CMake option. Sets the build configuration (e.g., `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`). Default is often `Release` or none.
*   **`BUILD_SHARED_LIBS`:** (`ON`/`OFF`, default `OFF`) - Build shared libraries (.dll/.so/.dylib) instead of static libraries (.lib/.a). Note: QB has `QB_DYNAMIC` for similar purpose, check which one is primary.
*   **`QB_BUILD_TEST`:** (`ON`/`OFF`, default likely `ON` based on test files presence) - Build the unit and system tests.
*   **`QB_BUILD_BENCHMARK`:** (`ON`/`OFF`, default likely `OFF`) - Build performance benchmarks (requires Google Benchmark).
*   **`QB_BUILD_DOC`:** (`ON`/`OFF`, default `OFF`) - Enable building documentation targets (e.g., Doxygen).
*   **`QB_BUILD_EXAMPLES`:** (`ON`/`OFF`, default `OFF`) - Build the example applications.
*   **`QB_INSTALL`:** (`ON`/`OFF`, default `ON`) - Generate installation rules for libraries and headers.
*   **`QB_IO_WITH_SSL`:** (`ON`/`OFF`, default likely based on `find_package(OpenSSL)`) - Enable features requiring OpenSSL (SSL/TLS transport, most `qb::crypto` functions). CMake will attempt to find OpenSSL if ON.
*   **`QB_IO_WITH_ZLIB`:** (`ON`/`OFF`, default likely based on `find_package(ZLIB)`) - Enable features requiring Zlib (`qb::compression`). CMake will attempt to find Zlib if ON.
*   **`QB_DYNAMIC`:** (`ON`/`OFF`, default `OFF`) - Build `qb-io` and `qb-core` as shared libraries (.dll/.so/.dylib) instead of static libraries (.lib/.a). Seems to be the primary option based on CMakeLists.txt logic.
*   **`QB_WITH_LOG`:** (`ON`/`OFF`, default `ON`) - Enable nanolog integration (also requires `QB_LOGGER` to be defined/ON).
*   **`QB_STDOUT_LOG`:** (`ON`/`OFF`, default likely `ON` based on CMakeLists) - Enable simple logging to `stdout` via `qb::io::cout()` when the full `QB_LOGGER` is disabled. Useful for basic debugging without the nanolog dependency.
*   **`QB_LOGGER`:** (`ON`/`OFF`, default likely `OFF`) - Enable high-performance logging using the `nanolog` library (requires `nanolog` to be available, possibly as a submodule or dependency, and `QB_WITH_LOG=ON`).
*   **`QB_WITH_TCMALLOC`:** (`ON`/`OFF`, default `OFF`) - Attempt to link with TCMalloc (from gperftools) for potential performance improvements (Linux only).
*   **`QB_BUILD_COVERAGE`:** (`ON`/`OFF`, default `OFF`) - Enable code coverage reporting using lcov/gcovr (Debug builds, non-Windows only).
*   **`QB_BUILD_ARCH`:** (String, default `native` on GCC/Clang) - Specify target architecture flags for optimization (e.g., `native`, `avx2`).
*   **`CMAKE_INSTALL_PREFIX`:** Standard CMake variable. Specifies the directory where `make install` or `cmake --install` will place the built libraries, headers, and CMake configuration files.

## Targets

*   **Libraries:**
    *   `qb-io`: The core I/O library.
    *   `qb-core`: The actor engine library (depends on `qb-io`).
    *   (Shared libraries might have different names depending on the platform).
*   **Executables:**
    *   Examples: Located in `example/core/`, `example/io/`, `example/core_io/`.
    *   Tests: Located in `qb/source/io/tests/` and `qb/source/core/tests/` (if `QB_BUILD_TEST=ON`).
    *   Benchmarks: (if `QB_BUILD_BENCHMARK=ON`).

## Dependencies

*   **Required:**
    *   C++17 Compiler
    *   CMake (>= 3.14)
    *   `libev` (Bundled or System - QB likely includes it or finds it)
    *   `ska_hash` (Bundled - Used for `qb::unordered_map/set`)
    *   `stduuid` (Bundled - Used for `qb::uuid`)
    *   `nlohmann/json` (Bundled - Used for `qb::protocol::json`)
*   **Optional:**
    *   OpenSSL (Needed for `QB_IO_WITH_SSL=ON`)
    *   Zlib (Needed for `QB_IO_WITH_ZLIB=ON`)
    *   Google Test (Needed for `QB_BUILD_TEST=ON`)
    *   Google Benchmark (Needed for `QB_BUILD_BENCHMARK=ON`)
    *   `nanolog` (Needed for `QB_LOGGER=ON`)

## Platform Notes

*   **Windows:** Uses Winsock2. Requires MSVC 2017 or later. Ensure OpenSSL/Zlib development libraries (if needed) are correctly installed and findable by CMake (e.g., via `CMAKE_PREFIX_PATH` or environment variables).
*   **Linux:** Uses POSIX sockets, `libev` (likely bundled), potentially `epoll` internally. Requires GCC 7+ or Clang 5+. Install development packages for optional dependencies (e.g., `libssl-dev`, `zlib1g-dev`).
*   **macOS:** Uses POSIX sockets, `libev` (likely bundled), potentially `kqueue` internally. Requires Clang (Xcode) or GCC. Install optional dependencies (e.g., via Homebrew: `brew install openssl zlib`). You might need to provide hints to CMake for finding libraries installed via Homebrew. 