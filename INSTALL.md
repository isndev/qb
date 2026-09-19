<!-- Verified-against: qb 3.1.0 (C++20 default, C++23 supported) -->

# Installation

How to add qb to a project and build it from source. For the day-to-day build-command reference see
[readme/7_reference/building.md](./readme/7_reference/building.md); for the full option list see
[readme/7_reference/cmake_options.md](./readme/7_reference/cmake_options.md).

## Supported toolchains

Continuous integration builds and tests every change on the following matrix:

| OS                         | Compilers   | Standard library | CI status |
|----------------------------|-------------|------------------|-----------|
| Linux (`ubuntu-latest`)    | GCC, Clang  | libstdc++        | enabled |
| macOS (`macos-latest`)     | Apple Clang | libc++           | enabled |
| Windows (`windows-latest`) | MSVC        | MSVC STL         | **currently disabled** |

> **Windows/MSVC is supported source, but is not exercised by CI right now.** The
> `windows-msvc-cxx20-release` matrix entry is commented out in
> [`.github/workflows/cmake.yml`](.github/workflows/cmake.yml) — every run would rebuild all
> dependencies from source, which is what makes it unaffordable until the CI has a vcpkg binary
> cache. Treat a Windows build as verified by you, not by this project's CI, until that entry is
> uncommented.

Requirements:

- A **C++20**-capable compiler from the families above. C++23 is supported by configuring with
  `-DQB_CXX_STANDARD=23`.
- **CMake 3.24** or newer (required for the `FetchContent` `FIND_PACKAGE_ARGS` resolution qb uses).
  The floor is a tested one, not just a declared one: the qb-dev superproject's `cmake-floor` job
  configures qb on 3.24.4 and on 3.28.3 on every push, so a distro CMake (Ubuntu 24.04 LTS ships
  3.28.3, Debian 12 ships 3.25.1, RHEL 9 ships 3.26.5) is enough. Consuming qb's own
  `CMakePresets.json` needs 3.24 (schema v3); the qb-dev superproject's presets are schema v6
  and need 3.25.
- A POSIX threads implementation (pthreads) on non-Windows platforms.

Architectures: x86_64 and ARM64 (including Apple Silicon).

### Known-bad toolchain: GCC 14.2 and `co_await` on a temporary

**GCC 14.2 crashes with an internal compiler error on a `co_await` whose operand is a call taking a
temporary argument.** Measured on Debian 13 / aarch64, by differential compilation with the project's
own build command:

```
internal compiler error: in gimple_add_tmp_var, at gimplify.cc:802
```

The trigger is ordinary, valid C++20 — a coroutine awaiting a call whose argument is materialised in
place, e.g. `co_await db->execute(stmt, params{std::vector<std::string>{...}})`. Clang 19.1.7 compiles
the identical translation unit cleanly, which is what identifies this as a **compiler defect, not a qb
defect**. It first surfaced in a `qbm-pgsql` parameter-round-trip integration test; no upstream GCC bug
number is recorded here, and whether x86_64 GCC 14.2 is equally affected has **not** been verified.

**qb does not work around it, deliberately.** The shape occurs at 26 call sites in `qbm-pgsql`'s tests
alone and is idiomatic throughout the coroutine API, so hoisting the temporary in the one file that
happened to crash would turn a loud, attributable compiler crash into a silent trap that adopters hit
in their own code instead — with nothing anywhere to name the cause.

If you are pinned to GCC 14.2, either:

- **hoist the temporary into a named local** before awaiting —
  `auto args = params{...}; auto r = co_await db->execute(stmt, std::move(args));` — at each site the
  ICE reports, or
- **build with Clang**, which the CI matrix above exercises on the same platform.

## Dependencies

qb resolves dependencies in three ways. Most builds need nothing installed beyond a compiler and CMake.

| Dependency       | How it is obtained                                                   | Needed for                    |
|------------------|----------------------------------------------------------------------|-------------------------------|
| qev (libev fork) | Forked, in-tree (`qb/src/qb/ev`), built automatically        | Always (the event loop)       |
| stduuid          | Forked + vendored (`qb/src/qb/vendor/uuid`); system uuid if absent   | Always (UUIDs)                |
| nlohmann/json    | System first, fetched as a fallback when `QB_DEPS_FETCH_FALLBACK=ON` | Always (`qb::json`)           |
| GoogleTest       | Fetched at configure time when `QB_BUILD_TESTS=ON`                   | Tests                         |
| Google Benchmark | Fetched at configure time when `QB_BUILD_BENCHMARKS=ON`              | Benchmarks                    |
| zlib             | System first, fetched as a fallback when `QB_DEPS_FETCH_FALLBACK=ON` | `QB_WITH_COMPRESSION`         |
| OpenSSL          | System only (`find_package`)                                         | `QB_WITH_SSL` (TLS, crypto)   |
| Argon2           | System only, looked up only when OpenSSL is present                  | Password hashing              |
| ngtcp2           | System only                                                          | `QB_WITH_QUIC` (requires SSL) |
| gperftools       | System only                                                          | `QB_WITH_PROFILING`           |

System packages, by platform. **nlohmann/json is the one that is not optional** — `qb::json` *is*
`nlohmann::json`, and qb stopped vendoring a copy in 3.0. A build without it still works (the pinned
`v3.12.0` is fetched), but producing an **installable** qb requires the real package; see
[Consume an installed copy](#consume-an-installed-copy).

```bash
# Debian / Ubuntu
sudo apt-get install nlohmann-json3-dev libssl-dev libargon2-dev zlib1g-dev

# macOS (Homebrew)
brew install nlohmann-json openssl argon2 zlib

# Windows (vcpkg) — as used in CI
vcpkg install nlohmann-json:x64-windows openssl:x64-windows argon2:x64-windows zlib:x64-windows
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

`qbConfig.cmake` calls `find_dependency(nlohmann_json 3.11)`, so **the consumer must have
nlohmann/json too** — `qb::json` is an alias for `nlohmann::json`, and the type crosses qb's API, so
both sides must resolve the same one. An absent package fails at `find_package(qb)` time, loudly.
For the same reason the build that *produces* the prefix must use a system nlohmann rather than the
fetched fallback: a fetched copy belongs to no export set, and installing its headers would put
`nlohmann/` into the consumer's include root. That combination is a configure-time error naming both
ways out. `<prefix>/include` therefore contains exactly `qb` on every host.

The install exports `qbConfig.cmake` and a `SameMajorVersion` version file. It also exports the
bundled `FindArgon2` / `FindNgtcp2` modules **when, and only when, the corresponding feature was
actually enabled in this build** — `FindArgon2.cmake` if `QB_HAS_ARGON2`, `FindNgtcp2.cmake` if
`QB_HAS_QUIC` (`CMakeLists.txt:292-297`). They travel with the package because `qbConfig.cmake`
calls `find_dependency(Argon2)` / `find_dependency(Ngtcp2)` by name and a consumer has no way to
invent them; a build with those features off needs neither.

### What was this prefix built with?

A prebuilt prefix records its own configuration in **`share/qb/abi-fingerprint.txt`**:

```
$ cat /your/prefix/share/qb/abi-fingerprint.txt
# qb ABI + configuration fingerprint of THIS installed prefix.
# ...
qb-abi qb=<version> cacheline=64 exceptions=1 coroutine_debug=0 std_jthread=1
cxx_standard=20 compiler=AppleClang-21.0.0.21000101 build_type=Release shared_libs=OFF
ssl=TRUE compression=TRUE quic=TRUE argon2=TRUE
unordered_map=ska nlohmann=3.12.0
```

The `qb-abi` line is read back **out of the installed archive**, not re-derived from CMake
variables, so it cannot drift from the artefact. Its five axes are enforced **at link time**: a
consumer compiled with a different value references a symbol this archive does not define and the
link fails naming the axis (see `src/qb/utility/abi.h`). The lines below it are *not* link-enforced
— they record what configure resolved, which is what a packager needs to check that a hermetic
build environment did not silently drop OpenSSL or zlib. Set **`-DQB_REQUIRE_FEATURES=ON`** to turn
any such silent downgrade into a configure-time error.

## Build from source

```bash
git clone --recursive https://github.com/isndev/qb.git
cd qb
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

A common production configuration keeps host-specific codegen off (which is already the default —
`QB_ENABLE_NATIVE_ARCH` is `OFF`, so the line below is a belt-and-braces assertion rather than a
change) and enables link-time optimization:

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DQB_ENABLE_NATIVE_ARCH=OFF \
      -DQB_ENABLE_LTO=ON \
      -DQB_WITH_SSL=ON -DQB_WITH_COMPRESSION=ON \
      -B build
```

See [production_checklist.md](./readme/6_guides/production_checklist.md) before shipping.

## Scaffolding a project or a module

Two bash scripts render the public templates ([`qb-sample-project`](https://github.com/isndev/qb-sample-project),
[`qb-sample-module`](https://github.com/isndev/qb-sample-module)) under the name you pass:

```bash
curl -fsSL https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh -o qb-new-project.sh
bash qb-new-project.sh MyProject          # a project: ./build/bin/MyProject, a ctest suite, a CI workflow
curl -fsSL https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-module.sh -o qb-new-module.sh
bash qb-new-module.sh mymodule            # a qbm module, run from your project's qbm/ directory
```

`MyProject` is substituted into the CMake project and target names, the C++ namespaces, the include
guards and the directory names, so nothing is renamed by hand; the name is validated first
(`[A-Za-z][A-Za-z0-9_-]*` for a project, `[a-z][a-z0-9_]*` for a module, which becomes the
`qbm-<name>` target and the `qbm::<name>` namespace) and anything else exits 2. Each script creates
nothing outside the directory it names, refuses to run if that name is taken, aborts on the first
failed step and removes what it made if it does not finish. The result is a fresh git repository with
one commit and no remote. Both are bash (`set -euo pipefail`) and shell out to `git` only: on Windows
run them from WSL or Git Bash; nothing else in qb's build needs a shell.

**Which qb the generated tree builds against is decided by the script, not stored in the template.**
Each script carries the version of the qb it ships with (`QB_SHIPPED_VERSION`, asserted equal to
`QB_FRAMEWORK_VERSION` in `cmake/qbConfig.cmake` by `scripts/check-scaffold-consistency.sh`) and
writes the matching `v<version>` tag as the qb ref of the generated `CMakeLists.txt`, which fetches
qb and the qbm modules with `FetchContent` at the first configure — no submodule to initialise. The
template revision resolves against the same version: an explicit `QB_TEMPLATE_REF` wins, then the
template's `v<version>` tag, then its default branch, reported as a fallback. The script prints the
template ref and the qb ref it wrote, and why. Two overrides exist: `QB_TEMPLATE_DIR=<path>` renders
from a local template checkout, `QB_REF=<ref>` overrides the qb ref written into the tree; and
`-DFETCHCONTENT_SOURCE_DIR_QB=/path/to/qb` at configure time builds the generated project against a
checkout you already have. The URL the script is fetched from selects the pairing:
`.../qb/main/script/...` is the released line, `.../qb/<tag>/script/...` pins one release.

A generated module ships its public headers under `src/qbm/<name>/`, a tiered `tests/` suite
registered with `qb_register_module_test()`, and a superbuild root that builds and runs those tests
with no further setup:

```bash
cmake -S .github/ci/superbuild -B build
cmake --build build --parallel --target qbm-mymodule-tests
ctest --test-dir build -L module:qbm-mymodule
```

A qbm module cannot be configured standalone — it calls `qb_register_module()` and `qb_add_test()`,
development-time helpers an installed qb does not ship — so `.github/ci/superbuild/CMakeLists.txt`
is the same answer qbm-http, qbm-pgsql and qbm-redis give: a minimal root that adds a qb *source*
tree first and the module second. The generated one defaults its paths and fetches qb, so it works
with no arguments on a machine that has no qb checkout.

## Troubleshooting

- **`CMake 3.24 or higher is required`** — upgrade CMake; the dependency resolution relies on it.
- **`libev … not found`** — the event loop lives directly under `qb/src/qb/ev` (committed files, not a
  submodule), so a normal clone always ships it. If it is missing, restore it from the repo
  (`git checkout -- src/qb/ev`) or re-clone; a `git submodule update` will not bring it back.
- **SSL features missing** — install OpenSSL development headers; without them `QB_WITH_SSL` is auto-disabled.
- **Host CPU binary fails on another machine** — check whether `QB_ENABLE_NATIVE_ARCH` was turned on.
  It is **`OFF` by default** (`cmake/qbConfig.cmake:140`) and every preset except `release-native` /
  `benchmarks` keeps it off, so a default build is already portable; if the cache says `ON`, rebuild
  with `-DQB_ENABLE_NATIVE_ARCH=OFF`. If it was already off, the illegal instruction is coming from
  somewhere else — look at your own flags before this one.
