# Testing the qb framework

This guide covers how qb / qbm tests and benchmarks are organized, how to run the lane you
want, and how to add a new test. The full design rationale lives in the monorepo audit
(`dev/tests-audit/_CONVENTIONS.md` — the authority); this file is the day-to-day reference and
is self-contained for a standalone `qb` checkout.

## Tiers

Every test belongs to exactly one **tier** — a directory under `<module>/tests/` and a
`tier:<name>` CTest label.

| tier | directory | binds to | daemon? | parallel-safe |
|---|---|---|---|---|
| **unit** | `unit/` | pure logic — codecs, parsers, traits, a coroutine scheduler driven with no socket | no | yes |
| **system** | `system/` | in-process event loop + kernel objects: fds, signals, a loopback `127.0.0.1` client+server, a local TLS/QUIC handshake, a `qb::Main` engine | no (in-process) | mostly (ephemeral ports) |
| **integration** | `integration/` | an **external daemon** (`postgres:5432`, `redis:6379`) — **qbm-pgsql / qbm-redis only** | **yes** | no (shared daemon) |
| **benchmark** | `benchmark/` | google-benchmark harness; daemon-free unless explicitly `live` | usually no | run alone |
| **shared** | `shared/` | header-only test support (fixtures, mocks, corpora) — no `TEST()` | — | — |

> `integration/` means "an external daemon must be running" — nothing else. A loopback
> client+server is **system**, not integration. This is what keeps a daemon-free CI lane green.

## Labels & how to run a lane

Targets carry `tier:<t>` + `module:<m>` automatically, plus feature tags (`ssl`, `quic`,
`http3`, `ws`, `compression`, `coroutine`, `network`, `live`, `slow`, `serial`, …).

```sh
# Fastest inner loop: pure-logic, every module, in parallel.
ctest -L 'tier:unit' --parallel "$(nproc)"

# The universal daemon-free gate (CI without postgres/redis). ALWAYS green offline.
ctest -LE 'live'

# One module, all tiers / just its unit lane.
ctest -L 'module:qbm-http'
ctest -L 'module:qbm-redis' -L 'tier:unit'

# Integration lane (needs the daemons up; auto-serialized via RESOURCE_LOCK).
ctest -L 'tier:integration'

# Exclude TLS/crypto on a QB_HAS_SSL=OFF build; exclude signal/serial from a parallel run.
ctest -LE 'ssl'
ctest -LE 'serial' --parallel "$(nproc)"
```

`-L` AND-combines repeated flags (regex per flag); `-LE` excludes. `tier:unit` and `-LE live`
are the two load-bearing lanes.

## Daemon policy: missing daemon SKIPS, never FAILS

A `REQUIRES live` test derives from the module's shared skip-not-fail fixture
(`qbm/redis/tests/shared/redis_integration_fixture.h`,
`qbm/pgsql/tests/shared/pg_integration_fixture.hpp`). The endpoint is env-overridable
(`REDIS_URI`, `QB_PG_DSN`); if the daemon is unreachable the fixture `GTEST_SKIP`s with the
sentinel `QBM_INTEGRATION_SKIP_DAEMON_UNREACHABLE`, which the CMake helper wires into CTest's
`SKIP_REGULAR_EXPRESSION` so the run reports **Skipped, not Failed**. Never `throw` / `ASSERT`
on a missing daemon.

## Adding a test

Tests are registered through **one** helper (in `qb/cmake/qbFunctions.cmake`) — never a raw
`add_executable` + `add_test`.

Core libraries (`qb-core`, `qb-io`):

```cmake
qb_add_test(
    MODULE   qb-io
    TIER     unit                 # unit | system | benchmark
    NAME     uri-parse            # → target qb-io-test-unit-uri-parse
    SOURCES  unit/net/uri-parse.cpp
    LABELS   coroutine            # extra feature tags (tier:/module: auto-added)
    # REQUIRES ssl|quic|compression|network|live   TIMEOUT <s>   RESOURCE_LOCK <name>
    # WINDOWS_EXCLUDE   (POSIX-only test: skipped, not built, on Windows)
)
```

qbm modules (`http`, `pgsql`, `redis`):

```cmake
qb_register_module_test(
    MODULE_NAME redis
    TIER        integration
    TEST_NAME   string-commands   # → target qbm-redis-test-integration-string-commands
    SOURCES     integration/commands/string-commands.cpp
    REQUIRES    live              # → live label + integration RESOURCE_LOCK + skip-guard
)

qb_register_module_benchmark(
    MODULE_NAME redis
    BENCH_NAME  resp-parse        # → target qbm-redis-bench-resp-parse
    SOURCES     benchmark/resp-parse.cpp
)
```

The helper injects `tier:`/`module:` labels, a label per `REQUIRES` token, the per-tier default
`TIMEOUT` (unit 60 / system 120 / integration 300 s), the IDE `FOLDER`, and the `-Werror`
toggle. `REQUIRES ssl|quic|compression` compile-gates on the matching `QB_HAS_*` feature
(absent → the target is silently not registered, never a build error).

### Naming

- Files: `<subject>-<aspect>.cpp` under the tier dir (drop the legacy `test-`/`bm-` prefix).
- Targets: `<module>-test-<tier>-<name>` / `<module>-bench-<name>`.

## Warnings as errors

```cmake
option(QB_TESTS_WERROR "Treat warnings as errors in test/benchmark targets" ${QB_CI})
```

Default **ON in CI** (`CI` env set), **OFF** for casual local builds. The per-phase "0-warning"
gate is verified with `-DQB_TESTS_WERROR=ON`.

## Standalone vs monorepo

Each module builds both inside the monorepo (qb added via `add_subdirectory`) and standalone
(`find_package(qb CONFIG REQUIRED)`). Module CMake resolves the dependency with the guard
`if(NOT TARGET qb::core) find_package(qb CONFIG REQUIRED) endif()` and links the namespaced
aliases (`qb::core`, `qb::io`, `qbm::http`, …), so the same `qb_add_test(...)` call works either
way.

## Quality bar (enforced during review)

- No wall-clock as an ordering oracle — use a bounded `pump_until(pred, deadline)` that fails
  loudly on timeout, not `sleep_for` + a `bool`.
- Ephemeral ports (`bind :0`), never fixed.
- Assert framework-observable truth, not a value the test itself set; no `x || !x`, no bare
  `SUCCEED()`/empty bodies, no `try/catch{}` that swallows a real failure.
- A coroutine/async test asserts its completion flag *after* the loop drains (no pass-if-never-run).
- Codec tests assert against external golden vectors, not `encode→decode` self-agreement.
- Throughput belongs in `benchmark/`, never in a ctest `EXPECT_LT(duration, …)` band.
