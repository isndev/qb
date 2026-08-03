# Changelog

All notable changes to qb are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to
[Semantic Versioning](https://semver.org/). See [VERSIONING.md](./VERSIONING.md) for the compatibility
policy.

## [Unreleased]

Tracks changes on the `develop` branch that are not yet part of a tagged release.
`QB_FRAMEWORK_VERSION` is **3.0.0** on this branch, so a build made from `develop` already reports
`3.0.0` while this section is still open; it becomes the `[3.0.0]` entry when that release is tagged.
The latest tag remains `v2.6.0`.

**This is a major release.** *Removed* below already lists two source-incompatible items
(`<cube.h>`, which left the installed public surface, and `std::to_string(const uuids::uuid &)`);
[VERSIONING.md](./VERSIONING.md) reserves removals that require source edits for a major release.
The vendored event loop's rename (`ev_*` → `qev_*`, `<qb/vendor/ev/ev++.h>` →
`<qb/vendor/qev/qev++.h>`) has now landed — see *Changed* below. Further structural breaks are
planned on this line and are **not yet landed**: moving the qbm public include prefixes
(`<http/...>` → `<qb/http/...>`, `<pgsql/...>` → `<qb/pgsql/...>`, `<redis/...>` →
`<qb/redis/...>`) and dropping the installed `.tpp` headers. Either requires a major bump on its own.

The qbm modules version in lockstep with the framework: `qbm-http`, `qbm-pgsql` and `qbm-redis` all
carry `3.0.0`. They are not standalone-configurable (they call `qb_register_module` / `qb_add_test`,
which an installed qb does not ship), so their version only ever means "the framework this was built
against" — and the include-prefix move above lands hardest in exactly those modules.

### Added

- **`find_package(qbm-<name>)` packages for the qbm modules.** `qb_register_module()` now emits the
  install and export rules that make a module consumable from an installed tree — one package per
  module (`qbm-http`, `qbm-pgsql`, `qbm-redis`), so a downstream project writes
  `find_package(qbm-http CONFIG REQUIRED)` → `qbm::http` and gets `find_dependency(qb)` resolved
  inside. Headers install under `<prefix>/include/qbm/<name>/`, package files under
  `<prefix>/lib/cmake/qbm-<name>/`, and the target is spelled `qbm::<name>` identically in the build
  tree and the install tree. New `qb_register_module()` arguments: `EXPORT_EXTRA_TARGETS` (bundled
  non-imported targets the module links `PUBLIC`, which must travel in the same export set),
  `CONFIG_DEPENDENCIES` (a template configured into `qbm-<name>Dependencies.cmake` and included
  before the targets file, where a module re-creates the ad-hoc `IMPORTED` targets its export set
  names by string), and `INSTALL_CMAKE_FILES` (extra `Find<Pkg>.cmake` modules shipped beside the
  package config). Because a module ships a prebuilt archive compiled against one specific qb, the
  generated config hard-fails at configure time on a qb version *or* feature-flag
  (`QB_HAS_SSL` / `QB_HAS_QUIC`) mismatch rather than deferring the skew to a compile error.
- `QBM_INSTALL` option (defaults to `QB_INSTALL`) gating the module install/export rules above. The
  two are never usefully split: an installed `qbm-http` whose `find_dependency(qb)` has nothing to
  find is not a package.
- **`qb_add_executable(... REQUIRES <token>...)`** — capability gating for executables, identical in
  vocabulary (`ssl`, `quic`, `compression`) and resolved by the same helper as `qb_add_test` /
  `qb_add_benchmark`. **This is a build gate**: a target whose requirement is unmet is not created at
  all, so callers can test `if(TARGET ...)`, instead of the target being left to fail the build.
- `qb-io-test-unit-crypto-nossl-core`, covering the OpenSSL-free members of `qb::crypto`.
  Deliberately ungated — every other crypto test is `REQUIRES ssl` and therefore unregistered without
  OpenSSL, so this is the only crypto coverage a `QB_WITH_SSL=OFF` build gets.

### Changed

- **The source tree is laid out around one rule: `src/` *is* the include root.** `include/` and
  `source/` are gone. What a consumer types after `#include <` is now exactly what is under `src/`,
  with each header's implementation beside it — `src/qb/core/Actor.h` next to `src/qb/core/Actor.cpp`,
  `src/qb/io/tcp/socket.h` next to `src/qb/io/tcp/socket.cpp` — and the vendored forks under
  `src/qb/vendor/`. Tests moved out of the libraries and now sit beside the include root at
  `tests/core/` and `tests/io/`, so a test header cannot be swept into the install.
  **No consumer-visible spelling changes**: `<qb/core/Actor.h>` is unchanged, and the installed tree
  is byte-for-byte what it was — `<prefix>/include/qb/…`. The in-tree path and the installed path are
  now the same string, which is the point: the build interface and the install interface can no
  longer drift apart. Affects only somebody who names qb's internal directories directly (a patch, a
  vendoring script, an `-I` into `qb/include`); `CMAKE_INSTALL_INCLUDEDIR`, `find_package(qb)` and
  `add_subdirectory(qb)` are unaffected. `QB_INCLUDE_DIR` now points at `<qb>/src` and
  `QB_SOURCE_DIR` at `<qb>/src/qb`.
- **The vendored libev fork is now `qev`: its own directory, header names, CMake target and C symbol
  space.** `qb/vendor/ev/` → `qb/vendor/qev/`, `ev.h`/`ev++.h` → `qev.h`/`qev++.h`
  (likewise `qev.c`, `qev_vars.h`, `qev_wrap.h`, the generated `qev_config.h` and the backend
  sources), so a consumer writes `#include <qb/vendor/qev/qev++.h>`. The CMake target `ev` is `qev`,
  `qb::ev` is `qb::qev`, `ev::ev` is `qev::qev`, and the archive is `libqev.a`. All 58 exported
  `ev_*` symbols — and the types and macros they are built from — are `qev_*`.

  This fixes a **silent** failure: qb's event loop and a system libev define the same 58 names, so
  linking both into one binary succeeded with **exit 0 and no diagnostic** and the winner was decided
  by command-line order alone. `-fvisibility=hidden` cannot close it, because `ev++.h`'s inline
  members put the undefined `ev_*` references in the *consumer's* object file; only renaming does.

  **Not renamed, deliberately:** the 24 `event_*` functions and libevent's `struct event` members
  (`ev_fd`, `ev_events`, `ev_callback`, …) — those *are* libevent's API, and `event.h`,
  `event_compat.h`, `event.c` keep their names for the same reason. So `libqev.a` exports 57 owned
  `qev_*` functions, `qev_default_loop_ptr`, and 24 unowned `event_*`: a chosen exposure that only
  matters to a binary that also links a real libevent. The `EV_*` macros (`EV_READ`, `EV_WRITE`,
  `EV_MULTIPLICITY`, the `QB_EV_*` CMake options) are unchanged, as is the `ev::` C++ namespace.
  Nothing was removed: every file in the fork is still there and still built.
- **`QB_VERSION_NUM` is computed from the version macros instead of being written out by hand.**
  `<qb/io/config.h>` had it frozen at `0x020600`, so the documented hex form of the version stayed on
  2.6.0 while `QB_FRAMEWORK_VERSION` moved. It is now
  `((QB_VERSION_MAJOR << 16) | (QB_VERSION_MINOR << 8) | QB_VERSION_PATCH)`, sourced from the same
  `cmake/qbConfig.cmake` values as every other version string.
- **`scripts/doc-lint.sh` validates the *value* of the `Verified-against:` markers**, not merely that
  they exist. Existence was all it ever checked, which is how 129 pages across this repo and the three
  qbm modules sat at `qb 2.6.0` through two version bumps without anything failing. The expected
  version is read from `cmake/qbConfig.cmake`; a version the script cannot determine is a hard stop
  (exit 2) rather than a skip, because a lint that quietly passes when it cannot find its expected
  value is indistinguishable from the unchecked marker it replaced. A missing marker remains a
  warning; a *wrong* one now fails.
- **CI workflows target `main` and `develop`.** `cmake`, `coverage`, `doc-lint`, `format-check`,
  `sanitize` and `sanitize-thread` all filtered on a `c++23` branch that no longer exists, so six of
  the seven workflows had silently stopped running; only `install-consume` was live. They now use the
  same `push: [main, develop]` / `pull_request: [main]` triggers it already had.
- **`<qb/io/crypto.h>` now compiles without OpenSSL.** It used to open with
  `#error "missing OpenSSL Library"`, which gated the *entire* file — including members of
  `qb::crypto` that call no OpenSSL API. The hex codec in particular is what the PostgreSQL `bytea`
  wire format is built on, so a cleartext (`QB_WITH_SSL=OFF`) build of `qbm-pgsql` could not compile
  at all. The gate is now per-member: `to_hex_string`, `hex_value`, `hex_to_string`, `xor_bytes` and
  `constant_time_compare` are always declared, and are defined in the new `crypto_core.cpp` compiled
  unconditionally; every OpenSSL-backed member is removed from the class in a no-SSL build, so misuse
  now fails at the **call site** (`no member named 'md5' in 'qb::crypto'`) rather than at this header.
- `<qb/io/crypto_jwt.h>` carries its own `#ifndef QB_HAS_SSL` → `#error`. JWT is HMAC, RSA and ECDSA
  all the way down and has no OpenSSL-free subset; the check moved here from `crypto.h`, which used
  to cover this case only as a side effect of gating everything.

### Removed

- **`<cube.h>`**, the legacy whole-framework umbrella header (guard `QB_QB_H`; just `qb/actor.h`,
  `qb/io.h` and `qb/main.h`). **This removes a name from the installed public surface.** It was dead —
  nothing in the tree ever included it — and it was the last generic top-level name in the installed
  include root, which is now exactly `qb/` (plus `nlohmann/` when qb falls back to its bundled copy);
  the `install-consume` CI job asserts that root. qb has no whole-framework convenience header:
  include the entry points directly (`qb/actor.h` + `qb/main.h` for qb-core, `qb/io.h` for qb-io).
- **`qb/io/system/sys__inet_compat.inl`**, and with it the `#if QB__HAS_NTOP` fallback branch in
  `qb/io/system/sys__socket.h` that declared `inet_ntop` / `inet_pton` for it. The bundled fallback
  was dead and unbuildable three times over: its `#include` resolved from no include path, it opened
  `qb::inet::ip::compat` rather than the `qb::io::inet::ip::compat` that declared the functions, and
  its only gate (`QB__HAS_NTOP == 0`) is reachable solely on a pre-Vista Windows target that cannot
  host C++20 — so the declarations could only ever have produced a link error. Every platform qb
  builds on provides both functions natively, and `qb::io::inet::ip::compat` is now a pure re-export.
  The file sat in the public header tree but was never shipped (qb's install rule matches `*.h`,
  `*.hpp` and `*.tpp` only). `QB__HAS_NTOP` itself is kept as a published feature-test macro.
- `std::to_string(const uuids::uuid &)` from the vendored stduuid header — a local addition, not
  upstream. Adding a *declaration* to namespace `std` is undefined behaviour ([namespace.std]/2
  permits only an explicit specialisation for a program-defined type, which is what the neighbouring
  `std::hash<uuids::uuid>` is), and it was never needed: `uuids::to_string` is found by ADL for an
  unqualified call, and every call site in the tree already spells it `uuids::to_string`.
  **Source-incompatible** for code that called `std::to_string(uuid)`.

### Fixed

- `<qb/uuid.h>` included `<qb/vendor/uuid/include/uuid.h>` *above* its own `QB_UUID_H` include guard,
  leaving the vendored include outside the guard it was meant to sit behind.

## [2.6.0] - 2026-06-29

### Added

- **Asynchronous actor initialization.** `Actor::onInit()` is now a coroutine
  (`qb::io::async::task<bool>`): an actor may `co_await` during init (sleep, `qb::ask` a peer, run a
  pattern). While a suspended `onInit()` is in flight the actor is **Activating** — inbound unicast
  business events are stashed and replayed FIFO once it becomes active (broadcasts and `KillEvent`
  still pass through), bounded by a configurable `qb::VirtualCore::activation_deadline_ns` (default
  5 s) that makes mutual-init deadlocks impossible. `~Actor()` is deferred while an init frame is in
  flight, so `this` is safe across a `co_await` in `onInit()`.
- **Phase-aware `ActorHandle<T>`.** `addRefActor<T>()` now returns `qb::ActorHandle<T>` (alias
  `RefActorHandle<T>`) with `id()` (valid immediately), `ready()`, `co_await ready_async(ctx)`, and a
  phase-aware `get()` that resolves the live actor only once active and never dangles. New
  `Actor::is_active()` (alive **and** activated) complements `is_alive()`.
- **Native request/response `ask`.** `qb::ask(ctx, target, event, timeout)` with `qb::AskEvent` /
  `qb::Request<Resp>`, `qb::answer(...)`, and `Actor::resolve_ask(e)`; a custom single-timer awaiter
  (response / timeout / scope-cancel) with no lingering helper tasks.
- **Patterns library** (`qb/patterns.h`, header-only free functions over the kernel): request,
  scatter (`ask_all` / `ask_any`), saga (`run_saga` + reverse-order compensation), resilience
  (`ask_retry`, `ask_guarded`, `CircuitBreaker`, `retry_policy`, `circuit_open_error`), routing
  (`WorkerPool`), pub/sub (`PubSub<Topic>`), supervision (`Supervisor`).
- **Actor-scoped coroutines.** `Actor::spawn()` (cancelled on kill via a per-actor cancellation
  scope) alongside `spawn_detached()` (renamed from `spawn_async`, runs to completion orphaned);
  `Actor::context()` exposes a cancellation-aware `ScopedCoroContext`.
- `qb::io::async::cancellation_token::remove_on_cancel(id)` (and `on_cancel` now returns a
  registration id) so cancellation-aware awaiters deregister on normal completion — a long-lived
  (actor-scope) token no longer accumulates callbacks across a `co_await ctx.sleep(...)` loop.
- `qb::LoopEvent` — per-loop-pass context (`now`, `iteration`) handed to the actor periodic tick,
  unifying the tick with the `on(Event&)` dispatch shape and keeping it forward-compatible.
- **Patterns enrichment.**
    - Quorum scatter: `qb::ask_quorum(ctx, targets, k, req, timeout)` resolves with the first **k** of
      N replies (the majority middle-ground between `ask_any` and `ask_all`); throws if the quorum
      becomes unreachable, and is cancel-on-kill.
    - Bounded scatter: `qb::ask_all(ctx, targets, req, timeout, max_in_flight)` caps in-flight asks
      via a true cancel-safe **sliding window** — fan out to many targets without overwhelming a
      downstream.
    - `qb::bulkhead` (failure isolation): bounds concurrent operations through a resource;
      `co_await bulkhead.enter(ctx)` returns an RAII slot, waiting cancellation-aware when full.
    - `qb::io::async::semaphore::acquire(cancellation_token)`: a **cancellation-aware** acquire — a
      kill while parked unwinds cleanly and retracts the queued claim (no permit leak), so the next
      waiter is served correctly. (Backs `bulkhead` and bounded `ask_all`.)
    - `qb::retry_policy::jitter` (in `[0, 1]`, default `0`): randomizes each `ask_retry` backoff over
      `[backoff*(1-jitter), backoff]` to desynchronize retry storms.
    - `qb::Supervisor`: killing the supervisor (a `KillEvent`) now tears down its children first (no
      orphans); and an optional `restart_window` turns `max_restarts` into a sliding-window intensity
      ("N restarts within T") instead of a lifetime-cumulative cap.
    - `qb::run_saga` compensation is cancellation-aware: a kill mid-rollback aborts the remaining
      compensations instead of spinning through them.
    - `qb::rate_limiter` (alias `qb::token_bucket`): a cancellation-aware token-bucket throttle
      (`acquire(ctx)` / `try_acquire(now)`) completing the resilience trio with `CircuitBreaker` and
      `ask_retry`.
    - Deadline budget: `qb::deadline` + `qb::deadline_in(ctx, dur)` / `qb::remaining(dl, ctx)` /
      `qb::ask_by(ctx, target, req, deadline)` — thread one absolute deadline through an ask chain to
      bound its *total* latency end-to-end (fails fast once the budget is spent).
    - **Coroutine discovery & liveness.** `co_await qb::ping(ctx, target, timeout)` → `bool` (targeted
      liveness) and `co_await qb::require<T>(ctx, timeout)` → `std::vector<ActorId>` (typed discovery
      within a window), with `qb::resolve_require(e)` for the asker's `on(RequireEvent&)`. An awaitable
      replacement for the legacy `Actor::require<...>()` + `on(RequireEvent&)` + `is<T>()` dance (which
      still works). `qb::PingEvent`/`qb::RequireEvent` now carry an echoed `correlation_id` (0 = legacy
      path), and `PingEvent` type `0` is a wildcard liveness probe.
    - `qb::CoroContext::broadcast<E>(args)` — broadcast from inside a spawned coroutine (mirrors
      `Actor::broadcast`; backs `qb::require`).
- **`onInit()` is a first-class coroutine context for the WHOLE pattern library.** A unified
  *continuation registry* + a generalized activation gate now deliver **every** correlated coroutine
  reply (`ask`, `ask_stream`, `ping`, `require`) to an actor that is still *Activating* — not just
  `ask`. So `co_await qb::ask_stream(...)` and `co_await qb::require<T>(...)` work **inside
  `onInit()`** (e.g. *discover-before-activate*: block activation until peers/streams resolve),
  bounded by `activation_deadline_ns`. New `qb::CorrelatedEvent` base (carries `correlation_id`)
  unifies the routing; `AskEvent`/`RequireEvent` derive it.
- `qb::Actor::now()` — the typed `qb::wall_time` view of `time()` (cached per loop iteration); plus
  `qb::wall_from_unix_nanos(ns)`.
    - Idempotency: `qb::answer_idempotent(self, e, cache, fn)` + `qb::dedup_map` (bounded LRU) +
      `qb::idempotent_event` — a responder runs the effect **once per stable `idempotency_key`** and
      replays the cached response for retries/duplicates.
    - Aggregation: `qb::batcher<T>` coalesces items and flushes on a **count** or **time** window
      (whichever first); the window timer is scope-bound (a kill cancels it, no `on_flush` on a dead
      actor).
    - Streaming: `qb::ask_stream` returns a `qb::stream<E>` of many replies for one request
      (`qb::StreamRequest<Chunk>`, responder helpers `qb::yield_answer` / `qb::end_stream`; the asker
      routes chunks with `resolve_ask` — they are `AskEvent`s); per-chunk timeout, cancel-on-kill, and
      a loud `stream_overflow_error` rather than silently dropping chunks.
- **Self-locating resources** (`qb/io/system/file.h`, namespace `qb::io::sys`):
  `self_path()` (absolute path of the running executable, via `GetModuleFileNameW` /
  `/proc/self/exe` / `_NSGetExecutablePath` — independent of `argv[0]` and the cwd), `self_dir()`
  (its parent directory), and `resolve_resource(std::filesystem::path)` — resolves a relative
  resource path against the cwd first (historical behaviour), then the executable's own directory,
  so a binary shipped next to its assets finds them from **any** working directory. Absolute paths
  pass through unchanged.

### Changed

- `Actor::onInit()` signature: `bool onInit()` → `qb::io::async::task<bool> onInit()` (`co_return
  true/false`). A synchronous init simply `co_return`s and pays none of the suspended-init
  machinery. **Source-incompatible** — see the migration guide.
- `qb::ICallback` periodic hook: `void onCallback()` → `void on(qb::LoopEvent const&)`. The tick is
  now an `on(...)` handler like every other notification (carrying `qb::LoopEvent`); register/
  unregister via `registerCallback`/`unregisterCallback` as before. **Source-incompatible.**
- `addRefActor<T>()` / `addRefHandle<T>()` return `ActorHandle<T>` instead of a raw `T*`.
- Discovery: `qb::RequireEvent` is auto-registered with a default handler (like `KillEvent`/
  `PingEvent`) that routes coroutine `ping`/`require` replies — **no `on(RequireEvent&)` boilerplate**.
  Removed `qb::RequireEvent::status` and the `qb::ActorStatus` enum (a reply *is* the liveness signal;
  `Dead` was never sent). The free `qb::resolve_stream` / `qb::resolve_require` are gone: stream
  chunks are routed by `Actor::resolve_ask` (they are `AskEvent`s) and discovery replies by the
  default `Actor::on(RequireEvent&)` / `Actor::resolve_require`. **Source-incompatible** for code that
  read `RequireEvent::status` or called the removed free functions.
- **Filesystem-path APIs now take `std::filesystem::path`** (were `std::string` / `const char*`), so
  Unicode paths are handled natively (Windows opens via `CreateFileW`): `sys::file::open` + the
  `file` ctor + `file_to_pipe` / `pipe_to_file::open`; the SSL helpers `create_server_context`,
  `load_ca_certificates` / `load_ca_directory`, `configure_mtls_server_context`,
  `configure_client_certificate`, `configure_dh_parameters_server` (and the `ssl::listener` mirrors);
  the UNIX-domain-socket `connect_un` / `n_connect_un` / `listen_un` / `bind_un` (tcp + udp + ssl);
  `async::file_watcher` / `directory_watcher::start`; and `nanolog::initialize`. URL/URI/route paths
  and remote/wire paths stay `std::string`. **Source-compatible** for callers — `std::string`,
  `const char*`, and string literals implicitly convert to `std::filesystem::path` — but an **ABI
  break** (the mangled symbols change), so rebuild dependents.
- SSL cert/key/CA/DH file paths are resolved through `qb::io::sys::resolve_resource` consistently
  across `create_server_context` and the CA/DH/client-certificate helpers, so a relative path works
  from any working directory (absolute paths unchanged).

### Fixed

- `qb::io::inet::endpoint::to_string()` for IPv6 wrote the closing `]` one byte early, **truncating
  the last character of every IPv6 address** (e.g. `::1` rendered as `[::]`, `2001:db8::abcd` as
  `[2001:db8::abc…]`). The bracket offset now accounts for the leading `[`.
- Coroutine frame pool aligns to the canonical cache line (`QB_LOCKFREE_CACHELINE_BYTES`) so a
  by-value `qb::Event` in a frame no longer faults under `-O3 -march=native`.
- Activation-stash events with non-trivial payloads are destroyed (not leaked) when an async
  `onInit()` fails, the actor is killed during init, the deadline expires, or the stash cap overflows.
- Windows server bind correctness/security: `socket::pserve` now sets `SO_EXCLUSIVEADDRUSE` on
  Windows instead of `SO_REUSEADDR`. The old `SO_REUSEADDR` had *hijack/shadow* semantics there — a
  bind to an in-use port succeeded but was silently shadowed by the existing socket, so the listener
  never accepted. An in-use bind now fails fast with `WSAEADDRINUSE` and no other process can hijack
  the port. POSIX keeps `SO_REUSEADDR` for fast `TIME_WAIT` rebind (guarded by `#ifdef _WIN32`).
- File/directory watcher path lifetime: libev `ev_stat` stores the path *pointer* without copying,
  so `async::file_watcher` / `directory_watcher` now own the path string for the watcher's lifetime
  (no dangling watch path).

## [2.0.0]

The 2.0 series modernizes the framework's vocabulary and hardens the runtime. Highlights below; the change
is broad, so entries are grouped rather than exhaustive.

### Added

- C++20 baseline across the framework (`QB_CXX_STANDARD=20` by default, `QB_CXX_STANDARD=23`
  supported, extensions off).
- A canonical `std::chrono` time vocabulary: `qb::duration` (nanosecond span), `qb::mono_time`
  (steady-clock instants), and `qb::wall_time` (system-clock instants), with helpers in
  `qb/include/qb/system/time.h`.
- QUIC/HTTP3 transport with tri-state `QB_WITH_QUIC` auto-detection (enabled when ngtcp2 is present).
- Tree-wide position-independent code and a modernized CMake configuration (presets, install/export,
  dependency resolution with system-first fallback).

### Changed

- **Time handling migrated to the new chrono model.** All timeout, delay, interval, latency, and deadline
  APIs now take `qb::duration` or a clock-typed instant. The previous `qb::Timestamp` / `qb::Duration`
  types are removed (see *Removed*).
- CMake build modernized: corrected PIC handling, dependency lookup, and install/export of `qb::core` /
  `qb::io` targets.
- Warning hygiene: builds clean under `-Wall -Wextra -Wpedantic`.

### Removed

- `qb::Timestamp`, `qb::Duration`, and `qb::TimePoint`. Replace `qb::Duration` with `qb::duration`, and
  `qb::Timestamp` / `qb::TimePoint` with `qb::mono_time` or `qb::wall_time` depending on use (a monotonic
  anchor vs. a calendar instant). See the [migration guide](./readme/6_guides/migration_guide.md).

### Fixed

- qb-io async/transport: use-after-free on protocol switch; stop the I/O watcher before the transport file
  descriptor closes in client/server/acceptor destructors.
- qb-io coroutine: spawned-frame leak and channel use-after-free (parked waiter resumed after destruction).
- qb-io crypto: implemented missing key generation and corrected Argon2 password hashing.
- qb-io URI: out-of-range ports are rejected instead of silently truncated.
- qb-system: lock-free SPSC/MPSC memory-safety and correctness fixes; ring-buffer copy exception safety;
  ISO 8601 parsing treated as UTC.
- qb-core: actor lifecycle, event routing, and core-affinity hardening from a deep audit.

### Security

- QUIC: connection-limit denial-of-service mitigation, fail-closed RNG, and flow-control hardening.
- async/transport and crypto paths hardened to fail closed under malformed or hostile input.

[Unreleased]: https://github.com/isndev/qb/compare/v2.6.0...HEAD
[2.6.0]: https://github.com/isndev/qb/releases/tag/v2.6.0
