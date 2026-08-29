<!-- Verified-against: qb 3.0.1 (C++20 default, C++23 supported) -->

# Versioning and compatibility

qb follows [Semantic Versioning 2.0.0](https://semver.org/). The version is defined in
`cmake/qbConfig.cmake` (`QB_FRAMEWORK_VERSION`) and consumed by `project()` in `CMakeLists.txt` — that
file is the single source of truth, and every other version string in the tree is a copy of it.

**`main` and `develop` both report 3.0.0, and no `v3.0.0` tag exists yet** — so a build from either
branch advertises a version that is not yet tagged. The latest *tagged* release remains **2.6.0**.
`main` was fast-forwarded to `develop` on 2026-08-11 in preparation for the release; until the tag
lands, the default clone is a pre-release 3.0.0. That release is a **major** one — see [CHANGELOG.md](./CHANGELOG.md) for what makes it major.

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

## Source compatibility

Guaranteed within a major version, as described above. Code that compiles against `3.0` keeps
compiling against every later `3.x`; anything that would require a source edit waits for `4.0`.

The installed CMake package version file is generated with `COMPATIBILITY SameMajorVersion`, so
`find_package(qb 3.0)` accepts any installed `3.x` and rejects a different major. That check is
about *source*: it answers "is this qb new enough to compile my code?". It is not, and cannot be,
a statement about binaries — see the next section, and note in particular that a `find_package`
that succeeds can still be followed by a link failure.

## Binary compatibility

**qb promises no binary compatibility between any two versions, in either direction, including
between `3.0.0` and `3.0.1`.** Every object file in a program — yours, qb's, and any static library
of your own that exposes a qb type — must be compiled against the same qb it links against.
Upgrading qb means rebuilding all of it. There is no drop-in replacement.

That is stronger than the usual "ABI stability is not guaranteed" disclaimer, and it is deliberate:
qb would rather be checked than trusted. **qb's own version is an axis of a link-time fingerprint**,
so the promise is enforced rather than merely documented. A consumer compiled against `3.0.0`
headers and linked against a `3.0.1` archive does not run and misbehave; it fails to link, naming
the version it was compiled with.

### What may change in a patch release

Everything except the source API. A patch release may change the layout of any type, the body of
any inline function or template, the size of any private member, and the order or identity of
anything not part of the documented public API. qb is a heavily templated, CRTP-based, partly
header-only framework: most of what a consumer compiles is qb's headers, so most changes are
layout-visible by construction. Several build options additionally change code generation on their
own — `QB_ENABLE_NATIVE_ARCH`, `QB_ENABLE_LTO`, `QB_ENABLE_FAST_MATH`, and any sanitizer.

### The fingerprint: five axes, enforced at link time

`src/qb/utility/abi.h` gives the archive one symbol per axis, named after the value **it** was
compiled with, and makes every consumer translation unit that parses a qb header reference one
symbol per axis named after the value **it** is being compiled with. Same configuration, the
references resolve; different configuration, the link fails and the undefined symbol names the axis.
There is no opt-out macro: each axis is a configuration in which the two sides are provably unsound
together, so the remedy is to rebuild qb, not to silence the check.

An axis qualifies only if a mismatch is simultaneously *possible* in a build that otherwise compiles,
*silent* under every tool, and *unsound*. Five qualify:

| Axis | Symbol | What differs when it is violated |
|---|---|---|
| qb version | `qb_abi_version_M_m_p` | anything (see above) — and it is the axis that catches a consumer compiled with a hand-written `-I`/`-l` line instead of qb's CMake usage requirements |
| cache line | `qb_abi_cacheline_N` | `KNOWN_L1_CACHE_LINE_SIZE` re-lays out `qb::Event` and the coroutine frame pool |
| exceptions | `qb_abi_exceptions_[01]` | `-fno-exceptions` forks inline bodies the archive also defines |
| coroutine debug | `qb_abi_coroutine_debug_[01]` | `QB_DEBUG_COROUTINES` grows `task<T>::promise_type` |
| jthread source | `qb_abi_std_jthread_[01]` | a standard library without `__cpp_lib_jthread`, or `QB_COMPAT_FORCE_THREAD_FALLBACK`, resizes `qb::jthread` and moves every member after it in `qb::Main` and `qb::VirtualCore` |

Measured against an installed `3.0.0` prefix (macOS 26 / arm64, AppleClang 21), one axis moved per
row, everything else held equal:

```
consumer configuration                     result
-----------------------------------------  -------------------------------------------------------
3.0.0, cacheline 64, coro-debug off        LINK OK                                    <- control
qb 3.0.1  (patch bump)                     undefined: _qb_abi_version_3_0_1
qb 3.1.0  (minor bump)                     undefined: _qb_abi_version_3_1_0
qb 4.0.0  (major bump)                     undefined: _qb_abi_version_4_0_0
hand-written -I/-l, no usage requirements  undefined: _qb_abi_version_unknown__compile_with_qb_s_…
-DQB_DEBUG_COROUTINES                      undefined: _qb_abi_coroutine_debug_1
-DKNOWN_L1_CACHE_LINE_SIZE=128             undefined: _qb_abi_cacheline_128
-DQB_COMPAT_FORCE_THREAD_FALLBACK          undefined: _qb_abi_std_jthread_0
-fno-exceptions                            undefined: _qb_abi_exceptions_0
-DNDEBUG                                   LINK OK    (not an axis — see below)
-std=c++23                                 LINK OK    (not an axis — layout is identical)
```

On MSVC and clang-cl the same axes are additionally emitted as `#pragma detect_mismatch` records,
which the Microsoft linker reports as `LNK2038` naming **both** values. That pragma is an inert
no-op on Mach-O and ELF, so it is a bonus where it works and never the mechanism.

### What the fingerprint does *not* protect

**It is a detector for five specific silent misconfigurations, not a general ABI checker.** It
compares qb's configuration against qb's configuration. It cannot see, and does not claim to see:

- **The compiler, or its version.** See the next section — this is the one with a worked example.
- **The standard library.** libstdc++ against libc++, or two libstdc++ versions, are ABI axes the
  standard libraries arbitrate with their own tags; qb does not duplicate that.
- **Standard-library hardening or debug modes** (`_GLIBCXX_DEBUG`, `_LIBCPP_HARDENING_MODE`, …).
- **`NDEBUG`** — deliberately excluded, and that exclusion is qb's obligation rather than yours.
  A consumer's `NDEBUG` comes from its own `CMAKE_BUILD_TYPE`, including the *unset* default, which
  defines none; a Debug consumer against a Release qb is a supported and CI-tested configuration, so
  fingerprinting `NDEBUG` would turn the default consumer configuration into a hard link failure.
  What qb owes in exchange is that no public type, member declaration or alignment in a shipped
  header is selected by `NDEBUG` (or `_DEBUG`/`DEBUG`) — the disagreement would be invisible to the
  linker *and* to `find_package`, surfacing as silent memory corruption. Two such splits existed and
  were removed in 3.0 (`qb::unordered_map`/`unordered_set`, and `qb::Event::id_type`);
  [`scripts/check-abi-macro-split.py`](./scripts/check-abi-macro-split.py) fails CI on a new one.
  One residual is **open and stated rather than papered over**: `assert()` sites inside `inline` and
  template bodies in shipped headers mean two translation units that disagree about `NDEBUG` emit two
  bodies under one vague-linkage symbol, and object order alone decides which survives — so a
  mismatched consumer can silently lose an assertion. Do not mix `NDEBUG` across translation units;
  [the building guide](./readme/7_reference/building.md) says so where a user will hit it.
- **Optimization, LTO, `-march`, or sanitizers.** Matching those is your build system's job.
- **Feature switches** (`QB_HAS_SSL`, `QB_HAS_QUIC`, `QB_HAS_COMPRESSION`, `QB_WITH_LOGGING`). These
  gate whole types rather than members of a type that exists either way, so a mismatch is a *compile*
  error, not silent corruption — and the genuinely silent case, a hand-written `-I`/`-l` consumer,
  is caught by the version axis.

### Mixing compilers is still your problem

The compiler is part of the "same configuration" requirement, not just the standard library, and the
fingerprint cannot check it. The worked example is real and is a 3.0 fix rather than a hypothetical:
clang appends the cxx11 ABI tag (`B5cxx11`) to the mangled name of a function-**local static** whose
type carries it, and gcc does not. A local static that the code treats as one shared object — a
sentinel compared by address, a cache whose identity matters — then exists twice in a mixed binary,
with no link-time diagnostic at all. In qb's case an empty `qb::unordered_map` created on one side
and destroyed on the other freed static storage (see *Fixed* in [CHANGELOG.md](./CHANGELOG.md)).

qb's own headers are clean of that class and the `ubuntu-abi-sentinel-sweep` CI job keeps them that
way — it compiles a probe with both compilers and diffs symbol names, and carries a negative control
so a green result means "the two compilers agree" rather than "the probe stopped instantiating".
**The guarantee stops at qb's surface.** Your own code and your other dependencies are not swept.
Build the whole program with one compiler.

Note also that macOS is structurally blind to this class: libc++ has no cxx11 tag, so a
macOS-only workflow cannot reproduce it however many configurations it tries.

### qb ships static-only

The installed prefix contains static archives and nothing else — `libqb-core.a`, `libqb-io.a`,
`libqev.a`, and one `.a` per qbm module. There is no shared build to consume: `SOVERSION` is set
exactly once in the whole tree and it is on the vendored event-loop fork, never on qb's own targets;
`QB_BUILD_SHARED_LIBS` exists but `BUILD_SHARED_LIBS` appears in no CI workflow, so a shared qb has
never been configured anywhere. Treat it as unsupported for the 3.0 series. What that implies:

- **Upgrading qb is a rebuild of your program, not a swap of a file.** This is the practical form of
  the no-binary-compatibility promise, and it is why the version axis is not an inconvenience: with
  static linkage there is no scenario in which replacing qb without recompiling was going to work.
- **No parallel installs.** Without `SOVERSION` there is no distro-style side-by-side of two qb
  versions in one prefix.
- **A few qb entities must exist exactly once per process** — the event type-id counter, the
  `ServiceActor` registry, the per-thread event loop and `VirtualCore`, the coroutine frame pool, and
  a null-object sentinel compared by address. They are vague-linkage entities that the dynamic linker
  coalesces across images, which `-fvisibility=hidden` would otherwise switch off; qb annotates them
  `visibility("default")` so a host executable plus a `dlopen`ed plugin that both statically link qb
  still share one of each. Without that annotation two event types silently receive the same id and
  the router sends them to the same slot. The annotation is empty on Windows, where a shared qb needs
  export-macro work first.

### Diagnosing a mismatch

1. The link error already names the axis and the value *your* translation unit was compiled with —
   for example `_qb_abi_cacheline_128`, `referenced from: qb::detail::abi_fingerprint in main.o`.
2. The archive's side of the story needs no demangler and no qb source tree:

   ```sh
   nm -g <prefix>/lib/libqb-io.a | grep qb_abi
   strings <prefix>/lib/libqb-io.a | grep '^qb-abi '
   ```

3. A prebuilt prefix also records its configuration as a file — `share/qb/abi-fingerprint.txt`, read
   back out of the archive rather than re-derived, so it cannot drift from the artefact. Comparing two
   prefixes is a `diff`. See [INSTALL.md](./INSTALL.md).
4. **Consume qb through `find_package` or `add_subdirectory`.** A hand-written `-I`/`-l` line misses
   qb's usage requirements — not only the version, but the feature macros — and yields the
   `qb_abi_version_unknown__compile_with_qb_s_cmake_usage_requirements` symbol by design.

## Deprecation policy

When an API is superseded:

1. It is marked deprecated and documented in the [CHANGELOG](./CHANGELOG.md) under *Deprecated*, with the
   replacement.
2. It keeps working for the remainder of the current major series.
3. It may be removed only in the next major release, listed under *Removed*.

A migration path is provided for every removal. See, for example, the time-type change described in
[the migration guide](./readme/6_guides/migration_guide.md).

## Supported versions

**One series is maintained at a time: the latest minor of the current major.** When `3.0.0` is
released it becomes that series, and `2.6.x` stops receiving releases — including for fixes that
already exist on `develop`. There is no backport commitment to the previous major and no separate
long-term-support branch; this section will be updated if one is introduced.

That is a deliberate policy rather than an oversight, and it has a visible consequence worth stating
plainly: a fix merged after `2.6.0` was tagged is reachable only by moving to the next release, and
for `3.0.0` that means a major upgrade with the source-breaking changes listed under *Removed* in
[CHANGELOG.md](./CHANGELOG.md). Security fixes are the one place this could bite hardest, so
[SECURITY.md](./SECURITY.md) states the same policy in the same words rather than implying a
broader guarantee.

## C++ standard and toolchains

qb requires C++20 by default (`QB_CXX_STANDARD=20`, extensions off) and also supports
`QB_CXX_STANDARD=23` for newer standard-library implementations. CMake 3.24 is the minimum. Supported
compilers and platforms are listed in [INSTALL.md](./INSTALL.md). A change to the minimum required
standard or toolchain is treated as a breaking change and reserved for a major release.
