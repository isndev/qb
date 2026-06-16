<!-- Verified-against: qb 2.0.0 (c++23 branch) -->
# Versioning and compatibility

qb follows [Semantic Versioning 2.0.0](https://semver.org/). The current version is **2.0.0**, defined in
`cmake/qbConfig.cmake` and consumed by `project()` in `CMakeLists.txt`.

## What each release level means

Given a version `MAJOR.MINOR.PATCH`:

- **PATCH** (`2.0.x`) — bug fixes, security fixes, documentation, and internal improvements. No
  source-breaking API changes.
- **MINOR** (`2.x.0`) — new, backward-compatible features. Existing code that compiles against `2.0` keeps
  compiling against `2.x`. New deprecations may be introduced (see below) but nothing already deprecated is
  removed.
- **MAJOR** (`x.0.0`) — changes that may require source edits, including removals of previously deprecated
  APIs.

## Source and binary compatibility

- **Source compatibility** is guaranteed within a major version, as described above.
- **Binary (ABI) compatibility is not guaranteed across versions.** qb is a heavily templated,
  CRTP-based, partly header-only framework, and several build options change code generation
  (`QB_ENABLE_NATIVE_ARCH`, `QB_ENABLE_LTO`, `QB_ENABLE_FAST_MATH`, sanitizers). Link only translation
  units built with the same qb version, the same standard library, and compatible flags. Do not ship a
  prebuilt qb binary expected to link against differently-configured consumers.

The installed CMake package version file is generated with `COMPATIBILITY SameMajorVersion`, so
`find_package(qb 2.0)` accepts any installed `2.x` but rejects a different major.

## Deprecation policy

When an API is superseded:

1. It is marked deprecated and documented in the [CHANGELOG](./CHANGELOG.md) under *Deprecated*, with the
   replacement.
2. It keeps working for the remainder of the current major series.
3. It may be removed only in the next major release, listed under *Removed*.

A migration path is provided for every removal. See, for example, the time-type change described in
[the migration guide](./readme/6_guides/migration_guide.md).

## Supported versions

The `2.0.x` line is the actively maintained series. Security and bug fixes target the latest minor of the
current major. There is no separate long-term-support branch at this time; this section will be updated if
one is introduced.

## C++ standard and toolchains

qb requires C++23 (`CMAKE_CXX_STANDARD 23`, extensions off) and CMake 3.24. Supported compilers and
platforms are listed in [INSTALL.md](./INSTALL.md). A change to the minimum required standard or toolchain
is treated as a breaking change and reserved for a major release.
