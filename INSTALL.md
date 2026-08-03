<!-- Verified-against: qb 3.0.0 (C++20 default, C++23 supported) -->

# Installation

How to add qb to a project and build it from source. For the day-to-day build-command reference see
[readme/7_reference/building.md](./readme/7_reference/building.md); for the full option list see
[readme/7_reference/cmake_options.md](./readme/7_reference/cmake_options.md).

## Supported toolchains

Continuous integration builds and tests every change on the following matrix:

| OS                         | Compilers   | Standard library |
|----------------------------|-------------|------------------|
| Linux (`ubuntu-latest`)    | GCC, Clang  | libstdc++        |
| macOS (`macos-latest`)     | Apple Clang | libc++           |
| Windows (`windows-latest`) | MSVC        | MSVC STL         |

Requirements:

- A **C++20**-capable compiler from the families above. C++23 is supported by configuring with
  `-DQB_CXX_STANDARD=23`.
- **CMake 3.24** or newer (required for the `FetchContent` `FIND_PACKAGE_ARGS` resolution qb uses).
- A POSIX threads implementation (pthreads) on non-Windows platforms.

Architectures: x86_64 and ARM64 (including Apple Silicon).

## Dependencies

qb resolves dependencies in three ways. Most builds need nothing installed beyond a compiler and CMake.

| Dependency       | How it is obtained                                                   | Needed for                    |
|------------------|----------------------------------------------------------------------|-------------------------------|
| libev            | Forked + vendored (`qb/include/qb/vendor/ev`), built automatically   | Always (the event loop)       |
| stduuid          | Forked + vendored (`qb/include/qb/vendor/uuid`); system uuid if absent | Always (UUIDs)              |
| GoogleTest       | Fetched at configure time when `QB_BUILD_TESTS=ON`                   | Tests                         |
| Google Benchmark | Fetched at configure time when `QB_BUILD_BENCHMARKS=ON`              | Benchmarks                    |
| zlib             | System first, fetched as a fallback when `QB_DEPS_FETCH_FALLBACK=ON` | `QB_WITH_COMPRESSION`         |
| OpenSSL          | System only (`find_package`)                                         | `QB_WITH_SSL` (TLS, crypto)   |
| Argon2           | System only, looked up only when OpenSSL is present                  | Password hashing              |
| ngtcp2           | System only                                                          | `QB_WITH_QUIC` (requires SSL) |
| gperftools       | System only                                                          | `QB_WITH_PROFILING`           |

Optional system packages, by platform:

```bash
# Debian / Ubuntu
sudo apt-get install libssl-dev libargon2-dev zlib1g-dev

# macOS (Homebrew)
brew install openssl argon2 zlib

# Windows (vcpkg) — as used in CI
vcpkg install openssl:x64-windows argon2:x64-windows zlib:x64-windows
```

Install `libngtcp2` as well when you want QUIC/HTTP3 auto-detection to enable that transport. On
Ubuntu CI this is `libngtcp2-dev` plus an OpenSSL crypto backend package when the image provides one;
on macOS use `brew install libngtcp2`.

If OpenSSL is not found, `QB_WITH_SSL` is forced off and the SSL/QUIC features are disabled rather than
failing the build.

## Integrate into your project

Two integration modes are supported. Both expose the `qb::core` and `qb::io` targets.

### Embed the source tree

```cmake
add_subdirectory(qb)
target_link_libraries(my_app PRIVATE qb::core qb::io)
```

Vendor qb as a submodule first:

```bash
git submodule add https://github.com/isndev/qb.git qb
# qb vendors libev/uuid directly (committed files, not submodules), so they
# arrive with the checkout — no recursive submodule init is needed for qb itself.
```

### Consume an installed copy

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DQB_INSTALL=ON -B build
cmake --build build --parallel
cmake --install build --prefix /your/prefix
```

```cmake
find_package(qb CONFIG REQUIRED)   # provides qb::core and qb::io
target_link_libraries(my_app PRIVATE qb::core qb::io)
```

The install exports `qbConfig.cmake`, a `SameMajorVersion` version file, and the bundled `FindArgon2` /
`FindNgtcp2` modules so downstream `find_package` works without extra setup.

## Build from source

```bash
git clone --recursive https://github.com/isndev/qb.git
cd qb
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

A common production configuration disables host-specific codegen for portable binaries and enables
link-time optimization:

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DQB_ENABLE_NATIVE_ARCH=OFF \
      -DQB_ENABLE_LTO=ON \
      -DQB_WITH_SSL=ON -DQB_WITH_COMPRESSION=ON \
      -B build
```

See [production_checklist.md](./readme/6_guides/production_checklist.md) before shipping.

## Troubleshooting

- **`CMake 3.24 or higher is required`** — upgrade CMake; the dependency resolution relies on it.
- **`libev … not found`** — libev is vendored directly under `qb/include/qb/vendor/ev` (committed files, not a
  submodule), so a normal clone always ships it. If it is missing, restore it from the repo
  (`git checkout -- include/qb/vendor/ev`) or re-clone; a `git submodule update` will not bring it back.
- **SSL features missing** — install OpenSSL development headers; without them `QB_WITH_SSL` is auto-disabled.
- **Host CPU binary fails on another machine** — the default `QB_ENABLE_NATIVE_ARCH=ON` targets the build
  host; rebuild with `-DQB_ENABLE_NATIVE_ARCH=OFF` for portable artifacts.
