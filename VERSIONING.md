<!-- Verified-against: qb 3.0.0 (C++20 default, C++23 supported) -->

# Versioning and compatibility

qb follows [Semantic Versioning 2.0.0](https://semver.org/). The version is defined in
`cmake/qbConfig.cmake` (`QB_FRAMEWORK_VERSION`) and consumed by `project()` in `CMakeLists.txt` — that
file is the single source of truth, and every other version string in the tree is a copy of it.

The latest **tagged release** is **2.6.0**. The `develop` branch, where the next release accumulates,
reports **3.0.0**; a build made from `develop` therefore advertises a version that is not yet tagged.
That next release is a **major** one — see [CHANGELOG.md](./CHANGELOG.md) for what makes it major.

The qbm modules (`qbm-http`, `qbm-pgsql`, `qbm-redis`) carry the framework version rather than
versions of their own. They are not standalone-configurable, so a module version can only ever mean
"the qb this was built against"; `project(qbm-<name> VERSION ...)` is kept in lockstep with
`QB_FRAMEWORK_VERSION`, and each module's `scripts/doc-lint.sh` fails if the two disagree whenever
both trees are visible.

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
- **The *compiler* is part of that list, not just the standard library.** clang and gcc over the same
  libstdc++ still disagree on one thing qb depends on: clang appends the cxx11 ABI tag (`B5cxx11`) to
  the mangled name of a function-**local static** whose type carries it, and gcc does not. A local
  static that the code treats as one shared object — a sentinel compared by address, a cache whose
  identity matters — then exists twice in a mixed binary, with no link-time diagnostic. That is a real
  3.0 fix, not a hypothetical (see *Fixed* in [CHANGELOG.md](./CHANGELOG.md): an empty
  `qb::unordered_map` destroyed across the boundary freed static storage). qb's own headers are now
  clean of it and the `ubuntu-abi-sentinel-sweep` CI job keeps them that way, but the guarantee stops
  at qb's surface: prefer one compiler for the whole program.
- **`NDEBUG` is the one exception, and it is qb's obligation, not yours.** A consumer's `NDEBUG` comes
  from its own `CMAKE_BUILD_TYPE` — including the *unset* default, which defines no `NDEBUG` — and
  nothing makes it agree with the build qb was installed from. No public type, member declaration or
  alignment in a shipped header may therefore be selected by `NDEBUG` (or `_DEBUG`/`DEBUG`): the
  disagreement is invisible to the linker and to `find_package`, so it surfaces as silent memory
  corruption rather than an error. Two such splits existed and were removed in 3.0 —
  `qb::unordered_map`/`unordered_set`, and `qb::Event::id_type` — and
  [`scripts/check-abi-macro-split.py`](./scripts/check-abi-macro-split.py) fails CI on a new one.
  `NDEBUG` may still change *behaviour* (`assert`, diagnostics); note that this leaves a formal ODR
  difference on inline functions containing `assert()`, where a mismatched consumer silently loses
  the assertion.

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

The `2.6.x` line is the actively maintained series. Security and bug fixes target the latest minor of the
current major. There is no separate long-term-support branch at this time; this section will be updated if
one is introduced.

## C++ standard and toolchains

qb requires C++20 by default (`QB_CXX_STANDARD=20`, extensions off) and also supports
`QB_CXX_STANDARD=23` for newer standard-library implementations. CMake 3.24 is the minimum. Supported
compilers and platforms are listed in [INSTALL.md](./INSTALL.md). A change to the minimum required
standard or toolchain is treated as a breaking change and reserved for a major release.
