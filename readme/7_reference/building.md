@page ref_building_md Reference: Building the QB Actor Framework
@brief A comprehensive guide to building the QB Actor Framework from source using CMake, including key build options and dependencies.

# Reference: Building the QB Actor Framework

This guide provides detailed information on building the QB Actor Framework from its source code using the CMake build system. Understanding these steps and options will allow you to configure the framework according to your project's needs.

## 1. Prerequisites

Before you begin, ensure your development environment has the following:

*   **C++17 Compliant Compiler:** A modern C++ compiler that supports C++17 features (e.g., GCC 7+, Clang 5+, MSVC 2017+).
*   **CMake:** Version 3.14 or higher is required to process the build scripts.
*   **Git:** For cloning the QB Framework repository.
*   **Optional Dependencies (for extended features):**
    *   **OpenSSL Development Libraries:** Required if you want to enable SSL/TLS for secure networking or use QB's cryptography features. (Package names like `libssl-dev` on Debian/Ubuntu, `openssl-devel` on Fedora/CentOS, or installed via installers on Windows/macOS).
    *   **Zlib Development Libraries:** Needed for data compression features (`qb::compression`). (Package names like `zlib1g-dev` on Debian/Ubuntu, `zlib-devel` on Fedora/CentOS).
    *   **Google Test:** If you intend to build and run the framework's unit and system tests (`QB_BUILD_TEST=ON`). QB may bundle this or fetch it via CMake's `FetchContent`.
    *   **Google Benchmark:** If you plan to build and run performance benchmarks (`QB_BUILD_BENCHMARK=ON`).

## 2. Standard Build Process

The recommended way to build QB is an out-of-source build:

1.  **Clone the Repository:**
    ```bash
    git clone <your_repository_url> qb-framework
    cd qb-framework
    # If the framework uses Git submodules for dependencies, initialize them:
    # git submodule update --init --recursive 
    ```

2.  **Create a Build Directory & Configure with CMake:**
    ```bash
    # From the root of the qb-framework directory
    mkdir build
    cd build

    # Configure the build. Adjust options as needed.
    # Example: Release build, enable tests, disable SSL/Zlib for a minimal build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_TEST=ON -DQB_IO_WITH_SSL=OFF -DQB_IO_WITH_ZLIB=OFF

    # Example: Debug build with tests, SSL, and Zlib enabled
    # cmake .. -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_TEST=ON -DQB_IO_WITH_SSL=ON -DQB_IO_WITH_ZLIB=ON
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
*   **`cmake/` Directory:** Often contains helper CMake modules, for instance, to find external dependencies like OpenSSL or Zlib, or to define custom CMake functions used throughout the build process.

## 4. Key CMake Build Options

You can customize the build by passing options to CMake during the configuration step (e.g., `cmake -DOPTION_NAME=VALUE ..`). Here are some of the most important ones for the QB Framework:

*   **`CMAKE_BUILD_TYPE`**: (String: `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`) Standard CMake option to set the build configuration. Impacts optimization levels and debug information.
*   **`BUILD_SHARED_LIBS`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Standard CMake option. If `ON`, libraries will be built as shared objects (.so, .dll, .dylib). QB might also use a custom `QB_DYNAMIC` option for this.
*   **`QB_DYNAMIC`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Specific QB option to build `qb-io` and `qb-core` as shared/dynamic libraries rather than static ones. This is often the primary control for shared vs. static builds in QB.
*   **`QB_BUILD_TEST`**: (Boolean: `ON`/`OFF`, Default: Often `ON`) Controls whether to build the unit and system tests (typically using Google Test).
*   **`QB_BUILD_BENCHMARK`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Controls whether to build performance benchmarks (may require Google Benchmark).
*   **`QB_BUILD_DOC`**: (Boolean: `ON`/`OFF`, Default: `OFF`) Enables CMake targets related to generating Doxygen API documentation.
*   **`QB_BUILD_EXAMPLES`**: (Boolean: `ON`/`OFF`, Default: Often `ON`) Controls whether to build the example applications provided with the framework.
*   **`QB_INSTALL`**: (Boolean: `ON`/`OFF`, Default: `ON`) If `ON`, CMake will generate installation rules. This allows you to use `cmake --install .`.
*   **`QB_IO_WITH_SSL`**: (Boolean: `ON`/`OFF`, Default: `OFF` or auto-detected) Enables SSL/TLS features in `qb-io` (e.g., `tcp::ssl::socket`, `qb::crypto`). Requires OpenSSL development libraries.
*   **`QB_IO_WITH_ZLIB`**: (Boolean: `ON`/`OFF`, Default: `OFF` or auto-detected) Enables data compression features in `qb-io` (`qb::compression`). Requires Zlib development libraries.
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
    *   **Benchmarks:** If `QB_BUILD_BENCHMARK=ON`.

## 6. Dependencies Overview

*   **Core Required by QB:**
    *   C++17 Standard Library
    *   `libev` (event loop library - QB likely bundles this or provides a CMake script to find/fetch it)
    *   `ska_hash` (for `qb::unordered_map/set` - likely bundled)
    *   `stduuid` (for `qb::uuid` - likely bundled)
    *   `nlohmann/json` (for `qb::protocol::json` - likely bundled)
*   **Optional External Libraries (enabled via CMake options):**
    *   OpenSSL (for `QB_IO_WITH_SSL=ON`)
    *   Zlib (for `QB_IO_WITH_ZLIB=ON`)
    *   Google Test (for `QB_BUILD_TEST=ON` - may be fetched by CMake)
    *   Google Benchmark (for `QB_BUILD_BENCHMARK=ON` - may be fetched by CMake)
    *   `nanolog` (for `QB_LOGGER=ON` - may be a submodule or fetched)

## 7. Platform-Specific Notes

*   **Windows:** Uses Winsock2. Ensure MSVC 2017+ is used. If enabling OpenSSL/Zlib, make sure their development libraries (headers, .lib files) are correctly installed and their paths are known to CMake (e.g., by setting `CMAKE_PREFIX_PATH`, or using environment variables like `OPENSSL_ROOT_DIR`).
*   **Linux:** Uses POSIX sockets. GCC 7+ or Clang 5+ are recommended. Install development packages for optional libraries (e.g., `libssl-dev`, `zlib1g-dev` on Debian/Ubuntu; `openssl-devel`, `zlib-devel` on Fedora/RHEL derivatives).
*   **macOS:** Uses POSIX sockets. Xcode's Clang or a separately installed GCC/Clang should work. Optional dependencies like OpenSSL and Zlib can often be installed via Homebrew (`brew install openssl zlib`). You may need to provide hints to CMake (e.g., via `CMAKE_PREFIX_PATH`) to locate Homebrew-installed libraries.

This guide should provide a solid understanding of how to build and configure the QB Actor Framework to suit your development and deployment needs.

**(Next:** Learn about `[Reference: Testing the QB Framework](./testing.md)` or revisit the `[Getting Started Guide](./../6_guides/getting_started.md)`.)** 