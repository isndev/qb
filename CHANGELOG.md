# Changelog

All notable changes to qb are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to
[Semantic Versioning](https://semver.org/). See [VERSIONING.md](./VERSIONING.md) for the compatibility
policy.

## [Unreleased]

Tracks changes on the development branch that are not yet part of a tagged release.

_Nothing yet._

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
  `qb/include/qb/system/timestamp.h`.
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
