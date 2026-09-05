# Changelog

All notable changes to qb are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to
[Semantic Versioning](https://semver.org/). See [VERSIONING.md](./VERSIONING.md) for the compatibility
policy.

## [Unreleased]

### Added

- **`qb::allocator::segmented_pipe<T>` + `segment_pool<T>`** (`qb/system/allocator/segmented_pipe.h`)
  — a FIFO of fixed-size segments (256 KB, from a pool the owning `VirtualCore` keeps) that grows
  by linking a segment behind the tail and never moves what it holds: `allocate_back(n)` is a
  compare and a cursor add while the tail has room, a range never straddles two segments, a
  request wider than a segment gets a dedicated exactly-sized one, and the read side walks
  `front()` / `consume_front()` / `pop_front()` a segment at a time, each popped segment going
  straight back to the pool. `qb::VirtualPipe` is now this type (was `allocator::pipe<EventBucket>`).
- **`qb::allocator::slab_cache`** (`qb/system/allocator/slab.h`, `qb/io/slab.cpp`) — the
  process-wide source of the pool's memory: 2 MB slabs mapped by the platform (`mmap` trimmed to
  a 2 MB boundary on POSIX, `VirtualAlloc` on Windows), on Linux `madvise(MADV_HUGEPAGE)`d and
  populated in one `MADV_POPULATE_WRITE` pass (5.14+), carved eight standard segments to a slab,
  and kept on a free list when a pool gives them back so the next engine or core draws memory
  that is already mapped and faulted. `trim()` returns the cached slabs to the OS. Cold path
  only — one acquire per eight segments of growth, never per event.
- **`CoreInitializer::setIdleSpin()` / `Main::setIdleSpin()`** — how long an idle `latency > 0`
  core keeps polling before it parks on its mailbox, a time floor measured from its first idle
  pass (default `kDefaultIdleSpin`, 50 µs; `getIdleSpin()` reads it back). Until now the floor
  was an event-count credit refilled from the previous pass, which parked a one-event-per-pass
  workload after two or three empty passes: every hop of a two-core request/response exchange
  paid an OS park + wake (measured 2–13 µs against ~300 ns of polling). `setIdleSpin(0)` restores
  the park-on-first-idle-pass behaviour.
- **`listener::has_work()`** — whether a qb-io `run()` turn has anything to do, read from the
  loop's own `ev_active_count()` / `ev_pending_count()` plus the deferred queue and the coroutine
  scheduler's ready list. `VirtualCore` uses it to gate its libev pass; consumers driving a
  listener with `EVRUN_NOWAIT` get the same gate.
- **`mpsc::ringbuffer::has_data()`** — does any producer ring hold an item; the predicate of the
  mailbox park.

### Changed

- **The reference `Actor::push` / `Pipe::push` / `allocated_push` returns is valid until the
  handler or callback that obtained it returns** — across any number of further pushes to the
  same core — where it used to die at the very next event queued to that destination core. The
  event pipes were one contiguous `allocator::pipe` per destination core: growth `memcpy`d
  everything already queued and compaction `memmove`d it in place, which is what invalidated
  the reference, and what cost a one-core burst of 1 M events 64 MB of copying plus ~26 600
  minor page faults per run re-touching each doubling (the cliff between 100 k and 300 k
  messages in `qb-vs-others`). With `segmented_pipe` nothing queued ever moves, so the reference
  holds until the engine consumes the event between handlers. Code written to the old rule is
  still correct; what remains forbidden is holding the reference across a coroutine's
  `co_await` or in a member. Pinned by `SegmentedPipeContract.*` (unit) and
  `PushReferenceStability.*` (through the engine, same-core, cross-core and under growth).
- **Event pipes allocate nothing until their first push, and the per-core pool holds the high
  water in 2 MB slabs** (`segment_pool::shrink()` returns whole idle slabs to the cache,
  `slab_cache::trim()` returns the cache to the OS). Every core used to allocate 256 KB eagerly
  for each of its N + 1 pipes — 68 MiB at rest on 16 cores — and each pipe doubled on its own,
  so the peak was 2–3× the largest burst and never came back. A consumed segment is the next
  one a push grows into, still warm in cache. Slabs, not `malloc`, because the A/B measured
  where the residual went: with segments from the heap a fresh engine's 1 M-event one-core
  burst still took 15 640 minor page faults — every 4 KB of every segment — which on WSL2 was
  ~15 ms of a 20 ms run; a slab is faulted once per 2 MB, in the kernel, and never again for
  the life of the process.
- **The receive loop reads an event's width once, before its handler runs, and the pool
  staggers its segments.** Segments carved on a fixed stride out of aligned slabs all start at
  the same offset within a 4 KB page, so item `k` of the pipe being drained and item `k` of the
  pipe being filled share their low twelve address bits — and a reply IS a byte copy of the
  received event into the outbound pipe at the same index. The loop then re-read
  `bucket_size` from the event to advance, a load trailing a store to an address the core
  cannot tell apart from it until the store commits (Intel's 4K aliasing); measured on
  `savina/big` at one core, that one stall per event doubled the run against the malloc-laid
  pipes it replaced (52 → 107 ms per rep, that reload alone 12% of the samples). The width is
  now read once, ahead of the handler, and `segment_pool` gives the `i`-th segment it carves a
  stagger of `(i * 27) % 64` cache lines so consecutive segments — the swap pair of a core —
  never share a page offset, which also covers a handler that touches the event after
  `reply()`. The stride is 27 and not 1 because a one-line stagger moves the store onto the
  NEXT event's load for one-line events, measured at +8% on the same benchmark. The receive
  loop is driven by `front()` alone — a range that is empty exactly when the pipe is — rather
  than testing `empty()` before and after each range. Same-core round trips end up faster than
  before the segmented pipe (Linux/g++-14, median of 5 interleaved launches: big 52.7 → 52.2 ms,
  ping-pong 67.7 → 64.2 ms; ten-launch census, ns per message: thread-ring 37.7 → 35.6 at one
  core and 110.6 → 105.5 across two, big 21.9 → 21.4). Windows/MSVC keeps its one-core
  ping-pong level within 1% (77.2 → 78.0 ns per round trip, median of 24 interleaved launches,
  distributions overlapping) while every burst cell there is 2–4× faster.
- **The `latency > 0` park handshake is race-free.** `Mailbox::wait()` was
  `cv.wait_for(lk, latency)` with no predicate and `notify()` signalled without the mutex, so an
  enqueue landing between the consumer's empty drain and its registration on the condition
  variable was never seen and the core slept the whole latency. Measured on a two-core ping-pong
  at 300 000 messages: 246–293 lost wakeups on Linux (1 ms each) and 48–2849 on Windows, where
  MSVC's `wait_for` rounds up to the 15.6 ms scheduler tick. The handshake is a Dekker pair over
  a cache-line-aligned `_parked` flag with `seq_cst` fences on both sides, the producer taking
  the mutex before it notifies and the consumer waiting on `has_data()`. At latency 0 `wait()`
  returns at once and `notify()` is only its fence.
- **`Mailbox::notify()` issues its `seq_cst` fence in spin mode too.** On x86 that fence is an
  `mfence`, which drains the producer's store buffer, so the event just enqueued becomes visible
  to a spinning consumer now rather than whenever the buffer flushes. Measured on a two-core
  ping-pong (interleaved A/B on a quiet host, p50 of 7 runs): Windows/MSVC 296–309 ns per
  round-trip without it and 259–263 with it, where the spinning cell had been slower than the
  parking one (256–272); Linux/g++-14 204–219 → 205–208, with the worst run falling from 295 to
  215 ns. Bulk single-producer throughput (`BM_Multi_Producer_Consumer`, 1M events) is unchanged
  at 18.6 M msg/s. arm64 is unmeasured.
- **A core no longer parks over its own pipe.** An actor pushing to itself from a callback moved
  no counted event, so a parked core delivered that push after `latency`; the idle test now also
  requires the self-core pipe to be empty.
- **`VirtualCore` polls libev only when the loop has work**, instead of on every pass once the
  core had ever touched a coroutine or registered a handler.
- **`Actor::time()` samples the clock on demand, once per pass**, keyed on the pass index,
  instead of unconditionally at the top of every pass.
- **`spsc::ringbuffer` lays its producer and consumer indices out on separate cache lines and
  each side keeps a private snapshot of the peer's index**, so an uncontended push or pop
  touches one line and re-reads the peer only when its snapshot says full or empty.

### Fixed

- **The start barrier yields once it has spun.** `Main::__wait__all__cores__ready()` and the
  calling thread's wait in `Main::start(true)` spun on the ready counter without ever yielding,
  so an engine started with more cores than CPUs — `hardware_concurrency()` ignores affinity
  masks and cgroup quotas — made the last core to initialise compete for a CPU against every
  core already waiting for it. Under ThreadSanitizer that is a hang, not a delay: on a 24-vCPU
  WSL2, 23 spinning acquire loads held TSan's atomics lock in read mode continuously, the 24th
  core's `fetch_add` never got in, and `MainLifecycle.StopMultiCoreGracefulNoError` ran past its
  600 s timeout at any CPU count. Both loops spin 1024 times and then `yield()`; the same test
  completes in 2 s and the signal variant went from 61 s to 2 s. **And then it sleeps**: with
  one CPU per waiter there is nobody to yield to, `yield()` returns at once, and the polls'
  acquire loads still starved the last core's every atomic op behind 23 readers of TSan's
  atomics lock — the three multi-core lifecycle tests measured anywhere from 0.1 s to 30 s per
  run on the same host. After 256 yields the poll sleeps 50 µs between loads (one shared
  `wait_sync_start` for both loops); the tests take 100 ms, every run, and an engine whose
  cores arrive more than ~100 µs apart pays at most one quantum, once.

### Known limitation

- A parked core does not consult qb-io's timer deadlines: a `qb::io::async::callback` armed on a
  `latency > 0` core that has parked fires when the park times out, so `latency` bounds timer
  precision on that core. Pinned by `core-park-policy`; an engine that learns io deadlines must
  move that expectation with it.

## [3.1.0] - 2026-08-30

### Added

- **Windows console control events now reach the signal pipeline.** A supervisor stopping a
  qb process from outside — the runner sending CTRL_BREAK to a process group, a console
  window closing — previously killed it with no actor teardown: SIGTERM is never
  OS-delivered on Windows, and nothing mapped the console events the OS does deliver.
  `install_default_signals()` now installs the console-control bridge: the CRT raises
  CTRL_BREAK and CTRL_CLOSE as `SIGBREAK`, and the bridge re-raises it as the `SIGTERM`
  the rest of the pipeline — and every actor's default kill path — already speaks. Written
  cross-platform actor code (`on(SignalEvent)` with `SIGTERM`) needs no `#ifdef`. An
  explicit `registerSignal(SIGBREAK)` still delivers `SIGBREAK` untranslated. CTRL_CLOSE
  grants ~5 s before the OS kills the process; the engine teardown fits well inside.

## [3.0.0] - 2026-08-20

**This is a major release, and it breaks source in eight places.** Count them here rather than
discovering them one build error at a time — *Removed* carries five and *Changed* three:

| | What breaks | Section |
|---|---|---|
| 1 | The vendored `nlohmann/json.hpp` is gone; nlohmann is now a **real dependency you must have** | Removed |
| 2 | The four `.tpp` headers — including `<qb/core/Actor.tpp>`, `<qb/core/Pipe.tpp>`, `<qb/core/Main.tpp>` | Removed |
| 3 | `<cube.h>`, the whole-framework umbrella, left the installed public surface | Removed |
| 4 | `SO_NOSIGPIPE` is no longer defined by qb | Removed |
| 5 | `std::to_string(const uuids::uuid &)` | Removed |
| 6 | The unprefixed socket-portability macros are **off by default** | Changed |
| 7 | `qb::Event::id_type` is now `qb::EventId` (a `uint16_t`) in *every* build mode | Changed |
| 8 | `qb::type_id<T>()` / `Event::type_to_id<T>()` / `detail::type_id_for<T>()` now require a **complete** `T` | Changed |

Each row is a bullet below, with the migration. [VERSIONING.md](./VERSIONING.md) reserves removals
that require source edits for a major release.
The vendored event loop's rename (`ev_*` → `ev_*`, `<qb/vendor/ev/ev++.h>` →
`<qb/vendor/qev/ev++.h>`) has now landed, and so has the qbm public include prefix — as
`<qbm/http/...>`, `<qbm/pgsql/...>`, `<qbm/redis/...>`, not the `<qb/...>` spelling this note
previously anticipated. Keeping `qbm/` leaves the installed location `include/qbm/<name>/`
untouched, so every CI assertion holds through the migration, and it keeps the two package
roots disjoint: `include/qb/` stays qb's alone rather than becoming co-owned by four
independently-versioned packages. The last structural break has now landed too: the four
installed `.tpp` headers are gone (see *Removed*), which requires a major bump on its own.

The qbm modules version in lockstep with the framework: `qbm-http`, `qbm-pgsql` and `qbm-redis` all
carry `3.0.0`. They are not standalone-configurable (they call `qb_register_module` / `qb_add_test`,
which an installed qb does not ship), so their version only ever means "the framework this was built
against" — and the include-prefix move above lands hardest in exactly those modules.

### Added

- **A CI job for the three packaging mechanisms this release added, which shipped with none.**
  `install-consume.yml`'s `packaging-switches` covers `QB_REQUIRE_FEATURES` (a degraded feature must
  become a configure error, with the default-warn path as its control so the check cannot go
  vacuous on a runner where the feature happens to be satisfiable), `QB_USE_SYSTEM_NLOHMANN`
  (`OFF` selects the bundled copy; an out-of-range value is rejected), and
  `share/qb/abi-fingerprint.txt` (the installed `qb-abi` line must be byte-identical to the string
  inside the archive it describes — that non-drift is the file's whole contract — and every
  configuration field must carry a value).
- **`qb::detail::prepare_event_storage`** (`core/Event.h`) — debug-only preparation of an event's
  bucket range before its payload is constructed into it. See *Fixed*: it is what makes
  `SharedCoreCommunication::send`'s self-pointer guard sound rather than merely suggestive.
- **`QB_USE_SYSTEM_NLOHMANN` — a tri-state (`AUTO` / `ON` / `OFF`) for the nlohmann/json source,
  and the first of the three things a distribution packager was missing.** `AUTO` (the default)
  probes for a system `nlohmann_json` and otherwise fetches the pinned `QB_NLOHMANN_GIT_TAG`
  (`v3.12.0`) via `FetchContent`, subject to `QB_DEPS_FETCH_FALLBACK` — the same system-first /
  git-fallback policy zlib, GoogleTest and Google Benchmark already follow. `ON` *requires* a
  system copy and hard-fails at configure time if it is absent; `OFF` always fetches. See
  *Removed* for why the bundled copy this option originally selected no longer exists.
- **`QB_REQUIRE_FEATURES` — make a silent feature downgrade a configure-time error.** Default `OFF`
  keeps the developer-friendly behaviour (warn, set `QB_HAS_<x>=FALSE`, carry on). `ON` turns every
  such downgrade into `FATAL_ERROR`. This exists because a hermetic packaging environment (vcpkg, a
  buildd chroot, a brew sandbox with no network) that cannot see OpenSSL or zlib produced a
  **quietly reduced** package at exit 0, and the packager's only way to notice was to inspect the
  artefact afterwards. Reached through the new `qb_feature_degraded()` helper, which is only called
  on a genuine degradation — an `AUTO`/`OFF` resolution is not one and does not go through it.
- **`share/qb/abi-fingerprint.txt` — a prebuilt prefix now records what it was built with.** Until
  now the only answer to "what is in this bottle?" was
  `strings lib/libqb-io.a | grep '^qb-abi '`, which needs the archive, the right grep, and prior
  knowledge that the string exists — and it covers only the five link-enforced ABI axes, not the
  feature flags that silently downgrade at configure time. The `qb-abi` line is **read back out of
  the built archive**, not re-derived from CMake variables, so it cannot drift from the artefact
  the way every other hand-maintained version string in this tree already has; if the archive does
  not contain it, the file says so and the install warns, rather than emitting a plausible-looking
  line. Documented in [INSTALL.md](./INSTALL.md).

- **`scripts/check-header-extensions.py` and `scripts/check-header-linkage.py` — the two header
  rules 3.0 relies on, made enforceable instead of remembered.** Both run in the `format-check` CI
  job (pure Python, no toolchain) and in the superproject's `dev/agent/verify.sh`.
  - The first fails if a `.tpp` or `.inl` reappears anywhere, or if anything `#include`s one. That
    rule had no check at all: `.cursor/rules/cpp.mdc` used to *instruct* agents to put template
    definitions in `.tpp` files, which is a standing instruction to reintroduce exactly what this
    release removed.
  - The second fails if a shipped header defines a **strong external symbol** — a non-template,
    non-`inline` function or variable at namespace scope, an explicit *specialization* definition
    (which is not implicitly inline), or an out-of-line member definition. This is the hazard that
    outlived the extension: `qb-core` is one TU, so `Actor.h`/`VirtualCore.h` reach both the
    archive and every consumer TU, and one such definition links green here and fails at the
    *consumer's* link. The rule covers all 267 shipped headers, not just the six merge sites,
    because the audit had already measured every namespace-scope definition in them to be a
    template or `inline`, and that is the invariant worth freezing. Escape hatch:
    `// header-linkage: allow <reason>`, reason mandatory.
  - Each rule was validated against a real **2-TU + archive link** before being written down, and
    the guard's verdict matched `ld64` on all seven shapes tried. One candidate rule was deleted
    for failing that comparison: an explicit *instantiation* definition (`template int f<int>(int);`)
    is emitted COMDAT/weak — `nm -m` shows `weak external` against `external` for a plain function —
    and links clean, so it is not a violation. `dev/agent/header-rules-negative-control.sh` replants
    all thirteen cases (including a resurrected `Actor.tpp`, both anti-vacuous floors, and a bare
    escape hatch with no reason) and asserts each is rejected, on a copy under `$TMPDIR`.
- **`scripts/check-abi-fingerprint.sh` + the `abi-fingerprint` CI job — coverage for the link-time
  configuration fingerprint, which shipped with none.** The script arms one mismatch per axis
  (cache line, exceptions, coroutine debug, jthread source, and the raw `-I`/`-l` version case)
  and requires each to **fail at link** naming that axis's symbol, requires a matching consumer to
  link *and run*, and checks that the reference array is still emitted — on ELF that the
  `abi_fingerprint` section still carries `SHF_GNU_RETAIN` (`readelf -S` → `WAGR`) and that a
  mismatch still fails under `-Wl,--gc-sections`. It refuses to report success vacuously: an axis
  this host cannot flip is an explicit SKIP, and a run below the check floor fails. The job runs on
  **both** Linux and macOS, because the `--gc-sections` defeat that `retain` exists to stop is
  ELF-only, and carries a negative control that removes `retain` and requires the check to fail
  (verified on Debian 13 / GNU ld 2.44: section flags `WAGR` → `WAG`, the cache-line mismatch links
  cleanly again, check exits 1).
- **`QB_ABI_ANCHOR` (`qb/utility/abi.h`) on every process-wide identity anchor.** The entities that
  must exist exactly once per process — `qb::detail::_type_id_counter`, the router's `_disposers`
  table and its two registration statics, the `ServiceActor` registry (`VirtualCore::getServices`,
  `servicesMutex`, `_nb_service`), `qb::io::async::no_protocol()`, the coroutine frame pool
  (`live_frames`, `pool_alive`, `buckets`), the two watcher free-lists, `listener::current`,
  `VirtualCore::_handler`, `CoroutineScheduler::current_` / `owned_current_` and
  `VirtualCore::activation_deadline_ns` — are annotated `visibility("default")` so they keep
  coalescing when a consumer compiles `-fvisibility=hidden`. The macro is empty on MSVC: a Windows
  shared build needs the export-macro work first, and this is not a substitute for it.
  See [readme/7_reference/building.md](./readme/7_reference/building.md#one-instance-per-process).
- **A process-wide type-id registry (`qb::detail::_type_id_registry`).** `type_id_for<T>()`'s magic
  static is now a per-image *cache* of an id owned by a shared intrusive list, instead of being the
  identity itself. This is what closes the id-space fork below in the one case an annotation
  cannot: a block-scope static is not exportable. The list head is `nullptr`, i.e. constant
  initialisation — `Event.cpp` still emits **no** `__mod_init_func`, so nothing about the
  static-initialisation-order guarantee changes, and the routing path still reads one
  already-initialised local static.
- **`QB_LOG_DEBUG` / `QB_LOG_VERB` / `QB_LOG_INFO` / `QB_LOG_WARN` / `QB_LOG_CRIT`** — the prefixed
  spellings of qb's logging macros, and the only ones qb's own 138 call sites (and qbm's 94) now
  use. `QB_NO_LEGACY_LOG_MACROS` suppresses the unprefixed aliases.
- **`QB_CLOSESOCKET` / `QB_IOCTLSOCKET` / `QB_SD_RECEIVE` / `QB_SD_SEND` / `QB_SD_BOTH` /
  `QB_SD_NONE` / `QB_FD_TO_SOCKET` / `QB_OPEN_FD_FROM_SOCKET`** (`qb/io/config.h`) — the prefixed
  spellings of the socket-portability macros. `QB_LEGACY_SOCKET_MACROS` brings the unprefixed ones
  back.
- **`QB_EV_LIBEVENT_COMPAT`** (default `OFF`) — builds and installs the qev fork's libevent
  compatibility layer (`event.c`, `event.h`, `event_compat.h`).

- **A link-time configuration fingerprint (`qb/utility/abi.h`).** qb ships headers plus a compiled
  archive, and a handful of macros the *consumer* sets change the layout of public types in those
  headers, or the body of an inline entity the archive also defines. Nothing detected the
  disagreement — not the compiler (each translation unit is internally consistent), not the linker
  (vague-linkage bodies merge silently, winner by link order). Measured consequence of the worst
  one: an application built with `-DKNOWN_L1_CACHE_LINE_SIZE=128` (a **documented** public knob, and
  the true `hw.cachelinesize` on Apple Silicon) against a stock qb gets `sizeof(qb::Event)` 64 in
  one half and 128 in the other, and the coroutine frame pool overruns its block —
  `AddressSanitizer: heap-buffer-overflow`, after linking and running clean.
  Now the archive *defines* one symbol per ABI axis, named after the value it was built with
  (`qb_abi_cacheline_64`, …), and every translation unit that parses a qb header *references* the
  symbol named after its own value, so a mismatch is an **undefined symbol at link** naming the axis
  and the application's value. Unlike a CMake-side check it survives `add_compile_options()` and
  generator expressions added after `find_package()`, because it is in the header the compiler
  actually reads. Five axes, each with a measured layout or shared-body divergence: qb version,
  `KNOWN_L1_CACHE_LINE_SIZE`, `-fno-exceptions`, `QB_DEBUG_COROUTINES`,
  `QB_COMPAT_FORCE_THREAD_FALLBACK`. `NDEBUG` is deliberately **excluded** — it changes no layout
  (and `scripts/check-abi-macro-split.py` keeps it that way), and a Debug consumer against a Release
  archive is a supported, CI-tested configuration that an `NDEBUG` axis would break by default. The
  archive also carries its configuration as readable text (`strings libqb-io.a | grep '^qb-abi '`),
  which is the first time an installed qb records how it was built. See
  [readme/7_reference/building.md](./readme/7_reference/building.md#link-time-configuration-fingerprint).
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
- **`scripts/check-installed-headers.sh` — installed-header self-containment + entry-point LINK
  gate**, wired into `install-consume.yml` (the `qb` tree, under clang) and into the superproject's
  `package-consume.yml` (the `qbm` tree). Two phases against an **installed prefix**:
  *(1)* every installed header compiled **alone**, one TU whose entire content is
  `#include <that/header.h>`; *(2)* every public entry point **linked** into an executable that
  ODR-uses its headline API (`scripts/installed-entry-points/`, one header per file). Phase 2 exists
  because `-c` and `-fsyntax-only` both pass on a header that declares a member template no reachable
  TU defines — only the linker says so. The job that existed before this could see neither class: it
  compiles one TU, and that TU includes `<qb/actor.h>` **and** `<qb/main.h>`, the one combination in
  which both `qb::Actor::push<E>` and `forget_frame_if_current` resolve. It found 18 headers and
  4 entry points, all fixed below.
  `--hostile` adds the consumer-side half: `-I` instead of the `-isystem` CMake puts on an IMPORTED
  target, plus `-Wall -Wextra -Werror` and, on clang, `-Wundefined-func-template`. On qb the
  `-isystem` is not CMake's automatic behaviour but an explicit
  `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` in the export (`qb::nlohmann`, whose include dir *is* qb's
  whole public include root), so `IMPORTED_NO_SYSTEM` alone changes nothing — the script clears all
  three and then **asserts** the generated command line really says `-I`.
  Every `--tree` carries an anti-vacuous floor, and the excluded-header list is checked against the
  prefix so an exclusion cannot outlive the file it names.
- **`scripts/check-installed-headers-selftest.sh`** — negative controls for the above, run by CI.
  Plants one defect at a time in a **copy** of the prefix (missing include; undefined function
  template; a link-only failure that leaves phase 1 at 0 failures; a `-Wall` finding only `-I` can
  see; an impossible floor; a stale exclusion; an empty entry dir), requires a rejection, restores,
  and verifies the restore is byte-exact by sha256. A control the compiler structurally cannot run
  is reported `N/A` and counted separately — never as a pass.

### Changed

- **`QB_ENABLE_NATIVE_ARCH`'s documented default was the inverse of the code, across the readme
  books, `INSTALL.md`, `README.md` and the `.cursor/` rules and skills.** The option is
  `option(... OFF)` (`qbConfig.cmake:140`) and has been — verified by running a bare
  `cmake -S qb -B tmp -DCMAKE_BUILD_TYPE=Release` with no preset, which yields `OFF`. There is no
  configuration in which `ON` is the default. Two files contradicted themselves within a few lines,
  and one stated the narrowest wrong version ("the `ON` default applies only to a raw `cmake -D...`
  build with no preset"), which is exactly the case that disproves it. The failure was silent in
  the way that matters: a reader wanting host tuning believed `release` already passed
  `-march=native`, a performance claim that was false with no diagnostic — and one troubleshooting
  entry mis-diagnosed "binary crashes on another machine" as native-arch, sending a SIGILL
  investigation down a dead end where the suggested fix is a no-op. `qbConfig.cmake` also carried
  two contradictory comment blocks on the same option; only one survives.
- **[INSTALL.md](./INSTALL.md) corrected in four places and extended in one.** The CI matrix
  advertised a Windows/MSVC row that is commented out in `.github/workflows/cmake.yml` (it would
  rebuild every dependency from source on each run); the table now marks it disabled and says why.
  The `QB_ENABLE_NATIVE_ARCH` default was wrong in the troubleshooting entry and implied a second
  time by the "common production configuration" example. The install was described as exporting the
  bundled `FindArgon2` / `FindNgtcp2` modules unconditionally — they ship only when the
  corresponding feature was enabled. And there is now a section on `share/qb/abi-fingerprint.txt`,
  which the page did not mention at all.
- **Citations re-pointed after the source moves in this release.** The `qbConfig.cmake`,
  `qbCompiler.cmake`, `qbDependencies.cmake`, `qbFunctions.cmake` and `VirtualCore.cpp` edits above
  shifted line numbers under ~150 `src:` citations across the readme books, `llm/` and
  `.cursor/`. Each was re-pointed and then *proved* by the digest baseline: the first cited line at
  the new coordinates must match the excerpt recorded when the citation was last hand-verified.
  138 verified that way, 0 mismatches; the handful that could not be proved mechanically (a range
  whose content genuinely changed, or a citation that had drifted before this release) were
  resolved by hand. Three of them were off-by-one, and two named the wrong subject entirely.

- **Every GoogleTest binary now runs with `--gtest_shuffle`** (`QB_TESTS_SHUFFLE`, default `ON`;
  `QB_TESTS_SHUFFLE_SEED` pins the order, `0` = seed from the clock, and GoogleTest prints the seed
  it used so any failure is reproducible). A suite whose result depends on declaration order is not
  measuring what it claims: `qb-core-test-unit-type-id-identity` reported `9 tests PASSED` in
  declaration order and segfaulted on half of the shuffle seeds. Measured fallout across the whole
  tree before enabling it: 355 test binaries × 3 seeds, exactly one order-dependent failure — the
  registry defect above.

- **`listener::current` is defined in the header, not in `listener.cpp`** — and the same for
  `VirtualCore::_handler`, `VirtualCore::_nb_service`, `VirtualCore::activation_deadline_ns` and
  `CoroutineScheduler::current_` / `owned_current_`. A `thread_local` (or `static`) **defined out of
  line** emits a symbol that is private to its image by construction — on Mach-O,
  `(__DATA,__thread_vars) non-external`. A host executable and a `dlopen`ed plugin that each
  statically link qb therefore got **two** `listener::current` on the *same thread*, with **no
  unusual flags at all**: measured `[HOST] &current=0xc24c28080 size()=2` vs
  `[PLUGIN] &current=0xc24c29880 size()=0`, in RTLD_LOCAL and RTLD_GLOBAL alike, so everything the
  plugin registered went into a loop nobody runs — silently. Defined `inline` in the header the same
  descriptor is *weak-external*, which dyld coalesces: after the change both images report the same
  address and the plugin's registration lands in the host's loop (`size()` 2 → 3). No API changed;
  `listener::current`, `VirtualCore::_handler` and the rest are spelled exactly as before.
- **The unprefixed socket-portability macros are off by default** (source-incompatible, see
  *Removed*). `qb/io/config.h` no longer defines `closesocket`, `ioctlsocket`, `SD_RECEIVE`,
  `SD_SEND`, `SD_BOTH`, `SD_NONE`, `FD_TO_SOCKET` or `OPEN_FD_FROM_SOCKET` unless
  `QB_LEGACY_SOCKET_MACROS` is defined.
- **The unprefixed `LOG_*` macros are aliases, and each is guarded by `#ifndef`.** A consumer who
  defines `LOG_INFO` before including qb now keeps their own definition. Before, qb replaced it,
  and the exact `-isystem` line qb's CMake package exports produced **zero warnings** while the
  consumer's log line stopped appearing. (With `-I` the same build reports
  `warning: 'LOG_INFO' macro redefined` — which is why the real integration never saw it.)
- **The qev fork no longer exports libevent's 24 unprefixed C symbols by default.** `event.c` — the
  libevent compatibility layer — is compiled and installed only under `QB_EV_LIBEVENT_COMPAT=ON`.
  `libqev.a` is on every consumer's link line (`qb::io` names `qb::qev` in its
  `INTERFACE_LINK_LIBRARIES`), so a consumer that also linked the real libevent got whichever
  `event_base_new` / `event_add` / `event_del` / … the archive order happened to pick, over two
  unrelated `struct event_base` layouts, with no diagnostic: measured, `-lfakeevent` before
  `libqev.a` ran the real one and after it ran qb's, both `rc=0`. Nothing in qb calls any of them
  (`nm -u libqb-io.a | grep -c '^ *_event_'` → 0). With the option off the real libevent now wins in
  **both** orders.
- **The qev fork's 35 `HAVE_*` autoconf macros are private to `libqev.a`.** `ev_config.h` is a
  public header (`ev.h` needs its `EV_USE_*` half, and `ev.h` is reached from `<qb/io.h>`), so it
  used to push qb's *build-host* answers — `HAVE_POLL 1`, `HAVE_KQUEUE 1`, `HAVE_EPOLL_CTL 0`,
  `HAVE_EVENTFD 0`, … — into every consumer translation unit, where a project running its own
  feature checks would read qb's answer instead of its own. The `HAVE_*` half is now gated on
  `QEV_BUILDING_LIBRARY`, which the `qev` target sets `PRIVATE`; the `EV_USE_*` half stays public
  and ungated, because a split *there* is the silent header/library divergence the generator's own
  comment warns about.

- **`qb::unordered_map` and `qb::unordered_set` are now unconditional aliases for
  `ska::unordered_map` / `ska::unordered_set`.** Since 2020 (`5c94d026`) they resolved to `ska::`
  under `NDEBUG` and to `std::` otherwise, which made the *identity and layout* of a public type
  depend on a build macro: `sizeof(qb::unordered_map<int,int>)` measured 32 with `NDEBUG` and 40
  without, and both templates are data members of public classes (`qb::VirtualCore`, `qb::Main`,
  `qb::router::*`) and of qbm's public headers. A consumer compiled **without** `NDEBUG` --
  `CMAKE_BUILD_TYPE=Debug`, or simply *unset*, which is CMake's default -- against a Release-built
  libqb read a `ska` map through `std` layout and aborted at run time with
  `std::overflow_error: __next_prime overflow`. Release builds are unaffected (that is the
  implementation they already used); Debug builds change container implementation, so a debugger's
  `std::unordered_map` pretty-printer no longer applies to these two aliases. Both are node-based:
  references and pointers to elements survive a rehash, as before.
- **BREAKING — `qb::Event::id_type` is now `qb::EventId` (a `uint16_t`) in *every* build mode.**
  It used to be `EventId` under `NDEBUG` and `const char *` otherwise, and `Event::type_to_id<T>()`
  changed return type with it. That is the same defect as the `qb::unordered_map` entry above, on
  the type that keys event routing: the header put `id` at offset 6 (2 bytes) in Release and offset
  8 (8 bytes, 8-aligned) in Debug, so `dest`/`source` sat at bytes 8/12 versus 16/20. Cross-core
  events are **memcpy-relocated** — `VirtualCore` reinterprets the wire buffer straight to
  `Event *` — so a consumer compiled with the other `NDEBUG` than the installed `libqb-core` read
  `dest` out of the payload and routed to a garbage `ActorId`. It did not crash: measured
  end-to-end, a Debug consumer against a Release libqb-core compiled, linked, reported the same
  `sizeof(qb::Event)` (64, unchanged in every mode) and delivered **0 of 10** events, silently.
  Release builds are byte-for-byte unaffected: the enqueue site `Pipe::push<T>()` and the
  steady-state dispatch loop of `VirtualCore::__receive_events__` are instruction-for-instruction
  identical to 2.6. Debug gets *faster*, not slower: the router key stops being a `const char *`,
  so `std::hash<const char *>` — a real call chain at `-O0` — becomes identity hashing. Measured
  directly on the dispatch lookup at `-O0` over a 211-type table (the real event-type count in this
  repo), 9 interleaved rounds: the 16-bit key is faster in **9 of 9**, median −51 %. At engine level
  that is −11 % ns/event on `messaging-api-oneway`, measured during the evaluation that recommended
  this change rather than re-derived here. Debug also
  regains 48 bytes of first-bucket payload capacity where it had 40.

  **What breaks, and only in Debug**, while a Release-only CI stays green: code that stored an
  `Event::id_type` in a `const char *`, printed it as a string, or wrapped it in its own
  `#ifdef NDEBUG`. Fix by using the id as the 16-bit integer it now is, and by taking the name from
  the new API below. See [the migration guide](./readme/6_guides/migration_guide.md#part-4--eventid_type-is-now-one-type-in-every-build-mode).
- **`qb::Event::type_to_name<T>()` and `qb::event_type_name(id)`** replace the readable name that
  the Debug-only `const char *` id used to provide by *being* the name. `type_to_name<T>()` returns
  `typeid(T).name()` (a link-time constant, Itanium-mangled — exactly what Debug printed before);
  `event_type_name(id)` reverse-resolves a runtime `Event::getID()` through a side registry filled
  when the id is assigned, returning `"<unregistered>"` for an id no type owns. Both are available
  in **every** build mode, so Release log lines gain a name they never had:
  `event[41]` → `event[N2qb9KillEventE#41]`. The registry is a direct-indexed table with one slot
  per `TypeId`: O(1), no allocation, no mutex, and nothing on a routing path reads it.
- **BREAKING — `qb::type_id<T>()`, `qb::Event::type_to_id<T>()` and `qb::detail::type_id_for<T>()`
  now require `T` to be a complete type in every build mode.** They reach `typeid(T)`
  unconditionally (that is what fills the name registry). Compiled against the 2.6 headers, both
  `NDEBUG` settings: `qb::type_id<T>()` — the path `ServiceActor<Tag>` and `getServiceId<Tag>()` go
  through — accepted an incomplete `T` in **both** modes, so this is a new error everywhere, not a
  Debug-only surprise. (`Event::type_to_id<T>()` already rejected one, but only in Debug, where it
  called `typeid(T).name()` directly.) Nothing in qb, qbm or the examples breaks — every
  `ServiceActor` tag here is defined — but the documented spelling
  `class S : public qb::ServiceActor<struct MyTag>` **stops compiling**: an elaborated-type-specifier
  declares `MyTag` without defining it. Write `struct MyTag {};` first. The paths that newly require
  completeness are the ones that never construct or size `T`: `ServiceActor<Tag>`,
  `Actor::registerIndex<Tag>` / `getServiceId<Tag>`, `Actor::require<T>` / `is<T>`,
  `qb::require<T>`, `ActorProxy::getType<T>`, and `router::*::unsubscribe<T>`.
- **New configure-time coherence gate: `QB_ABI_UNORDERED_MAP`.** `cmake/qbConfig.cmake` now
  publishes a token naming which implementation those aliases resolve to, and a prebuilt qbm module
  records the value it was compiled against. `find_package(qbm-<mod> CONFIG REQUIRED)` fails, naming
  both sides, if they disagree -- so a future public type whose identity is picked by a build macro
  breaks the configure rather than the process. Sits beside the existing version and
  `QB_HAS_SSL`/`QUIC`/`COMPRESSION` skew checks in `cmake/qbmModuleConfig.cmake.in`.
- **One install/export rule for qb and every qbm module: `qb_install_package()`, new in
  `cmake/qbPackage.cmake`.** The same job used to be implemented twice — hand-rolled in
  `CMakeLists.txt` for qb, and again as `_qb_module_install_rules()` in `qbFunctions.cmake` for a
  module — and no single function could have served both, because a module's install root was its
  whole *repository*, re-rooted onto `<includedir>/qbm/<name>` and filtered by an eight-term
  blacklist (`tests|readme|docs|scripts|cmake|examples|benchmarks|not-qb`) of everything under it
  that was not a header. Putting the modules on the same `src/`-is-the-include-root rule as qb
  deleted the blacklist, the re-rooting and the second implementation together: the header rule is
  now one directory copied verbatim onto another, byte-identical on both sides, and the only
  exclusion left anywhere is qb's own (stduuid's vendored Catch2). `_qb_module_install_rules()` and
  `_qb_module_include_roots()` are gone; `qb_package_include_root()` is now the single place that
  decides the build-tree/install-tree include pair, so `$<BUILD_INTERFACE:>`,
  `$<INSTALL_INTERFACE:>` and `install(DIRECTORY)` cannot drift apart.
  **Nothing a consumer writes changed, and nothing the install produces changed**:
  `find_package(qb CONFIG)` → `qb::core`/`qb::io`, `find_package(qbm-http CONFIG)` → `qbm::http`
  resolving qb via `find_dependency(qb)`, and the installed tree is file-for-file identical
  (verified by installing before and after into two prefixes and diffing: 398 entries, 289 headers,
  12 licence files, zero difference). The qb-version-and-feature skew gate in
  `qbmModuleConfig.cmake.in` is untouched.
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
  space.** `qb/vendor/ev/` → `qb/vendor/qev/`, `ev.h`/`ev++.h` → `ev.h`/`ev++.h`
  (likewise `ev.c`, `ev_vars.h`, `ev_wrap.h`, the generated `ev_config.h` and the backend
  sources), so a consumer writes `#include <qb/vendor/qev/ev++.h>`. The CMake target `ev` is `qev`,
  `qb::ev` is `qb::qev`, `ev::ev` is `qev::qev`, and the archive is `libqev.a`. All 58 exported
  `ev_*` symbols — and the types and macros they are built from — are `ev_*`.

  This fixes a **silent** failure: qb's event loop and a system libev define the same 58 names, so
  linking both into one binary succeeded with **exit 0 and no diagnostic** and the winner was decided
  by command-line order alone. `-fvisibility=hidden` cannot close it, because `ev++.h`'s inline
  members put the undefined `ev_*` references in the *consumer's* object file; only renaming does.

  **Not renamed, deliberately:** the 24 `event_*` functions and libevent's `struct event` members
  (`ev_fd`, `ev_events`, `ev_callback`, …) — those *are* libevent's API, and `event.h`,
  `event_compat.h`, `event.c` keep their names for the same reason. So `libqev.a` exports 57 owned
  `ev_*` functions, `ev_default_loop_ptr`, and 24 unowned `event_*`: a chosen exposure that only
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
- **New CI job `ubuntu-abi-sentinel-sweep`** (in `cmake.yml`), driving the new
  [`scripts/check-cross-compiler-statics.sh`](./scripts/check-cross-compiler-statics.sh). It compiles
  one probe translation unit with **both** `clang++` and `g++`, lists every function-local entity out
  of each object file (`_ZZ…`, `_ZGVZ…`), and fails when a symbol present in one is the other's symbol
  plus an ABI-tag suffix. That is the only check that can see the `B5cxx11` class fixed above: the
  matrix cannot, however many rows it grows, because every row compiles the tree with ONE toolchain
  and the defect only exists where two meet in one binary — and macOS/libc++ has no cxx11 tag at all.
  The job carries its own negative control: it reverts both sentinels in a scratch copy, requires the
  sweep to exit **exactly 1** with a divergence reported, and restores. Requiring 1 rather than
  "non-zero" is deliberate — a botched revert that merely breaks the probe exits 2, and scoring that
  as a detection is a false pass that was measured while writing the job. Seconds, no qb build.
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

- **BREAKING — the vendored `modules/nlohmann/json.hpp` (25,712 lines). qb no longer ships a copy
  of nlohmann/json, and a consumer must now provide one.** This is a public-contract change,
  because `qb::json` **is** `nlohmann::json`: the type crosses qb's API (`qb/json.h` also defines
  `to_json`/`from_json` for `qb::uuid`), so the dependency was always the consumer's too — it was
  merely hidden while qb happened to install a header for them.

  *Why.* The vendored file declared `NLOHMANN_JSON_VERSION_MAJOR/MINOR/PATCH = 3/12/0` but was an
  untagged post-3.12.0 `develop` snapshot: **450 added / 264 removed lines** against the `v3.12.0`
  tag (`git diff --numstat` against `single_include/nlohmann/json.hpp` at that tag). nlohmann
  guards against version mixing with an inline namespace whose name encodes the version, and that
  machinery was **byte-identical** to the real 3.12.0's — so our copy emitted the *same*
  `nlohmann::json_abi_v3_12_0` tag over a *different* set of definitions. A program linking qb and
  a genuine 3.12.0 therefore got one inline namespace spanning two definition sets: an ODR
  violation no linker diagnoses, because the guard that exists to turn version mixing into a link
  error instead certified the two as identical. The label lied, so the protection did not protect.
  Deleting the copy removes the lie; a `FetchContent`ed one is by construction the version its
  macros claim.

  *And the include root.* `<prefix>/include` is now exactly **`qb`** (`qb qbm` for the workspace
  package) on every host. It previously gained an `nlohmann/` directory or not depending on whether
  the *build* machine happened to have a system `nlohmann_json` — a host-dependent installed
  surface a packager cannot control, and the collision that made `brew link` fail against a
  distro's own `nlohmann-json` package. `nlohmann` is a name qb has no business claiming in a
  consumer's include namespace.

  *Migration.* Install nlohmann/json (`brew install nlohmann-json`,
  `apt install nlohmann-json3-dev`, `vcpkg install nlohmann-json`, …) before configuring qb, and —
  if you consume an installed qb — make it resolvable to `find_package(qb)`, whose generated config
  now calls `find_dependency(nlohmann_json 3.11)` **unconditionally**. No source change is required:
  `qb::json` and every alias in `qb/json.h` are unchanged, and `#include <nlohmann/json.hpp>` still
  resolves — from the real package rather than from qb's prefix. Builds that only compile and test
  need do nothing: with no system copy, `AUTO` fetches the pinned `v3.12.0`
  (`QB_NLOHMANN_GIT_TAG`). Producing an *installable* qb does require a real system package, and
  says so at configure time rather than failing later: a fetched nlohmann belongs to no export set,
  so `install(EXPORT qbTargets)` would abort with `requires target "nlohmann_json" that is not in
  any export set`, and installing its headers would simply put `nlohmann/` back in the include
  root. The same reasoning zlib already carried in `qbDependencies.cmake`.

  Two consequences worth noting for packagers: `share/licenses/qb/third-party/nlohmann/LICENSE.MIT`
  is gone (qb no longer redistributes the header, so it has no notice obligation to discharge —
  the [THIRD-PARTY-NOTICES](./THIRD-PARTY-NOTICES) entry went with it, and the vendored-unit count
  in `scripts/check-vendor-attribution.py` drops from 8 to 7), and the
  `share/qb/abi-fingerprint.txt` field `system_nlohmann=<bool>` is replaced by
  `nlohmann=<version>` — the boolean could no longer vary, while the version both varies and is
  what a packager needs, since nlohmann encodes it in that inline namespace.

- **All four installed `.tpp` headers — `<qb/core/Actor.tpp>`, `<qb/core/VirtualCore.tpp>`,
  `<qb/core/Pipe.tpp>` and `<qb/core/Main.tpp>`. Source-incompatible for anyone who includes one
  by name.** `.h` is now the only header extension in qb. Not one line of code was deleted: every
  template body moved, byte-for-byte, into the `.h` that declares it — `VirtualCore.tpp` and
  `Main.tpp` to the tails of `VirtualCore.h` and `Main.h`, `Pipe.tpp` to the tail of `Pipe.h`, and
  `Actor.tpp` to the tail of **`VirtualCore.h`**, not `Actor.h`.

  That last one is the whole point, and it is why a second file never was the fix. Sixteen of
  `Actor.tpp`'s bodies name `VirtualCore::` in a nested-name-specifier and so need a *complete*
  `qb::VirtualCore`; `VirtualCore.h` includes `Actor.h`, so an `#include "VirtualCore.h"` at
  `Actor.h`'s tail is a guard no-op and the class is still incomplete there. What a `.tpp` was
  reaching for was never a file — it was a **position**, and the position is the tail of the
  header that closes the cycle. Two supporting moves fall out of the same reasoning:
  `service_event_type` moved from `Actor.h:141-142` to `Event.h:696` (Pipe.h is included *from*
  `Actor.h:50`, 91 lines before the concept was declared, so a body in Pipe.h could never see it),
  and `Pipe.h` gained `<qb/system/event/router.h>`, which `Pipe.tpp` had silently borrowed from
  its includer — it carried no includes at all.

  **Migration.** A consumer that includes only `<qb/core/Actor.h>` and calls `push<E>()` already
  failed to link before this change, and still does; what changes is the remedy. `#include
  <qb/core/Actor.tpp>` becomes `#include <qb/core/VirtualCore.h>` — or, better, include an
  umbrella (`<qb/actor.h>`, `<qb/main.h>`, `<qb/patterns.h>`), which is the supported entry point
  and needs no edit at all. Every umbrella was rewired here, so nothing in qb, qbm or examples
  changed at a call site.

  **There was never a multiple-definition defect to preserve.** All 41 definitions across the four
  files are templates, i.e. vague linkage: a forced-instantiation probe emitted 136 weak-external
  and exactly one strong-external symbol per TU, and that one was the probe's own function. Zero
  hits for "multiple definition" / "duplicate symbol" across 802 commits. What *is* real is
  latent, and it survives the rename: these bodies are reached by both `libqb-core`'s single
  amalgamated TU and every consumer TU, and an include guard is per-TU. One non-template,
  non-`inline` definition added to them is an instant duplicate symbol — measured again on the
  merged header, two consumer TUs plus the archive: `duplicate symbol 'qb::actor_tpp_helper(int)'`
  → `ld: 1 duplicate symbols`, where the same two TUs link clean without it. The `.tpp` extension
  used to be the signal that only templates go there; a banner at each merge site says so now, and
  because a comment is not a guard, **`scripts/check-header-linkage.py`** enforces it: no shipped
  header may define a strong external symbol, over all 267 headers rather than only the six merge
  sites. `inline` is not a workaround — it makes the link succeed and leaves N definitions of one
  entity in the program, the identity-duplication class this release spent a whole step fixing.

  Verified by **linking**, not by `-fsyntax-only` or `-c` (both of which pass on precisely this
  defect): against an installed prefix, 143 headers each compile alone and all 18 entry points
  compile *and link* under `-I -Wall -Wextra -Werror -Wundefined-func-template`. The matrix keeps
  a `<qb/main.h>`-first entry point on purpose — the `Actor` cycle failure is invisible from
  `<qb/actor.h>` and only appears through `<qb/main.h>`. `qb_core_VirtualCore_h.cpp` now ODR-uses
  `push<E>` and `registerEvent<E>` so the new contract is asserted by a TU whose only qb include
  is that header, rather than only through the four umbrellas.

  `install(... PATTERN "*.tpp")` **stays**, but as a net rather than a dependency: `qbm-http`'s
  `routing/router.tpp` — the last `.tpp` in the tree — was merged into `router.h` in the same
  release, so the pattern now matches nothing. It is kept because the rule is shared with
  `qb_register_module()` and the failure it prevents is silent at configure *and* install time,
  surfacing only in a downstream consumer's first translation unit.

- **The unprefixed socket-portability macros, from the default configuration.** `closesocket`,
  `ioctlsocket`, `SD_RECEIVE`, `SD_SEND`, `SD_BOTH`, `SD_NONE`, `FD_TO_SOCKET` and
  `OPEN_FD_FROM_SOCKET` are no longer defined by `<qb/io/config.h>` unless
  `QB_LEGACY_SOCKET_MACROS` is defined; use the `QB_`-prefixed spellings. An `#ifndef` guard was
  measured to be **insufficient** for this set, which is why the default flipped rather than merely
  gaining a guard: `closesocket` and `ioctlsocket` are *function* names, so a consumer who writes
  `static int closesocket(int)` passes `#ifndef closesocket` and then has every one of their calls
  rewritten to `close` by an object-like macro — their function was never entered. Nothing in qb,
  qbm or examples used the unprefixed spellings.
- **`<qb/vendor/qev/event.h>` and `<qb/vendor/qev/event_compat.h>` from the default install.** They
  declare libevent's API over an incompatible `struct event_base`, and the bodies behind them are
  only built under `QB_EV_LIBEVENT_COMPAT=ON`, which also reinstates the headers.


- **`<cube.h>`**, the legacy whole-framework umbrella header (guard `QB_QB_H`; just `qb/actor.h`,
  `qb/io.h` and `qb/main.h`). **This removes a name from the installed public surface.** It was dead —
  nothing in the tree ever included it — and it was the last generic top-level name in the installed
  include root, which is now exactly `qb/` — the bundled nlohmann copy that used to add a second name
  there was deleted in this same release, and the `install-consume` CI job now accepts that one value
  and no other. qb has no whole-framework convenience header:
  include the entry points directly (`qb/actor.h` + `qb/main.h` for qb-core, `qb/io.h` for qb-io).
- **`qb/io/system/sys__inet_compat.inl`**, and with it the `#if QB__HAS_NTOP` fallback branch in
  `qb/io/system/sys__socket.h` that declared `inet_ntop` / `inet_pton` for it. The bundled fallback
  was dead and unbuildable three times over: its `#include` resolved from no include path, it opened
  `qb::inet::ip::compat` rather than the `qb::io::inet::ip::compat` that declared the functions, and
  its only gate (`QB__HAS_NTOP == 0`) is reachable solely on a pre-Vista Windows target that cannot
  host C++20 — so the declarations could only ever have produced a link error. Every platform qb
  builds on provides both functions natively, and `qb::io::inet::ip::compat` is now a pure re-export.
  The file sat in the public header tree but was never shipped (qb's install rule matches `*.h`,
  `*.hpp`, `*.tpp` and `*.inl` — `qbPackage.cmake:161-165`; the last two now match nothing, this
  release having retired both extensions). `QB__HAS_NTOP` itself is kept as a published
  feature-test macro.
- **`SO_NOSIGPIPE` is no longer defined on Linux.** `<qb/io/config.h>` — an **installed public
  header** — carried `#if defined(__linux__)` / `#define SO_NOSIGPIPE MSG_NOSIGNAL`: a socket
  *option* name bound to a message *flag* value. Linux has no such option; measured, the resulting
  `setsockopt(SOL_SOCKET, 0x4000, …)` returns `-1` / `ENOPROTOOPT`. The failed call was never the
  damage. The portable idiom forks on the name — `#ifdef SO_NOSIGPIPE` → `setsockopt` per descriptor,
  `#else` → `MSG_NOSIGNAL` per call — so defining it sent a Linux consumer down the BSD branch and
  away from the one mechanism that works there, reproducing in the consumer exactly the SIGPIPE hole
  fixed above. `SO_*` is reserved to `<sys/socket.h>` besides. **Source-incompatible** for code that
  names `SO_NOSIGPIPE` on Linux, which now fails to compile — the truthful outcome, since the option
  does not exist. Nothing in qb read it there (both `defined(SO_NOSIGPIPE)` sites in the tree already
  spell `&& !defined(__linux__)`), so no behaviour changes on any platform.
- `std::to_string(const uuids::uuid &)` from the vendored stduuid header — a local addition, not
  upstream. Adding a *declaration* to namespace `std` is undefined behaviour ([namespace.std]/2
  permits only an explicit specialisation for a program-defined type, which is what the neighbouring
  `std::hash<uuids::uuid>` is), and it was never needed: `uuids::to_string` is found by ADL for an
  unqualified call, and every call site in the tree already spells it `uuids::to_string`.
  **Source-incompatible** for code that called `std::to_string(uuid)`.

### Fixed

- **The cross-core relocation guard aborted intermittently on payloads that are perfectly
  relocatable.** `qb-core-test-system-shutdown-saturation` aborted **2/30** standalone on Linux
  (Debug, libstdc++, no sanitizer) with
  `assert(false && "qb: event payload is not trivially relocatable")`. It was the guard, not the
  payload. `SharedCoreCommunication::send`'s `event_points_into_itself` has to scan the whole
  `bucket_size * 64` range — that is what cross-core delivery `memcpy`s — but an event's payload
  does not WRITE that whole range, and the unwritten bytes come out of an outbound pipe that is
  rewound and reused event after event over heap memory the allocator recycles. Instrumented and
  measured on the aborting runs: a 64-byte `HeavyEvent` whose live members end at offset 52, with
  the offending word at **offset 40** (the dead tail of a heap-backed `std::string`'s inline SSO
  buffer, `[40,48)`) and at **offset 56** (tail padding after the last member, `[52,64)`), while the
  payload's only pointer — the string's `data()` at offset 16 — correctly addressed the heap.
  Every event-construction site (`Pipe::push`, `Pipe::allocated_push`, `VirtualCore::push<T>`,
  `VirtualCore::send<T>`) now prepares the bucket range through `qb::detail::prepare_event_storage`
  before the payload is constructed into it, so the only way a word inside the range can address the
  range is if the payload wrote it — which is exactly the hazard. Debug-only: the guard is
  `#ifndef NDEBUG`, and release must not pay a per-event `memset` for a check it does not compile
  in. Pinned by `RelocatablePayload.EventStorageIsPreparedSoDeadBytesCannotTripTheGuard`, which
  poisons a pipe slot, asserts the next event lands in the *same* slot, and requires its dead bytes
  to read zero — 12/12 poison bytes before the fix, 0 after. Soak after the fix: **0/30** standalone
  and **0/20** under load on both `dev-cxx23` and `feature-gates`. Why nothing else saw it: `release`
  compiles the guard out, ASan's deterministic allocation fill masks the stale bytes, and macOS
  libc++ recomputes a short string's `data()` from `this`, so the whole class is structurally
  invisible there.
- **`listener::clear()`'s deferred-queue drain had no covering test.** The swap-before-drop fix was
  right by construction — releasing a deferred closure runs arbitrary destructors, and a captured
  `shared_ptr` whose teardown calls `defer()` again is a `push_back` into the `std::deque` that
  `clear()` is halfway through destroying — but reverting it left the whole macOS `sanitize` suite
  green, so a future revert would have landed silently. `qb-io-test-unit-listener-clear-reentrant-defer`
  counts captured state instead of waiting for a crash: on libstdc++ the re-entrant closure is
  constructed into storage the same `clear()` then deallocates without destroying it, so it leaks
  with no diagnostic.
- **`QB_BUILD_BENCHMARKS=ON` with `QB_BUILD_TESTS=OFF` built zero benchmarks and reported success.**
  The benchmarks live *under* `tests/core/benchmark` and `tests/io/benchmark`, and those trees were
  `add_subdirectory`'d only inside `if (QB_BUILD_TESTS)`, so `qb_add_benchmark` was never reached.
  The configure summary still printed `- Benchmarks: ON` and `Google Benchmark: TRUE`, and the
  dependency was still fetched and linked — the build paid for it and produced nothing. No preset
  could see it, because the one preset that enables benchmarks (`benchmarks`) also has tests on.
  The gate is now `QB_BUILD_TESTS OR QB_BUILD_BENCHMARKS`, with the unit/system trees carrying their
  own `QB_BUILD_TESTS` gate. Measured: 0 executable targets before, **45** after.
- **A `QB_INSTALL=OFF` build still installed one qb header into the consumer's prefix.**
  `src/qb/vendor/qev/CMakeLists.txt` emitted `install(FILES ${QB_EV_CONFIG_OUT} ...)` for the
  generated `ev_config.h` outside any `QB_INSTALL` guard — the only `install()` in the whole
  embedded-qb subtree that sat outside it. So `cmake --install` of a `QB_INSTALL=OFF` build (every
  dev preset, and every parent reaching qb via `add_subdirectory()`/`FetchContent`) silently
  deposited `include/qb/vendor/qev/ev_config.h`, exit 0, no diagnostic — precisely the leak the
  option exists to prevent, and the host-probed generated header is the worst single file to leak.
  Measured: 1 file installed before, **0** after.
- **`find_package(qb)` permanently polluted the consumer's `CMAKE_MODULE_PATH`.**
  `qbConfig.cmake.in` appended its own directory for the Argon2 and QUIC `Find` modules and never
  restored it (twice, when both features were on), with no dedup. Because the append ran *during*
  `find_package(qb)`, qb's entry sat **ahead of** any module dir the consumer appended afterwards —
  so a consumer's own `FindArgon2.cmake` / `FindNgtcp2.cmake` was silently shadowed by qb's for the
  rest of their configure. Exit 0, no diagnostic. This is the same defect `qbmModuleConfig.cmake.in`
  was written to prevent and which CI already gated — but that gate covered `qbm-httpConfig.cmake`
  only, and this one leaks on the **success** path. `package-consume.yml` now gates
  `qbConfig.cmake` too, with the same negative control (delete the guard, require the probe to
  catch it).
- **`qb_check_cpp_features()` probed at the compiler's default standard, and one wrong answer
  shipped in the package's public interface.** `CMAKE_CXX_STANDARD` is only set globally when qb is
  top-level, and nothing set `CMAKE_REQUIRED_*`, so in every embedded build — which includes this
  superproject and every `FetchContent` consumer — the seven `check_cxx_source_compiles` probes
  compiled with no `-std=` at all, i.e. C++14 on Apple Clang and GCC. Six of the seven failed for
  want of a flag rather than a feature (`error: no member named 'optional' in namespace 'std'`);
  the survivor, `QB_HAS_STRING_VIEW` (libc++ exposes `<string_view>` pre-C++17), then reached
  consumers inside `qbTargets.cmake`'s `INTERFACE_COMPILE_DEFINITIONS`. Two hosts with different
  default standards therefore produced packages with different public compile definitions. The
  probes now run at `QB_CXX_STANDARD`, set in function scope so nothing leaks.
- **Multi-config generators: a hard error in the superproject, a silent bogus cache standalone.**
  `set_property(CACHE CMAKE_BUILD_TYPE ...)` is a hard error when the cache entry does not exist,
  which killed every `Ninja Multi-Config` / `Xcode` configure of the superproject outright. In
  *standalone* qb the same missing guard was silent instead: the multi-config test used
  `CMAKE_CONFIGURATION_TYPES`, which the generator does not populate until `project()` runs — and
  this file is `include()`d before it — so the guard was always true and qb wrote
  `CMAKE_BUILD_TYPE=Release` into a cache that also carried
  `CMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo`, exactly the state its own comment says
  must never happen. Both now test the `GENERATOR_IS_MULTI_CONFIG` global property, which *is* set
  pre-`project()`. The per-config output-directory loop, dead for the same reason, works again.
- **`QB_WITH_LOGGING=OFF` with `QB_STDOUT_LOGGING=ON` did not compile.** Each macro in the
  `QB_STDOUT_LOGGING` branch of `io.h` was defined with a trailing semicolon, so
  `if (…) QB_LOG_INFO(x); else` expanded to `if (…) stmt;; else` and orphaned the `else`:
  `VirtualCore.cpp: error: expected expression`. The sibling no-op branch already used
  `do {} while (false)`, which is why plain `QB_WITH_LOGGING=OFF` built. All five macros now use the
  same `do/while` form. Blast radius measured, not assumed: exactly one call site across qb, all
  three qbm modules and `examples/`. `QB_WITH_LOGGING=OFF` alone also emitted two
  `-Wunused-lambda-capture` warnings on Apple Clang, where the captured `this` is read only by a
  log statement that compiles to nothing; both are gone.
- **`cmake/FindArgon2.cmake` could never satisfy a version request, and searched hard-coded host
  prefixes with no hints.** Upstream declares the version as an *enum member*
  (`ARGON2_VERSION_NUMBER = ARGON2_VERSION_13`), not a `#define`, so the old regex matched nothing
  on any real installation, `ARGON2_VERSION_STRING` was always empty, and `-- Found Argon2: `
  printed a blank version. An empty `VERSION_VAR` is not benign:
  `find_package_handle_standard_args` treats it as unsuitable for *any* version request. Note the
  header value is the argon2 **hash-format** version (0x13 = format v1.3) and says nothing about
  which release is installed — decoding it through the packed-hex major/minor/patch arithmetic
  yields `0.0.19`, a triple that corresponds to nothing. The module now takes the real library
  version from **pkg-config** (`libargon2.pc` → e.g. `20190702`), which is the only place it
  exists, and exposes the format version separately as `ARGON2_FORMAT_VERSION`. pkg-config also
  supplies `HINTS`, so a sandboxed package build binds the argon2 it actually declared instead of
  whatever sits in `/opt/homebrew`; the hard-coded `PATHS` remain as a last resort for hosts
  without pkg-config, where the release version is honestly reported as unknown rather than
  guessed. `FindNgtcp2.cmake` and `FindNghttp3.cmake` already resolved this way — this module was
  the holdout.
- **`scripts/check-abi-fingerprint.sh` failed on Linux + GCC, on a defect that does not exist.**
  The `exceptions` axis cannot be flipped under g++, which rejects `throw` under `-fno-exceptions`
  even in code it will not instantiate, so the mismatch build dies in `vendor/qev/ev++.h` *before
  the link the check is about*; clang accepts the unreached `throw` and proceeds. That shape landed
  in the "failed, but not with the symbol" arm and reported FAIL, so the whole script exited 1 and
  the `linux-gcc` lane was red. It is now an explicit SKIP with the reason named — which is what
  this script's own contract already promised — and the axis remains required and armed: a
  `-fno-exceptions` consumer that does not reach `ev++.h` compiles fine and then depends on the
  link symbol. Measured: 8 PASS on clang, 7 PASS + 1 SKIP on g++-14.
- **Standalone qb with default options could not configure on a host without a system zlib.**
  Standalone defaults `QB_INSTALL=ON` and `QB_DEPS_FETCH_FALLBACK=ON`, so zlib was built from
  source — and a source-built `zlibstatic` belongs to no export set, so `install(EXPORT qbTargets)`
  aborted at generate time with `target "zlibstatic" that is not in any export set`, naming neither
  zlib, nor qb, nor a way out. That is the documented `cmake --install` → `find_package(qb)` route
  failing on a bare machine. The condition is now detected where it happens and reported with the
  three ways out (install a system zlib, `-DQB_WITH_COMPRESSION=OFF`, or `-DQB_DEPS_FETCH_FALLBACK=OFF`).
- **`QB_MODULE_LIBRARIES` never reached the caller.** `qb_register_module()` wrote it with
  `PARENT_SCOPE`, but `qb_load_modules()` calls `add_subdirectory()` from *inside a function*, so
  the write landed in that function's frame and evaporated on return: with all three qbm modules
  loaded, the configuration summary still printed `Available libraries: qb-io;qb-core`. It now
  accumulates in a `GLOBAL` property and `qb_load_modules()` hands the list back and reports it —
  `Registered modules: qbm-http;qbm-pgsql;qbm-redis`.

- **`io/async/tcp/connector.h` ran three `#include` directives inside `namespace
  qb::io::async::tcp`.** `<coroutine>`, `<optional>` and `<chrono>` sat at `:640-642`, under
  `#ifdef __cpp_impl_coroutine`, between the braces of the namespace opened at `:69` — measured
  brace depth 1. An `#include` is a textual splice, so those directives declared
  `qb::io::async::tcp::std`, not `::std`. They were harmless only because an earlier header had
  already pulled the same three in, so each include guard fired and the misplaced directive was a
  no-op: an ordering accident, not a guarantee. Compiler-confirmed latent rather than live in both
  states — `qb::io::async::tcp::std::coroutine_handle<>` is rejected (`no member named 'std' in
  namespace 'qb::io::async::tcp'`, with clang's note pointing at the *global* `std` that
  `<unordered_set>` had already opened) while `::std::coroutine_handle<>` compiles. This is the same
  defect qbm-pgsql's `transaction_coro.inl` carried, where deleting the sibling `<fstream>` was
  measured to reparse it inside the namespace and emit 20 errors led by *did you mean
  `::std::basic_streambuf`?* — the `.tpp`/`.inl` elimination fixed that one as a side effect and
  left this one, which is the argument for a check rather than a merge.

  The three move to the top include block outside the namespace, `<coroutine>` keeping its
  `#ifdef` guard. Confirmed free: the preprocessed token stream is **identical** (351 693 tokens
  before and after), and every one of the 22 differing tokens is a bare integer — `__LINE__`
  expansions moving with the file, zero non-numeric differences. The instrument was controlled
  first by deleting `<chrono>` from the top block (299 260 token lines changed) and restoring
  byte-exact.

  `scripts/check-namespace-scoped-includes.py` now enforces this across qb, all three qbm modules
  and `examples/` (942 files, 6 832 directives), with `extern "C"` blocks and global-scope class-body
  X-macro splices deliberately allowed, vendored trees deliberately out of scope, per-surface
  anti-vacuous floors, and a brace-depth confidence check. It runs in `format-check.yml` and in the
  superproject's `dev/agent/verify.sh`, with negative controls that replant both real defects.

- **`script/qb-new-module.sh` cloned a repository that does not exist, and then destroyed the
  user's working directory.** Two defects, one of which is the reason the other went unnoticed.
  The clone URL was `isndev/qbm-sample` (`gh repo view` → *Could not resolve to a Repository*) while
  the `cd` two lines later named `qb-sample-module`, which is the repository that actually exists —
  a typo that survived because the two were separate string literals. And neither scaffolding script
  had `set -e`: with the clone failed, the `cd` failed, `cd ../${NAME}` failed, and `mkdir .git;
  mv * .git` then ran against the **invocation** directory, moving everything the user had there
  into a `.git` folder — exiting **0**, so a caller saw success. Reproduced end to end before the
  fix, and re-run after it: the directory and its contents survive, and the script stops at the
  failed clone. Both scripts now `set -euo pipefail`, derive the clone / `cd` / cleanup paths from a
  single `TEMPLATE` variable so they cannot drift apart again, refuse to run when the target or the
  template directory already exists, and exit non-zero (2) when the name argument is missing instead
  of `0`. This matters most for `qb-new-project.sh`, which `README.md` documents as
  `curl … | bash /dev/stdin MyProject` — i.e. it runs in whatever directory the user is standing in,
  and it took the identical path any time its own clone failed.

  **A second round found three more defects of the same shape — exit 0, wrong result — that
  `set -e` structurally cannot catch, because no command in them fails.** (1) `git --bare init`
  takes HEAD from the user's `init.defaultBranch`, while the push named `master` because that is the
  template's branch. On a machine configured with `init.defaultBranch=main`, HEAD pointed at a
  branch that did not exist, `git reset --hard` succeeded against an unborn HEAD without writing a
  file, `git submodule update` found no `.gitmodules`, and the script printed `Created 'MyProject'`
  and exited **0** over a directory containing nothing but `.git`. Reproduced under both settings.
  (2) `rm -rf "../${TEMPLATE}"` ran from inside `${NAME}`, so its target depended on where `${NAME}`
  had put us: a name containing a separator (`../foo`) moved the deletion one directory up onto an
  unrelated `qb-sample-project`. (3) Nothing removed the half-built directories when a later step
  failed, so the freshly added guards then refused the retry the user obviously wanted.

  Both scripts now validate the name as a plain directory name, resolve the single target path once
  and absolutely, and `trap` a cleanup that removes only what they created and only on failure. The
  bare-repo dance is gone with them: `git clone` into the target followed by `git remote remove
  origin` reaches the same result — full template history, no upstream remote — in two commands that
  cannot run anywhere else, take HEAD from the source rather than from `init.defaultBranch`, and
  create nothing outside the target, so there is no intermediate template directory to collide with.
  `mkdir .git; mv * .git`, the primitive that did the original damage, no longer appears in either
  file. Each script then **asserts** the result is non-empty and prints the file count instead of an
  unconditional `Created`, which is what turns this whole class loud; the assertion was positively
  controlled against a template whose only commit is empty. An intermediate `git init` + `git fetch
  +refs/heads/X:refs/heads/X` form was rejected on measurement: `fetch` refuses to write the branch
  HEAD is on, so it inverted defect 1 rather than fixing it, failing whenever `init.defaultBranch`
  happened to *match* the template's branch. Both scripts also take `QB_TEMPLATE_REF` to pin the
  template clone.

- **The two scaffolding templates are two major versions behind the framework, and every
  documentation surface describing them said otherwise.** Measured, not inferred:
  `qb-sample-project` pins qb at a commit from before **v2.0.0** and is internally consistent with
  it, so it configures *and builds* — which is why nothing ever complained. Repointed at qb `main`
  (v2.6.0) or `develop` (3.0.0) it fails to compile, first on `#include <http/http.h>` (3.0 spells
  it `<qbm/http/http.h>`), then on `bool onInit()` where the base has returned
  `qb::io::async::task<bool>` since before 2.6.0, then on `router().get()`, `qb::http::date::now()`
  and a `qb::json` initializer; configured the way its own README instructs
  (`-DQB_BUILD_TEST=ON` — the option has been `QB_BUILD_TESTS` for some time) it does not even
  configure, because `test/CMakeLists.txt` calls a `cxx_gtest()` that no longer exists.
  `qb-sample-module` has not been touched since **2019** and fails at line 3 of its own
  `CMakeLists.txt`: `qb_register_module()` reports `SOURCES is required for non-header-only
  modules`, and adding `HEADER_ONLY` only advances it to the next failure, which is that 3.0 expects
  a module's public headers under `src/qbm/<name>/` rather than in a flat `actor/ event/ service/`
  tree. `README.md` called the first "buildable" and the second "the layout `qb_load_modules`
  expects, with the `qb_register_module` call already wired"; `readme/6_guides/getting_started.md`
  called the result "a buildable CMake project with qb wired in and ready to extend". All three
  claims are now replaced by what was measured, with a pointer to the embed-in-an-existing-project
  path and, for modules, to the three real modules and their `.github/ci/superbuild/CMakeLists.txt`.
  Nothing builds either template in CI, in any repository, which is why none of this was caught.
- **The documented crypto surface described three functions that have never existed, and gave
  `ecies_encrypt`'s return pair in the wrong order.** `readme/3_qb_io/utilities.md` listed
  `ecdh_derive_secret` under *Key agreement* and `envelope_encrypt`/`envelope_decrypt` under
  *Hybrid encryption*; none is declared in `crypto.h` in any version, so a caller got a compile
  error against documentation. (`EnvelopeFormat` is real, and is consumed by nothing — which is how
  the invention looked plausible.) The real key agreement is `x25519_key_exchange`; the AEAD-with-
  metadata pair is `encrypt_with_metadata`/`decrypt_with_metadata`, now documented with a citation
  and a worked example. Separately, `ecies_encrypt` returns `{ephemeral_public_key, ciphertext}`
  (`crypto_asymmetric.cpp:718`) while `ecies_decrypt` takes the ciphertext **first**, so the pair is
  deliberately not in call order — the reference said the opposite, and following it throws
  `Failed to create public key from raw bytes` at run time. Found by compiling and running the
  documented snippet rather than by reading it; every corrected snippet on that page now round-trips
  against an installed prefix. No library code changed.
- **qb did not compile at `-std=c++23`.** `CoroutineScheduler::owned_current_` is a
  `static inline thread_local std::unique_ptr<CoroutineScheduler>` — a `unique_ptr` to the very
  class it is a member of, so it is instantiated while that class is incomplete. C++23 made
  `~unique_ptr` `constexpr` (P2273R3), which makes libc++ and libstdc++ instantiate its body
  eagerly and reach `default_delete`'s `static_assert(sizeof(_Tp) >= 0)`:
  `invalid application of 'sizeof' to an incomplete type`. **45 of the 134** public headers stopped
  compiling on their own — `qb/main.h`, `qb/actor.h`, `qb/io/async.h` and every coroutine header —
  on AppleClang 21/libc++, clang++-19/libstdc++ and clang++-21/libstdc++, while g++-14 and every
  compiler at `-std=c++20` stayed green. A deleter declared before the class and defined after it
  (`qb::io::async::detail::scheduler_deleter`) removes the eager instantiation; every symbol,
  including the weak-external TLS descriptor `QB_ABI_ANCHOR` exists to preserve, is byte-identical
  to before. The `dev-cxx23` preset is now part of the release gate, which is what makes this
  class of defect visible at all: `release` and `feature-gates` are both C++20.
- **`register_type_id` published a stack address into the process-wide type-id registry.** The
  3.0.0 regression test for the registry passed a `type_id_slot` with **automatic** storage, and
  `register_type_id` links `&slot` into a list that outlives the call, so every later registration
  walked a dangling node. It passed only because that test ran last in declaration order:
  `--gtest_shuffle` gave `rc=139` (SIGSEGV) on 3 of 6 seeds. ASan was silent, because the reader
  is in the un-instrumented archive. Fixed in the test, and the contract is now **checked** rather
  than only documented — in Debug on POSIX, `register_type_id` asserts that `&slot` is outside the
  calling thread's stack before publishing it, with a death test and a matching negative control
  pinning that the check fires.

- **The event-id space forked under `-fvisibility=hidden`, and two distinct types received the same
  id.** `qb/core/Event.h` already documented that a type-id collision "silently breaks event routing
  (two distinct types collapse to the same slot in `router::memh`)"; this is that outcome, reachable
  without qb doing anything wrong. Measured on a host + `dlopen`ed plugin, plugin compiled
  `-fvisibility=hidden`: the host held `KillEvent=1, SignalEvent=2` while the plugin drew
  `KillEvent=1, Noop=2`. Both runs exited 0 with no diagnostic. Two things were needed. `QB_ABI_ANCHOR`
  restores coalescing for `_type_id_counter`, a namespace-scope `inline` variable — but **not** for
  the per-type magic static inside `type_id_for<T>()`: a block-scope static cannot be annotated back
  into the export trie, and measured, the hidden plugin exports **0** of the 8 magic statics its
  default-visibility twin exports while `__attribute__((visibility("default")))` on the variable *or*
  the function is accepted without a warning and changes nothing. So the magic static stopped being
  the identity: it now caches an id owned by `_type_id_registry`, a shared list keyed by
  `typeid(T).name()`. After the fix the hidden plugin reports `KillEvent=1, Noop=7` — identical to
  the default-visibility control. This also covers the case no annotation can reach: two separate
  copies of `libqb-core.a`, each with its own `Event.cpp` name table.


- **A gcc-built consumer linked against a clang-built qb corrupted the heap on the first empty
  `qb::unordered_map` it destroyed.** `free(): invalid pointer`, SIGABRT, no link-time diagnostic.
  When the key or value carries libstdc++'s cxx11 ABI tag (any `std::string`), clang appends
  `B5cxx11` to the mangled name of a function-**local static** and gcc does not, so ska's empty-table
  sentinel — `sherwood_v10_entry::empty_pointer()`, plus `sherwood_v3_entry::empty_default_table()`
  behind `qb::unordered_flat_map` — did not merge across the two objects. Every empty table points at
  that sentinel and `deallocate_data()` decides whether to free by comparing against it, so a table
  created on one side of the boundary and destroyed on the other took the free branch and handed the
  allocator static storage. Both sentinels now keep the tagged type out of the static: the v10 one is
  an `inline static` data member (mangled from the class template's arguments, which both compilers
  spell identically, and still constant-initialized), the v3 one keeps its function-local static but
  gets tag-free storage — raw bytes plus placement new — because hoisting *it* would have cost
  constant initialization and traded a cross-compiler bug for a static-initialization-order one.
  **macOS/libc++ has no cxx11 tag and is structurally blind to this**, which is why it survived; the
  new `ubuntu-abi-sentinel-sweep` CI job below is the only check that can see the class.
- **`send_n()` / `recv_n()` re-introduced the SIGPIPE that `send()` guards against.** They declared
  `flags = 0` while `send()`, `recv()`, `sendto()` and `recvfrom()` all default it to `MSG_NOSIGNAL`,
  and `send_n` forwards its flags straight into `socket::send()` — so the wrapper's `0` *overrode* the
  primitive's own default. A single write to a peer that had closed raised `SIGPIPE`, whose default
  disposition terminates the process. Not Linux-only, contrary to the folklore: current Darwin defines
  and honours `MSG_NOSIGNAL` (`0x80000`) too. It read as benign because `io/tcp/ssl/init.cpp` installs
  a process-wide `signal(SIGPIPE, SIG_IGN)` — with `QB_WITH_SSL=OFF` nothing catches it, so the new
  cases reset `SIGPIPE` to `SIG_DFL` in a forked child rather than trusting that initialiser.
  The same shape was then found in the descriptor **acquisition** paths: `open()` and `accept_n()` set
  the BSD/macOS socket-level `SO_NOSIGPIPE` backstop, but `socket(socket_type)` and
  `operator=(socket_type)` — how `socket::accept()` and `tcp::listener::accept()` produce their socket
  — did not. The three `setsockopt` sites are now one `suppress_sigpipe()` helper, so a new
  descriptor-producing path cannot silently skip it. The two guards are not interchangeable and both
  are needed: `setsockopt(SO_NOSIGPIPE)` on a socket whose peer has already closed fails with
  `EINVAL`, so the backstop alone does not rescue `send_n`.
- **The vendored `qev` fork kept upstream libev's include guards, so a consumer could not use both.**
  `ev.h` still said `EV_H_`, `ev++.h` `EVPP_H__`, the libevent-compat `event.h` `EVENT_H_`. `ev.h`
  is installed and `<qb/main.h>` pulls it in transitively (`core/Actor.h` → `io/async/coroutine.h` →
  `coroutine/scheduler.h` → `ev++.h`), so any binary that also used a real libev broke in **both**
  include orders — with qb first `ev_default_loop` is undeclared, with libev first the errors land
  inside qb's own `ev++.h` — and there was no workaround short of splitting the two APIs across
  different `.cpp` files. Every guard is now named after the fork: `QEV_H_`, `QEVPP_H_`,
  `QEV_EVENT_H_`, `QEV_EVENT_COMPAT_H_`, `QEV_WRAP_H`, `QEV_WEPOLL_H_`, `QEV_CONFIG_H_`. No
  conditional anywhere reads any of those names, so the rename is inert beyond the guards. The
  path-redirect macros `EV_H`, `EV_EVENT_H` and `EV_CONFIG_H` — no trailing underscore, set by CMake
  as `PUBLIC` compile definitions — are deliberately untouched. This completes the `ev_*` → `ev_*`
  rename recorded under *Changed*: the C symbols moved first, the header-level coexistence was left
  unfinished. The 24 `event_*` libevent-compat symbols still collide at link time, still deliberately.
- The documented return convention for the TLS `connect*` / `n_connect*` family said a failed peer
  verification "surfaces as `qb::io::SocketStatus::CertificateError` (value 1)". It does not, and
  never did: verification failure fails the handshake inside `handCheck()`, which disconnects and
  returns `-1`, indistinguishable at this level from any other handshake error. `CertificateError` is
  declared in the enum and returned by nothing in the tree. The enumerator stays (it is public
  surface); `readme/3_qb_io/ssl_transport.md` now states what actually happens and points at
  `SSL_get_verify_result()` for callers who need to tell the cases apart.
- Three shipped `#include` directives named files that do not exist, on every platform:
  `<qb/io/async/epoll.h>` (an **installed public header**) included `"../helper.h"`, deleted in
  `581094a9` -- the header has been uncompilable ever since, and because the missing include was
  also the only thing defining `__WIN__SYSTEM__`, its `#error "epoll is not available on windows"`
  guard could never fire. `qb/io/system/sys__socket.h` included `"qb/socket.cpp"` under
  `QB_HEADER_ONLY`; the implementation has always been the sibling `sys__socket.cpp`, so
  `-DQB_HEADER_ONLY` failed outright. The vendored `ev.c` carries upstream libev's dangling
  `#include "ev_iocp.c"` (renamed `ev_iocp.c`): upstream never shipped that file and neither does
  qev, and `EV_USE_IOCP` is reachable from no qb configuration, so the branch now says so with an
  `#error` instead of reporting a missing file.
- `<qb/uuid.h>` included `<qb/vendor/uuid/include/uuid.h>` *above* its own `QB_UUID_H` include guard,
  leaving the vendored include outside the guard it was meant to sit behind.
- **`<qb/main.h>` alone could not link `qb::Actor::push<E>` / `qb::Pipe::push<E>`.** `Main`'s own
  bodies reach a complete `qb::Actor` with every member template *declared*, while Actor's bodies were
  reached only by `qb/actor.h`, `qb/patterns.h` and `qb/core/patterns.h`. A TU whose only qb include
  was the engine umbrella compiled clean and failed at link. `qb/main.h` now pulls
  `core/VirtualCore.h`, the same position and for the same reason as `qb/actor.h:18`.
  `<qb/core/Actor.h>` alone still does **not** carry the template bodies, and that stays deliberate:
  they need a complete `qb::VirtualCore`, and `VirtualCore.h` is what drags `<windows.h>`,
  `WIN32_LEAN_AND_MEAN` and `NOMINMAX` into a TU. The umbrellas are the entry points; the class
  headers are not. (Stated in terms of the `.tpp` files through the release drafts; those are gone —
  see *Removed* — and the bodies now sit at the tail of `VirtualCore.h`.)
- **`<qb/io/async/coroutine/task.h>` alone could not destroy a `task<T>`.**
  `defer_frame_destruction` / `forget_frame_if_current` were declared here **without `inline`** and
  defined `inline` in `scheduler.h`: a formal ODR mismatch between TUs that see one declaration and
  TUs that see both, and it silenced the `-Wundefined-inline` that would have said so. Both are now
  `inline`, and task.h pulls `scheduler.h` from its **tail** — after `task<T>` is complete, inside the
  guard, which is the one position that works in both include orders. `inline` alone was necessary but
  **not** sufficient: an inline function must be *defined* in every TU that uses it. `~task<T>` is
  called by every consumer that owns a task, and `generator.h` hit the same wall.
- **18 installed headers were not self-contained** — each compiled only because something else came
  first, and each now carries the one include it needs, named in a comment:
  `coroutine/cancellation.h` (`combinators.h` — `when_any` / `timeout_error`),
  `coroutine/channel.h` (`<any>`), `coroutine/generator.h` (`task.h`, `<vector>`, `<memory>`),
  `coroutine/shared_task.h` (`utils.h` — `coro_scheduler()`), `coroutine/scope.h` and
  `coroutine/stream.h` (`sync.h` — `semaphore`), `coroutine/sync.h` and `async/tcp/server.h`
  (transitively), `async/io_handler.h` (`<qb/utility/type_traits.h>` — `qb::has_on`),
  `system/container/ring_buffer.h` (`<cstddef>`, `<iterator>`), and
  `core/patterns/{discovery,supervisor}.h` (`VirtualCore.h`, which carries Actor's template bodies —
  both instantiate `push<E>` / `push_to<E>` in a non-dependent context).
  `ring_buffer.h` and `generator.h` are **libstdc++-only**: libc++ supplies `ptrdiff_t` and
  `std::shared_ptr` transitively and libstdc++ does not, so no amount of macOS testing could have
  found them. Both came out of the Linux leg.

## [2.6.0] - 2026-08-02

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

[Unreleased]: https://github.com/isndev/qb/compare/v3.0.0...HEAD
[3.0.0]: https://github.com/isndev/qb/compare/v2.6.0...v3.0.0
[2.6.0]: https://github.com/isndev/qb/releases/tag/v2.6.0
