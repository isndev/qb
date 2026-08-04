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
`<qb/vendor/qev/qev++.h>`) has now landed, and so has the qbm public include prefix — as
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
- **The qev fork's 35 `HAVE_*` autoconf macros are private to `libqev.a`.** `qev_config.h` is a
  public header (`qev.h` needs its `EV_USE_*` half, and `qev.h` is reached from `<qb/io.h>`), so it
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
  this change (`dev/analysis/EVENT-ID-ABI-3.0.md` §1) rather than re-derived here. Debug also
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
  used to be the signal that only templates go there; the banner at each merge site now is.
  `inline` is not a workaround — it makes the link succeed and leaves N definitions of one entity
  in the program, the identity-duplication class this release spent a whole step fixing.

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
  `qev.h` still said `EV_H_`, `qev++.h` `EVPP_H__`, the libevent-compat `event.h` `EVENT_H_`. `qev.h`
  is installed and `<qb/main.h>` pulls it in transitively (`core/Actor.h` → `io/async/coroutine.h` →
  `coroutine/scheduler.h` → `qev++.h`), so any binary that also used a real libev broke in **both**
  include orders — with qb first `ev_default_loop` is undeclared, with libev first the errors land
  inside qb's own `qev++.h` — and there was no workaround short of splitting the two APIs across
  different `.cpp` files. Every guard is now named after the fork: `QEV_H_`, `QEVPP_H_`,
  `QEV_EVENT_H_`, `QEV_EVENT_COMPAT_H_`, `QEV_WRAP_H`, `QEV_WEPOLL_H_`, `QEV_CONFIG_H_`. No
  conditional anywhere reads any of those names, so the rename is inert beyond the guards. The
  path-redirect macros `EV_H`, `EV_EVENT_H` and `EV_CONFIG_H` — no trailing underscore, set by CMake
  as `PUBLIC` compile definitions — are deliberately untouched. This completes the `ev_*` → `qev_*`
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
  `-DQB_HEADER_ONLY` failed outright. The vendored `qev.c` carries upstream libev's dangling
  `#include "ev_iocp.c"` (renamed `qev_iocp.c`): upstream never shipped that file and neither does
  qev, and `EV_USE_IOCP` is reachable from no qb configuration, so the branch now says so with an
  `#error` instead of reporting a missing file.
- `<qb/uuid.h>` included `<qb/vendor/uuid/include/uuid.h>` *above* its own `QB_UUID_H` include guard,
  leaving the vendored include outside the guard it was meant to sit behind.
- **`<qb/main.h>` alone could not link `qb::Actor::push<E>` / `qb::Pipe::push<E>`.** `Main.tpp` reaches
  a complete `qb::Actor` with every member template *declared*, and the bodies live in `Actor.tpp`,
  which only `qb/actor.h`, `qb/patterns.h` and `qb/core/patterns.h` pulled. A TU whose only qb include
  was the engine umbrella compiled clean and failed at link. `qb/main.h` now pulls `core/Actor.tpp` +
  `core/Pipe.tpp`, the same position and for the same reason as `qb/actor.h:15`.
  `<qb/core/Actor.h>` and `<qb/core/VirtualCore.h>` still do **not** carry the template bodies, and
  that stays deliberate: `Actor.tpp` needs a complete `qb::VirtualCore`, and `VirtualCore.h` is what
  drags `<windows.h>`, `WIN32_LEAN_AND_MEAN` and `NOMINMAX` into a TU. The umbrellas are the entry
  points; the class headers are not.
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
  `core/patterns/{discovery,supervisor}.h` (`Actor.tpp` — both instantiate `push<E>` / `push_to<E>` in
  a non-dependent context).
  `ring_buffer.h` and `generator.h` are **libstdc++-only**: libc++ supplies `ptrdiff_t` and
  `std::shared_ptr` transitively and libstdc++ does not, so no amount of macOS testing could have
  found them. Both came out of the Linux leg.

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
