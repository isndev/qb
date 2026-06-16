# Testing the framework

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

How the qb test suite is organized, how to build and run it with CTest and GoogleTest, and how the coverage option is wired.

**Prerequisites:** [Building qb](./building.md), [CMake options](./cmake_options.md) — **See also:** [CMake and dependencies](./cmake_dependencies.md), [FAQ](./faq.md)

## Summary

qb ships its unit and integration tests as GoogleTest executables, registered with CTest by the `qb_add_test` CMake helper. Tests are built only when `QB_BUILD_TESTS` is `ON` (the repo-root build forces it on). GoogleTest is resolved system-first, with a pinned from-source fallback through FetchContent. Every registered test runs from `${CMAKE_BINARY_DIR}/bin/tests`, carries the CTest label `qb-tests`, and has a 300-second timeout. An optional `QB_BUILD_COVERAGE` flag adds lcov/gcovr report targets on Debug, non-Windows builds.

This page is for contributors building and running the suite, and for anyone adding a new test. It does not document the actor or I/O APIs the tests exercise — see the relevant reference pages for those.

## How the suite is organized

Test sources live under each component's `tests/` directory. The two libraries are laid out as follows.

| Path | Contents |
| --- | --- |
| `qb/source/core/tests/unit/` | Focused unit tests for single components (event router, the canonical time vocabulary, string utilities). |
| `qb/source/core/tests/system/` | End-to-end actor-runtime tests that spin up a real `qb::Main` engine across one or more cores. |
| `qb/source/io/tests/system/` | qb-io integration tests: URI parsing, TCP/UDP/Unix sockets, asynchronous I/O, plus optional crypto and compression suites. |
| `qb/source/io/tests/coroutine/` | Tests for the C++23 coroutine runtime layered on libev (`task<T>`, generators, channels, scopes). |

<!-- src: qb/source/core/tests, qb/source/io/tests -->

The distinction is one of scope, not of mechanism — both kinds are GoogleTest executables.

- **Unit tests** isolate one class or function with minimal dependencies. Examples: `test-timestamp.cpp` exercises `qb::duration` / `qb::mono_time` / `qb::wall_time`; `test-string.cpp` exercises the string utilities; `test-event-router.cpp` exercises event routing.
- **System/integration tests** verify several components working together. A typical core system test instantiates `qb::Main`, adds actors to one or more `VirtualCore`s, runs the engine, and asserts on the collected results and on `Main::hasError()`.

Note the asymmetry: qb-core has both `unit/` and `system/` directories, while qb-io has `system/` and `coroutine/` but no `unit/` directory.

### Naming and target conventions

Test source files are named `test-<feature>.cpp` for core and qb-io system tests (for example `test-actor-event.cpp`, `test-uri.cpp`). Coroutine sources use underscores: `test_<feature>.cpp` (for example `test_coroutine_regression.cpp`).

CMake derives the executable name from the source file, prefixed with the owning project. Because the prefixes differ per directory, so do the resulting target names:

| Source | CMake target (and ctest name) |
| --- | --- |
| `core/tests/system/test-actor-event.cpp` | `qb-core-gtest-system-test-actor-event` |
| `core/tests/unit/test-timestamp.cpp` | `qb-core-gtest-unit-timestamp` |
| `io/tests/system/test-async-io.cpp` | `qb-io-gtest-test-async-io` |
| `io/tests/coroutine/test_basic.cpp` | `qb-io-gtest-coroutine-basic` |

<!-- src: qb/source/core/tests/system/CMakeLists.txt:66-79, qb/source/io/tests/system/CMakeLists.txt:46-55, qb/source/io/tests/coroutine/CMakeLists.txt:30-34 -->

The core system and unit CMake files follow a `gtest-system-<file>` / `gtest-unit-<name>` scheme; the qb-io system file uses `gtest-<file>`; the coroutine file lists each target by hand as `gtest-coroutine-<name>`. When you grep for a test executable, match on the feature fragment rather than assuming a single prefix.

## Building the tests

Tests compile as part of the standard build when `QB_BUILD_TESTS` is `ON`. The option defaults to `ON` (`qb/cmake/qbConfig.cmake:61`), and the repository root forces it on for the development build (`qb-dev/CMakeLists.txt:14`).

```bash
# From the repository root
cmake -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_TESTS=ON -B build
cmake --build build --parallel
```

<!-- src: qb/readme/7_reference/building.md -->

When `QB_BUILD_TESTS` is `OFF`, `qb_add_test` returns early before defining any target, so no test executables and no CTest registrations exist (`qb/cmake/qbFunctions.cmake:251`).

Test executables are written to `${CMAKE_BINARY_DIR}/bin/tests` (`qb/cmake/qbFunctions.cmake:299`), not alongside the per-module build trees. After a build, list them with:

```bash
ls build/bin/tests/
```

### How GoogleTest is resolved

`qb_add_test` links each test against `GTest::gtest_main` (`qb/cmake/qbFunctions.cmake:261-267`), which provides the `main()` entry point — test sources do not declare their own. GoogleTest itself is resolved once, before any test target is defined, by `qb/cmake/qbFetchGoogleDeps.cmake`, only when `QB_BUILD_TESTS` (or `QB_BUILD_BENCHMARKS`) is on. The policy is:

- **`QB_USE_SYSTEM_GTEST=ON`** (default `OFF`) — require a system package via `find_package(GTest CONFIG REQUIRED)`; never fetch.
- **`QB_DEPS_FETCH_FALLBACK=ON`** (the default) — use a system GoogleTest if `find_package` locates one, otherwise build the pinned tag from source through FetchContent ("system if present, else git"). The from-source path needs network access on the first configure.
- **`QB_DEPS_FETCH_FALLBACK=OFF`** — always build the pinned tag from source, ignoring any system copy.

The pinned tag is `QB_GOOGLETEST_GIT_TAG`, default `v1.15.2` (`qb/cmake/qbConfig.cmake:77`, marked advanced). FetchContent is configured with `BUILD_GMOCK=ON` and `INSTALL_GTEST=OFF`. See [CMake and dependencies](./cmake_dependencies.md) for the full dependency-resolution model.

### Test resources

If OpenSSL is available (`QB_HAS_SSL`), `qb_setup_test_resources` registers a `qb_copy_test_ssl_resources` target that copies the SSL fixture directory into `build/bin/tests/ssl` (`qb/cmake/qbFunctions.cmake:600-625`). SSL-dependent qb-io tests additionally depend on a `generate_ssl_certs` target that produces a self-signed certificate. Because tests look up resources relative to their working directory, they must be launched from `bin/tests` — CTest sets that working directory automatically (`qb/cmake/qbFunctions.cmake:330-333`).

### Conditional suites

Some qb-io suites exist only when their optional dependency is present:

- **Crypto tests** (`test-crypto*`, `test-crypto-jwt`) build only under `QB_HAS_SSL` (OpenSSL).
- **Compression tests** (`test-compression`, `test-compression-levels`) build only under `QB_HAS_COMPRESSION` (zlib).

<!-- src: qb/source/io/tests/system/CMakeLists.txt:79-210 -->

Without the corresponding library these suites are not configured at all — they do not appear as skipped, they do not exist. QUIC is different: `test-quic.cpp` is always built (it is registered unconditionally), but its cases call `GTEST_SKIP()` at runtime when `QB_HAS_QUIC` is undefined, so they report as skipped rather than absent (`qb/source/io/tests/system/test-quic.cpp:727-728`). Some multi-core core tests also require a host with more than one hardware thread; the multi-core cases in `test-main.cpp` check `EXPECT_GT(max_core, 1u)` (`qb/source/core/tests/system/test-main.cpp:73`).

### Running under sanitizers

To build and run the suite instrumented, configure with a sanitizer preset rather than a plain Debug build: `cmake --preset sanitize` for AddressSanitizer + UndefinedBehaviorSanitizer, or `cmake --preset sanitize-thread` for ThreadSanitizer. The `QB_SANITIZE` flag (default empty) applies its sanitizer list to every qb, qbm, and test target plus the link step, regardless of `CMAKE_BUILD_TYPE`. See `QB_SANITIZE` in [CMake options](./cmake_options.md).

## Running the tests

`enable_testing()` is invoked once at the repository root (`qb-dev/CMakeLists.txt:20`), not inside qb, so CTest discovers the whole tree. There are two ways to run tests.

### With CTest

CTest is the recommended driver for CI and for the full suite. Run it from the build directory:

```bash
cd build

# Run every discovered test
ctest

# Run in parallel across N jobs
ctest -j8

# Verbose output (per-case results and any stdout from failing tests)
ctest -V

# Run only tests whose name matches a regular expression
ctest -R coroutine          # every coroutine test
ctest -R test-actor-event   # the actor-event suite

# Select the framework's whole test corpus by label
ctest -L qb-tests
```

Every test registered by `qb_add_test` carries the label `qb-tests` and a 300-second timeout, and runs with its working directory set to `bin/tests` (`qb/cmake/qbFunctions.cmake:330-340`). The `-R` regular expression matches the CTest test name, which equals the target name from the table above.

### Running an executable directly

Running a test binary directly exposes GoogleTest's own command-line flags. Launch it from `bin/tests` so it can find its resources:

```bash
cd build/bin/tests

# Run one suite, colorized
./qb-core-gtest-system-test-actor-event --gtest_color=yes

# Run a single test case within that executable
./qb-core-gtest-system-test-actor-event --gtest_filter='*SpecificCase*'

# List the cases an executable contains
./qb-core-gtest-system-test-actor-event --gtest_list_tests
```

See the GoogleTest documentation for the full flag set.

## Writing a test

Test cases use the standard GoogleTest macros. A unit test includes `<gtest/gtest.h>` and the headers under test, then declares `TEST(Suite, Case)` or, with a fixture, `TEST_F(Fixture, Case)`. No `main()` is needed — `GTest::gtest_main` supplies it.

```cpp
// A focused unit test.
#include <gtest/gtest.h>
#include <qb/system/timestamp.h>

TEST(Duration, DefaultIsZero) {
    qb::duration d{};
    EXPECT_EQ(d.count(), 0);
}
```

<!-- src: qb/source/core/tests/unit/test-timestamp.cpp:23-39 -->

A system test drives the actor runtime. The common pattern is `start()` then `join()`: `Main::start(bool async = true)` defaults to `async = true`, spawning worker threads and returning immediately, after which `join()` blocks until every core has stopped (`qb/include/qb/core/Main.h:487,512`). Passing `start(false)` instead turns the calling thread into a worker and blocks inline until the engine stops (`qb/include/qb/core/Main.h:483-484`). Either way, collect results into an `std::atomic` (or a response event) and assert after the run completes, including on `Main::hasError()`.

```cpp
// A minimal actor system test.
#include <gtest/gtest.h>
#include <atomic>
#include <qb/main.h>
#include <qb/actor.h>

namespace {
std::atomic<int> g_seen{0};

struct Ping : qb::Event {};

class Worker : public qb::Actor {
public:
    bool onInit() final {
        registerEvent<Ping>(*this);
        push<Ping>(id());   // send to self
        return true;
    }
    void on(const Ping &) {
        ++g_seen;
        kill();             // work done; stop this actor
    }
};
} // namespace

TEST(WorkerSuite, HandlesPing) {
    g_seen = 0;

    qb::Main engine;
    engine.addActor<Worker>(0);   // core 0; equivalent to core(0).addActor<Worker>()

    engine.start();               // spawn worker threads (async, the default)
    engine.join();                // block until all actors stop

    EXPECT_FALSE(engine.hasError());
    EXPECT_EQ(g_seen.load(), 1);
}
```

<!-- src: qb/source/core/tests/system/test-actor-event.cpp:309-313 -->

`Main::addActor<A>(core_id, args...)` is a convenience equivalent to `core(core_id).addActor<A>(args...)`; both return an `ActorId` (`qb/include/qb/core/Main.h:216,533`). For staged work inside a test — sequencing steps or waiting on a condition — schedule continuations with `qb::io::async::callback` from within the actors rather than sleeping in the test thread.

### Registering the test with CMake

Add the source to the relevant `CMakeLists.txt` so it becomes a target. Core and qb-io system tests append the filename to a source list that a `foreach` loop feeds into `qb_add_test`; unit and coroutine files are registered with an explicit `qb_add_test` call. Follow the surrounding pattern in the directory you are adding to. A direct registration looks like:

```cmake
qb_add_test(
    NAME    ${PROJECT_NAME}-gtest-unit-my-feature
    SOURCES test-my-feature.cpp
    DEPENDS ${PROJECT_NAME}
)
```

<!-- src: qb/source/core/tests/unit/CMakeLists.txt:26-30 -->

`qb_add_test` links `GTest::gtest_main` for you; do not list `gtest_main` under `DEPENDS` as well (the helper strips a duplicate to avoid a linker warning, but listing it is redundant). Reconfigure CMake after editing the file, then rebuild.

## Coverage

`QB_BUILD_COVERAGE` (default `OFF`, `qb/cmake/qbConfig.cmake:97`) enables coverage instrumentation. It applies only when **all** of these hold: `CMAKE_BUILD_TYPE` is `Debug`, the platform is **not** Windows, and `lcov` plus `gcov` are found on the system (`qb/CMakeLists.txt:119-157`). When any condition fails, the build proceeds without coverage and emits a warning if the tools are missing.

When enabled, three report targets are registered, each driven by running `ctest`:

| Target | Tool | Output |
| --- | --- | --- |
| `qb-coverage` | lcov | lcov `.info` report |
| `qb-coverage-xml` | gcovr | Cobertura XML |
| `qb-coverage-html` | gcovr | HTML report |

<!-- src: qb/CMakeLists.txt:137-153, qb/cmake/CodeCoverage.cmake -->

The exclusion list filters out system headers, benchmarks, modules, examples, and the test sources themselves so the report reflects framework code. A typical run:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_COVERAGE=ON -B build
cmake --build build --parallel
cmake --build build --target qb-coverage-html
```

The instrumentation flags (`-g -fprofile-arcs -ftest-coverage`, plus `--coverage` on GCC) are GCC/gcov-oriented; the targets `FATAL_ERROR` at configure time if their tool is absent.

## Pitfalls

- **No tests in a tests-off build.** With `QB_BUILD_TESTS=OFF`, `qb_add_test` returns before creating anything — there are no executables and nothing for CTest to discover. The repo-root build forces the option on, so this only bites custom configurations.
- **Run from the right directory.** Tests resolve resources (SSL certs, fixtures) relative to `bin/tests`. CTest sets this automatically; if you launch a binary by hand, `cd build/bin/tests` first or resource-dependent cases fail.
- **Optional suites are absent or skipped, depending on the dependency.** Crypto and compression suites are not configured without OpenSSL / zlib — those targets do not exist at all, so they cannot pass. QUIC is gated differently: `test-quic.cpp` is always built, but its cases `GTEST_SKIP()` when libngtcp2 was not found (`QB_HAS_QUIC` undefined), so they show as skipped. Either way, an absent or skipped suite means a missing dependency, not a passing run. Confirm which features were enabled at configure time before reading a green result as full coverage.
- **Coverage is narrow.** `QB_BUILD_COVERAGE` works only on Debug, non-Windows, with lcov + gcov present, and the GCC/gcov toolchain. It is not a general-purpose option across all build types.
- **Coroutine tests must clean up the per-thread state.** Async coroutine fixtures call `qb::io::async::init()` in `SetUp` and reset state in `TearDown`. The minimum is `qb::io::async::listener::current.clear()` (`qb/source/io/tests/coroutine/test_coroutine_channel.cpp:32-34`); fixtures that spawn coroutines drain the scheduler first — `run_for(5ms)` then `reset_coro_scheduler()` then `clear()` — to avoid leaking suspended coroutine frames across tests (`qb/source/io/tests/coroutine/test_coroutine_scope.cpp:314-319`). Follow the existing fixture pattern in the file you are adding to.
- **CI runs Release only.** The CI workflow builds Release and does not exercise Debug, sanitizers, or coverage, and does not install libngtcp2 (so QUIC is auto-disabled on CI). Run sanitizer presets and coverage locally — do not assume CI exercised them.

## See also

- [Building qb](./building.md) — configure presets, build types, toolchains.
- [CMake options](./cmake_options.md) — every `QB_*` flag, including `QB_BUILD_TESTS`, `QB_BUILD_COVERAGE`, `QB_USE_SYSTEM_GTEST`, and `QB_SANITIZE`.
- [CMake and dependencies](./cmake_dependencies.md) — how GoogleTest and the other dependencies are resolved.
- [Glossary](./glossary.md) — definitions of system test, unit test, regression test, and the `qb-tests` CTest label.
