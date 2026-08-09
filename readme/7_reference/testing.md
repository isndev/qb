# Testing the framework

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

How the qb test suite is organized, how to build and run it with CTest and GoogleTest, and how the coverage option is wired.

**Prerequisites:** [Building qb](./building.md), [CMake options](./cmake_options.md) — **See also:** [CMake and dependencies](./cmake_dependencies.md), [FAQ](./faq.md)

## Summary

qb ships its unit and integration tests as GoogleTest executables, registered with CTest by the `qb_add_test` CMake helper. Tests are built only when `QB_BUILD_TESTS` is `ON` (the repo-root build forces it on). GoogleTest is resolved system-first, with a pinned from-source fallback through FetchContent. Every registered test runs from `${CMAKE_BINARY_DIR}/bin/tests`, carries `tier:<tier>` and `module:<module>` CTest labels, and has a per-tier timeout (unit 60 s, system 120 s, integration 300 s). An optional `QB_BUILD_COVERAGE` flag adds lcov/gcovr report targets on Debug, non-Windows builds.

This page is for contributors building and running the suite, and for anyone adding a new test. It does not document the actor or I/O APIs the tests exercise — see the relevant reference pages for those.

## How the suite is organized

Test sources live under `tests/`, beside the `src/` include root rather than inside it, one subtree per library. The two are laid out as follows.

| Path | Contents |
| --- | --- |
| `qb/tests/core/unit/` | Focused unit tests, grouped by subject (`container/`, `core/`, `json/`, `lockfree/`, `patterns/`, `system/`, `type/`): one class or function in isolation. |
| `qb/tests/core/system/` | End-to-end actor-runtime tests, grouped by concern (`actor/`, `engine/`, `event/`, `messaging/`, `lifecycle/`, `coroutine/`, `concurrency/`, …): a real `qb::Main` across one or more cores. |
| `qb/tests/io/unit/` | qb-io unit tests, grouped by subject (`core/`, `coroutine/`, `crypto/`, `compression/`, `protocol/`, `ssl/`, `stream/`, …). |
| `qb/tests/io/system/` | qb-io integration tests, grouped by transport/feature (`tcp/`, `udp/`, `tls/`, `quic/`, `async/`, `session/`, …). |
| `qb/tests/{core,io}/benchmark/` | Performance benchmarks, built under `QB_BUILD_BENCHMARKS` (not `QB_BUILD_TESTS`). |
| `qb/tests/{core,io}/shared/` | Shared fixtures and helpers — not tests themselves. |

<!-- src: qb/tests/core, qb/tests/io -->

The distinction between unit and system is one of scope, not of mechanism — both are GoogleTest executables and both register through the same `qb_add_test` helper.

- **Unit tests** isolate one class or function with minimal dependencies. Examples: `unit/system/time.cpp` exercises `qb::duration` / `qb::mono_time` / `qb::wall_time`; `unit/container/string.cpp` exercises the string utilities; `unit/system/event-router.cpp` exercises event routing.
- **System/integration tests** verify several components working together. A typical core system test instantiates `qb::Main`, adds actors to one or more `VirtualCore`s, runs the engine, and asserts on the collected results and on `Main::hasError()`.

Both libraries use the same three tiers — `unit/`, `system/`, and `benchmark/` — each split into topic subdirectories. Coroutine coverage is a subject group inside `unit/` and `system/` (tagged with the `coroutine` label), not a separate tier.

### Naming and target conventions

Test sources sit in a topic subdirectory of their tier and are named for the subject under test — `actor/actor-add.cpp`, `core/uri-parse.cpp`, `system/time.cpp` — with no `test-` prefix. Each is registered with one `qb_add_test` call naming the module, tier, and short subject:

```cmake
qb_add_test(MODULE qb-core TIER system NAME actor-add SOURCES actor/actor-add.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io   TIER unit   NAME uri-parse SOURCES core/uri-parse.cpp  DEPENDS ${PROJECT_NAME})
```

The helper derives both the executable and the CTest entry name uniformly as `<module>-test-<tier>-<name>`:

| Source | `qb_add_test` arguments | Target / CTest name |
| --- | --- | --- |
| `core/tests/system/actor/actor-add.cpp` | `MODULE qb-core TIER system NAME actor-add` | `qb-core-test-system-actor-add` |
| `core/tests/unit/system/time.cpp` | `MODULE qb-core TIER unit NAME time` | `qb-core-test-unit-time` |
| `io/tests/unit/core/uri-parse.cpp` | `MODULE qb-io TIER unit NAME uri-parse` | `qb-io-test-unit-uri-parse` |
| `io/tests/system/coroutine/channel-lifetime.cpp` | `MODULE qb-io TIER system NAME channel-lifetime` | `qb-io-test-system-channel-lifetime` |

<!-- src: qb/cmake/qbFunctions.cmake:436-443, qb/tests/io/unit/CMakeLists.txt:27 -->

Every test also carries CTest labels — `tier:<tier>` and `module:<module>`, plus any capability tokens from `REQUIRES` (`ssl`, `quic`, `compression`, `network`, `live`) — and a per-tier default timeout (unit 60 s, system 120 s, integration 300 s). To add a test, drop the source into the right tier/topic directory and add one `qb_add_test` line; there are no per-directory naming rules to remember.

## Building the tests

Tests compile as part of the standard build when `QB_BUILD_TESTS` is `ON`. The option defaults to `ON` (`qb/cmake/qbConfig.cmake:86`, with the default computed at `74-87`), and the repository root forces it on for the development build (`qb-dev/CMakeLists.txt:38`).

```bash
# From the repository root
cmake -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_TESTS=ON -B build
cmake --build build --parallel
```

<!-- src: qb/readme/7_reference/building.md -->

When `QB_BUILD_TESTS` is `OFF`, `qb_add_test` returns early before defining any target, so no test executables and no CTest registrations exist (`qb/cmake/qbFunctions.cmake:444-447`).

Test executables are written to `${CMAKE_BINARY_DIR}/bin/tests` (`qb/cmake/qbFunctions.cmake:532-534`), not alongside the per-module build trees. After a build, list them with:

```bash
ls build/bin/tests/
```

### How GoogleTest is resolved

`qb_add_test` links each test against `GTest::gtest_main` (`qb/cmake/qbFunctions.cmake:494-495`), which provides the `main()` entry point — test sources do not declare their own. GoogleTest itself is resolved once, before any test target is defined, by `qb/cmake/qbFetchGoogleDeps.cmake`, only when `QB_BUILD_TESTS` (or `QB_BUILD_BENCHMARKS`) is on. The policy is:

- **`QB_USE_SYSTEM_GTEST=ON`** (default `OFF`) — require a system package via `find_package(GTest CONFIG REQUIRED)`; never fetch.
- **`QB_DEPS_FETCH_FALLBACK=ON`** (the default) — use a system GoogleTest if `find_package` locates one, otherwise build the pinned tag from source through FetchContent ("system if present, else git"). The from-source path needs network access on the first configure.
- **`QB_DEPS_FETCH_FALLBACK=OFF`** — always build the pinned tag from source, ignoring any system copy.

The pinned tag is `QB_GOOGLETEST_GIT_TAG`, default `v1.15.2` (`qb/cmake/qbConfig.cmake:105`, marked advanced at `108`). FetchContent is configured with `BUILD_GMOCK=ON` and `INSTALL_GTEST=OFF`. See [CMake and dependencies](./cmake_dependencies.md) for the full dependency-resolution model.

### Test resources

If OpenSSL is available (`QB_HAS_SSL`), `qb_setup_test_resources` registers a `qb_copy_test_ssl_resources` target that copies the SSL fixture directory into `build/bin/tests/ssl` (`qb/cmake/qbFunctions.cmake:1143-1164`). SSL-dependent qb-io tests additionally depend on a `generate_ssl_certs` target that produces a self-signed certificate. Because tests look up resources relative to their working directory, they must be launched from `bin/tests` — CTest sets that working directory automatically (`qb/cmake/qbFunctions.cmake:587-589`).

### Conditional suites

Some qb-io suites exist only when their optional dependency is present:

- **Crypto tests** (the `unit/crypto/` group — `crypto-primitives`, `crypto-jwt`, `kdf-and-tokens`, `asymmetric-keys`, … — registered with `REQUIRES ssl`) build only under `QB_HAS_SSL` (OpenSSL).
- **Compression tests** (`unit/compression/compression-codec`, registered with `REQUIRES compression`) build only under `QB_HAS_COMPRESSION` (zlib).

<!-- src: qb/tests/io/unit/CMakeLists.txt:64-71 -->

Without the corresponding library these suites are not configured at all — they do not appear as skipped, they do not exist. The QUIC suites are gated the same way, by `REQUIRES quic ssl network` (`qb/tests/io/system/CMakeLists.txt:88-89`), so they too are simply not registered without QUIC; their *cases* additionally carry a `QB_HAS_QUIC` `#ifdef` so the file still asserts the negative contract when it is compiled without it (`qb/tests/io/system/quic/quic-handshake.cpp:86-98`). Some multi-core core tests also require a host with more than one hardware thread; the multi-core cases skip when only one core is available (`qb/tests/core/system/engine/main-lifecycle.cpp:179-182`).

### Running under sanitizers

To build and run the suite instrumented, configure with a sanitizer preset rather than a plain Debug build: `cmake --preset sanitize` for AddressSanitizer + UndefinedBehaviorSanitizer, or `cmake --preset sanitize-thread` for ThreadSanitizer. The `QB_SANITIZE` flag (default empty) applies its sanitizer list to every qb, qbm, and test target plus the link step, regardless of `CMAKE_BUILD_TYPE`. The GitHub Actions `sanitize` and `sanitize-thread` workflows run these presets on Ubuntu with Clang. See `QB_SANITIZE` in [CMake options](./cmake_options.md).

## Running the tests

`enable_testing()` is invoked once at the repository root (`qb-dev/CMakeLists.txt:74`), not inside qb, so CTest discovers the whole tree. There are two ways to run tests.

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
ctest -R messaging          # the messaging suites (messaging-api, …)

# Select by label
ctest -L coroutine          # every test tagged 'coroutine'
ctest -L tier:unit          # every unit-tier test
ctest -L module:qb-io       # every qb-io test
```

Every test registered by `qb_add_test` carries `tier:<tier>` and `module:<module>` labels (plus any capability tags such as `ssl` or `coroutine`) and a per-tier timeout (unit 60 s, system 120 s, integration 300 s), and runs with its working directory set to `bin/tests` (`qb/cmake/qbFunctions.cmake:587-589` for the working directory, `588-597` for the labels, timeout, resource locks and skip regex). The `-R` regular expression matches the CTest test name (which equals the target name from the table above); `-L` matches labels.

### Running an executable directly

Running a test binary directly exposes GoogleTest's own command-line flags. Launch it from `bin/tests` so it can find its resources:

```bash
cd build/bin/tests

# Run one suite, colorized
./qb-core-test-system-actor-add --gtest_color=yes

# Run a single test case within that executable
./qb-core-test-system-actor-add --gtest_filter='*SpecificCase*'

# List the cases an executable contains
./qb-core-test-system-actor-add --gtest_list_tests
```

See the GoogleTest documentation for the full flag set.

## Writing a test

Test cases use the standard GoogleTest macros. A unit test includes `<gtest/gtest.h>` and the headers under test, then declares `TEST(Suite, Case)` or, with a fixture, `TEST_F(Fixture, Case)`. No `main()` is needed — `GTest::gtest_main` supplies it.

```cpp
// A focused unit test.
#include <gtest/gtest.h>
#include <qb/system/time.h>

TEST(Duration, DefaultAndExplicit) {
    qb::duration d{};
    EXPECT_EQ(d.count(), 0);
    // ... the same case then checks an explicitly-constructed duration.
}
```

<!-- src: qb/tests/core/unit/system/time.cpp:64-72 -->

A system test drives the actor runtime. The common pattern is `start()` then `join()`: `Main::start(bool async = true)` defaults to `async = true`, spawning worker threads and returning immediately, after which `join()` blocks until every core has stopped (`qb/src/qb/core/Main.h:536,561`). Passing `start(false)` instead turns the calling thread into a worker and blocks inline until the engine stops (`qb/src/qb/core/Main.h:536`, `qb/src/qb/core/Main.cpp:414-419`). Either way, collect results into an `std::atomic` (or a response event) and assert after the run completes, including on `Main::hasError()`.

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
    qb::io::async::task<bool> onInit() final {
        registerEvent<Ping>(*this);
        push<Ping>(id());   // send to self
        co_return true;
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

<!-- src: qb/tests/core/system/event/service-event-ring.cpp:171-174 -->

`Main::addActor<A>(core_id, args...)` is a convenience equivalent to `core(core_id).addActor<A>(args...)`; both return an `ActorId` (`qb/src/qb/core/Main.h:217,582`). For staged work inside a test — sequencing steps or waiting on a condition — schedule continuations with `qb::io::async::callback` from within the actors rather than sleeping in the test thread.

### Registering the test with CMake

Add one `qb_add_test` call to the `CMakeLists.txt` of the tier you are adding to. Name the module, tier, short subject, and the source path (relative to the tier directory); follow the surrounding lines in that file:

```cmake
qb_add_test(MODULE qb-core TIER unit NAME my-feature SOURCES core/my-feature.cpp DEPENDS ${PROJECT_NAME})
```

That registers the target and CTest entry as `qb-core-test-unit-my-feature`. Optional dependencies gate through `REQUIRES` (for example `REQUIRES ssl` builds the case only under `QB_HAS_SSL`); extra CTest labels go through `LABELS`.

<!-- src: qb/tests/core/unit/CMakeLists.txt:25-37 -->

`qb_add_test` links `GTest::gtest_main` for you; do not list `gtest_main` under `DEPENDS` as well (the helper strips a duplicate to avoid a linker warning, but listing it is redundant). Reconfigure CMake after editing the file, then rebuild.

## Coverage

`QB_BUILD_COVERAGE` (default `OFF`, `qb/cmake/qbConfig.cmake:144`) enables coverage instrumentation. It applies only when **all** of these hold: `CMAKE_BUILD_TYPE` is `Debug`, the platform is **not** Windows, and `lcov` plus `gcov` are found on the system (`qb/CMakeLists.txt:146-154`). When any condition fails, the build proceeds without coverage and emits a warning if the tools are missing.

When enabled, three report targets are registered, each driven by running `ctest`:

| Target | Tool | Output |
| --- | --- | --- |
| `qb-coverage` | lcov | lcov `.info` report |
| `qb-coverage-xml` | gcovr | Cobertura XML |
| `qb-coverage-html` | gcovr | HTML report |

<!-- src: qb/CMakeLists.txt:194-216, qb/cmake/CodeCoverage.cmake -->

The exclusion list filters out system headers, benchmarks, modules, the vendored forks, examples, and the test sources themselves so the report reflects framework code (`qb/CMakeLists.txt:170-178`). A typical run:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DQB_BUILD_COVERAGE=ON -B build
cmake --build build --parallel
cmake --build build --target qb-coverage-html
```

The instrumentation flags (`-g -fprofile-arcs -ftest-coverage`, plus `--coverage` on GCC) are GCC/gcov-oriented; the targets `FATAL_ERROR` at configure time if their tool is absent.

## Pitfalls

- **No tests in a tests-off build.** With `QB_BUILD_TESTS=OFF`, `qb_add_test` returns before creating anything — there are no executables and nothing for CTest to discover. The repo-root build forces the option on, so this only bites custom configurations.
- **Run from the right directory.** Tests resolve resources (SSL certs, fixtures) relative to `bin/tests`. CTest sets this automatically; if you launch a binary by hand, `cd build/bin/tests` first or resource-dependent cases fail.
- **Optional suites are absent or skipped, depending on the dependency.** Crypto and compression suites are not configured without OpenSSL / zlib — those targets do not exist at all, so they cannot pass. The QUIC suites are gated the same way (`REQUIRES quic ssl network`), so they are absent without libngtcp2; the cases inside also carry a `QB_HAS_QUIC` `#ifdef` for the builds where the file is compiled. Either way, an absent or skipped suite means a missing dependency, not a passing run. Confirm which features were enabled at configure time before reading a green result as full coverage.
- **Coverage is narrow.** `QB_BUILD_COVERAGE` works only on Debug, non-Windows, with lcov + gcov present, and the GCC/gcov toolchain. It is not a general-purpose option across all build types.
- **Coroutine tests must clean up the per-thread state.** Async coroutine fixtures call `qb::io::async::init()` in `SetUp` and reset state in `TearDown`. The minimum is `qb::io::async::listener::current.clear()` (`qb/tests/io/system/async/callback-dispatch.cpp:56-63`, whose `reset_async_context()` is an intent-named alias of `qb::io::async::init()`); fixtures that spawn coroutines drain the scheduler first — `run_for(5ms)` then `reset_coro_scheduler()` then `clear()` — to avoid leaking suspended coroutine frames across tests (`qb/tests/io/unit/coroutine/scope-structured-concurrency.cpp:58-63`). Follow the existing fixture pattern in the file you are adding to.
- **CI coverage is split across workflows.** The cross-platform CMake workflow builds Release. Dedicated Ubuntu workflows run ASan/UBSan, TSan, coverage, and changed-file format checks. Clang-tidy is intentionally script-driven through `scripts/clang-tidy.sh` rather than a CMake workflow. Linux jobs install libngtcp2 through apt and require the OpenSSL crypto helper (`libngtcp2-crypto-ossl-dev`); when it is absent the install does not pull the GnuTLS helper (qb has no GnuTLS backend) and QUIC simply stays auto-disabled (`QB_WITH_QUIC=AUTO`).

## See also

- [Building qb](./building.md) — configure presets, build types, toolchains.
- [CMake options](./cmake_options.md) — every `QB_*` flag, including `QB_BUILD_TESTS`, `QB_BUILD_COVERAGE`, `QB_USE_SYSTEM_GTEST`, and `QB_SANITIZE`.
- [CMake and dependencies](./cmake_dependencies.md) — how GoogleTest and the other dependencies are resolved.
- [Glossary](./glossary.md) — definitions of system test, unit test, regression test, and the tier/module CTest labels.
