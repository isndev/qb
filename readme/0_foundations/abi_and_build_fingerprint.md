# ABI, the cache line, and the build fingerprint

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported) — ef7d3ea7

qb ships as public headers plus a compiled archive, and several of those headers change the *layout* of public types according to macros the **consumer** sets. This page is the machinery that turns that class of silent corruption into a link error you can read — plus the cache-line constant everything is laid out on, and the rest of `qb/utility/`.

**Prerequisites:** none — **See also:** [Foundations overview](./README.md) · [The pipe](./buffers.md) · [Containers](./containers.md) · [Building qb](../7_reference/building.md)

## The problem this exists to solve

Two translation units can each be internally consistent, link cleanly, run — and disagree about the size of a type they both name.

That is not a hypothetical. `KNOWN_L1_CACHE_LINE_SIZE` is a documented public knob, and on a host whose real `hw.cachelinesize` is 128 it is the honest value to set. Measured on this checkout, setting it moves three things at once:

| | as shipped | `-DKNOWN_L1_CACHE_LINE_SIZE=128` |
|---|---|---|
| `QB_LOCKFREE_CACHELINE_BYTES` | 64 | 128 |
| `sizeof(qb::Event)` | 64, align 64 | 128, align 128 |
| `CoroutineFrameAllocator::kAlign` | 64 | 128 |

A consumer compiled with the knob against an archive compiled without it therefore disagrees with the archive about which pool bucket a coroutine frame belongs to, and about how wide an event is. Nothing catches that: not the compiler (each translation unit is internally consistent), not the linker (vague-linkage bodies merge silently and the winner is decided by link order). The program compiles, links, runs, and corrupts memory. The recorded symptom is `AddressSanitizer: heap-buffer-overflow WRITE of size 200`, exit 134, with zero diagnostics before that point (`qb/src/qb/utility/abi.h:20-30`).

## The mechanism: the symbol's *name* is the configuration

The archive **defines** one symbol per axis, named after the value it was compiled with. Every consumer translation unit that parses a qb header **references** one symbol per axis, named after the value *it* is being compiled with:

```cpp
QB_ABI_USED QB_ABI_RETAIN inline const void *const abi_fingerprint[] = {
    &QB_ABI_SYM_VERSION, &QB_ABI_SYM_CACHELINE, &QB_ABI_SYM_EXCEPTIONS,
    &QB_ABI_SYM_CORO_DEBUG, &QB_ABI_SYM_STD_JTHREAD
};
```
<!-- src: qb/src/qb/utility/abi.h:364-366 -->

Equal configuration, the references resolve. Different configuration, the link fails and names the axis and *this* translation unit's value. Reproduced here by compiling one object at 128 and the other at 64:

```
Undefined symbols for architecture arm64:
  "_qb_abi_cacheline_128", referenced from:
      qb::detail::abi_fingerprint in main.o
```

The array is named `abi_fingerprint` on purpose: the linker prints it on the `referenced from:` line, and grepping for it is the one search that leads to `qb/utility/abi.h` and its explanation.

The archive's side of the story is one command away, and needs no demangler:

```sh
nm -g <prefix>/lib/libqb-io.a | grep qb_abi        # -> _qb_abi_cacheline_64
strings <prefix>/lib/libqb-io.a | grep '^qb-abi '  # -> qb-abi qb=3.0.0 cacheline=64 exceptions=1 …
```
<!-- src: qb/src/qb/utility/abi.h:47-50 -->

Both were run here against the object that defines them; the second prints `qb-abi qb=3.0.0 cacheline=64 exceptions=1 coroutine_debug=0 std_jthread=1`, which is the answer to "what was this built with?" in one line. The definitions live in `qb/src/qb/io/abi.cpp:46-50`, so every archive carries them.

**There is no opt-out macro, deliberately.** Every axis below is a configuration in which the two sides are provably unsound together; the fix is to rebuild qb with the same setting, not to silence the check (`qb/src/qb/utility/abi.h:58-60`).

Two implementation details are load-bearing rather than decorative, and both were measured rather than assumed:

- **`__attribute__((used))`** is what makes the check per-translation-unit rather than per-optimiser-whim. Without it, a constant-initialised, never-read pointer array is discarded at `-O1` and above, and a lone oddly-configured translation unit slips through (`qb/src/qb/utility/abi.h:247-252`).
- **`__attribute__((retain))` is not optional on ELF.** `used` only stops the *compiler* from dropping the array; GNU ld's `--gc-sections` discards the unreferenced section before it resolves the relocations inside it, so every mismatched configuration linked cleanly under `-ffunction-sections -fdata-sections -Wl,--gc-sections` — with clang-19 *and* g++-14, ld 2.44. That is a common consumer setting, so without `retain` the check would be silently defeated for exactly the builds most likely to use it (`qb/src/qb/utility/abi.h:254-261`).

On MSVC and clang-cl the same axes are additionally emitted as `#pragma detect_mismatch` records, which the Microsoft linker reports as `LNK2038` naming **both** values (`qb/src/qb/utility/abi.h:385-393`). That pragma is a verified no-op on Mach-O and ELF, so it is a bonus on Windows, never the mechanism. It goes through `_Pragma` rather than `#pragma` because a `#pragma` line's arguments are not macro-expanded — both sides of the comparison would otherwise read `QB_ABI_STR(...)` and could never differ (`qb/src/qb/utility/abi.h:378-384`).

## The five axes

An axis qualifies only if a difference between archive and consumer is (a) *possible* in a build that otherwise compiles and links, (b) *silent* — no diagnostic from any tool, and (c) *unsound* — it changes the layout of a public type, or the body of an entity the archive also defines (`qb/src/qb/utility/abi.h:63-68`).

| Axis | Symbol | What differs |
|---|---|---|
| qb version | `qb_abi_version_M_m_p` | installed headers and archive are one unit, and nothing else detects skew. It is also the axis that catches a consumer compiled with **no** qb CMake usage requirements at all |
| cache line | `qb_abi_cacheline_N` | `sizeof(qb::Event)` 64 → 128, `CoroutineFrameAllocator::kAlign` 64 → 128 |
| exceptions | `qb_abi_exceptions_[01]` | `-fno-exceptions` forks 8 inline bodies, two of which the archive also defines |
| coroutine debug | `qb_abi_coroutine_debug_[01]` | `QB_DEBUG_COROUTINES` grows `task<T>::promise_type` 32 → 40 |
| jthread source | `qb_abi_std_jthread_[01]` | `qb::jthread` 16 → 24, moving every member after it: `qb::Main` 88 → 96, `qb::VirtualCore` 8648 → 8656 |

<!-- src: qb/src/qb/utility/abi.h:69-77 -->

Each value is derived **once**, in `abi.h`, from the raw knob — and qb's own switch is then defined *from* that value: `QB_LOCKFREE_CACHELINE_BYTES` is `QB_ABI_CACHELINE_BYTES` (`qb/src/qb/utility/prefix.h:66`), and `QB_COMPAT_HAS_STD_JTHREAD` is `QB_ABI_STD_JTHREAD` (`qb/src/qb/utility/compat.h:60`). The fingerprint therefore cannot drift from the thing it fingerprints, because there is only one derivation (`qb/src/qb/utility/abi.h:147-151`).

The version comes from qb's CMake usage requirements, never from a literal written in the header. A translation unit compiled without them — a hand-written `-I`/`-l` line — is not merely missing the version, it is also missing `QB_HAS_SSL` / `QB_HAS_QUIC` / `QB_HAS_COMPRESSION`, and gets a program whose inline feature answers contradict the archive's out-of-line ones. So that case gets a symbol whose *name* says what to do about it:

```
"_qb_abi_version_unknown__compile_with_qb_s_cmake_usage_requirements", referenced from:
```

which is what a compile against the source tree with a bare `-Isrc` produces, verbatim (`qb/src/qb/utility/abi.h:226-232`).

### What is deliberately excluded

Each exclusion must be re-argued before it changes (`qb/src/qb/utility/abi.h:79-120`):

- **`NDEBUG`** — measured to change **no** layout, and kept that way by `qb/scripts/check-abi-macro-split.py`. A Debug consumer against a Release archive is a *supported, CI-tested* configuration, and an unset `CMAKE_BUILD_TYPE` — CMake's default — is one of the three the install-consume workflow builds. Putting `NDEBUG` in the fingerprint would turn the default consumer configuration into a hard link failure.

  There is a residual here and it is **open, not fixed**: 39 `assert(` sites and 7 `#if*NDEBUG` blocks still sit inside `inline` and template bodies in shipped headers, so two translation units that disagree about `NDEBUG` emit two bodies under one vague-linkage symbol and **object order alone** decides which survives. Measured with the same two objects on macOS/ld-prime and Linux/GNU ld 2.44: `main.o tu_dbg.o` exits 0, `tu_dbg.o main.o` exits 134. It stays open because every candidate fix costs more than it saves — the constraint a user can act on is written where a user will see it, under "Do not mix `NDEBUG` across translation units" in [Building qb](../7_reference/building.md).
- **`__cpp_rtti`** — `-fno-rtti` cannot compile qb at all (`Event.h` uses `typeid`). Already loud, at the first translation unit.
- **`-std=c++20` vs `-std=c++23`** — measured: identical layout for every public type, `qb::expected` included. qb exports `cxx_std_20` as a *minimum* and builds both.
- **`QB_HAS_SSL` / `QB_HAS_QUIC` / `QB_HAS_COMPRESSION` / `QB_WITH_LOGGING`** — they gate whole types, never a member of a type that exists in both configurations, so a mismatch is a *compile* error rather than silent corruption.
- **`QB_ENABLE_UDS`** — defined unconditionally by `qb/io/config.h`, so it cannot differ between two translation units.
- **Standard-library hardening / debug modes** (`_GLIBCXX_DEBUG`, `_LIBCPP_HARDENING_MODE`, …) — real ABI axes, but not qb's to arbitrate; the standard libraries carry their own ABI tags.
- **Architecture, pointer size, target triple** — the linker already refuses those.

## `QB_ABI_ANCHOR`: entities that must exist exactly once per process

A handful of qb entities are *identity* rather than data: the event type-id counter, the per-type magic static that draws from it, the router's disposer table, the `ServiceActor` registry, the per-thread `listener` and `VirtualCore`, the coroutine frame pool, and the `no_protocol()` null-object sentinel — which is compared **by address**.

Every one of them is a vague-linkage entity: an `inline` variable, a static data member of a class template, or a function-local `static`. Vague linkage means "N definitions, the linker keeps one", and *which* linker does the keeping matters. The static linker folds them within one image; the dynamic linker coalesces the remaining weak definitions **across** images. That second step is what `-fvisibility=hidden` turns off (`qb/src/qb/utility/abi.h:284-294`).

The failure it produces is exact and silent. Host executable plus a `dlopen`ed plugin, both statically linking qb, plugin compiled `-fvisibility=hidden` (`RTLD_LOCAL` and `RTLD_GLOBAL` alike):

```
 plugin default visibility (control)   plugin -fvisibility=hidden
 &_type_id_counter 0x100e18000 (both)  &_type_id_counter 0x1021b0000 / 0x10744c158
 host: KillEvent=1  SignalEvent=2      host:   KillEvent=1  SignalEvent=2
 plug: KillEvent=1  Noop=7             plug:   KillEvent=1  Noop=2   <-- COLLISION
```
<!-- src: qb/src/qb/utility/abi.h:298-306 -->

Two distinct event types hold id 2, so the router sends them to the same slot. Both runs exit 0 and print nothing. `visibility("default")` on the anchor restores the coalescing, measured on the same harness with the plugin still compiled `-fvisibility=hidden`.

The attribute has a **second** job that decides whether an anchor can be shared at all. A `thread_local` defined out of line in a `.cpp` emits a `non-external` TLS descriptor — private to its image by construction — so two images that each link `libqb-io.a` get two per-thread event loops on the *same* thread, and neither dyld nor the linker says a word. The same entity defined `inline` in a header emits a **weak-external** descriptor that dyld coalesces. That is why these anchors live in headers rather than in `.cpp` files, and why moving one back would silently reintroduce the split (`qb/src/qb/utility/abi.h:310-321`).

`QB_ABI_ANCHOR` is empty on MSVC: qb annotates no symbol for `__declspec(dllimport)` today, so a Windows *shared* build needs the export-macro work first. On ELF and Mach-O it is exactly the annotation `-fvisibility=hidden` requires (`qb/src/qb/utility/abi.h:324-337`).

## The cache line: `qb/utility/prefix.h`

```cpp
#define QB_LOCKFREE_CACHELINE_BYTES    QB_ABI_CACHELINE_BYTES        // 64 by default
#define QB_LOCKFREE_EVENT_BUCKET_BYTES QB_LOCKFREE_CACHELINE_BYTES

struct QB_LOCKFREE_CACHELINE_ALIGNMENT    CacheLine   { uint32_t __raw__[…]; };
struct QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT EventBucket { uint32_t __raw__[…]; };
```
<!-- src: qb/src/qb/utility/prefix.h:66-68, :123-140 -->

`CacheLine` and `EventBucket` are padding structs sized to exactly one line, usable as a base class or a member to guarantee an object starts on a line boundary. The alignment macros behind them expand to `alignas(...)` under GCC/Clang and `__declspec(align(...))` under MSVC (`qb/src/qb/utility/prefix.h:72-73`, `:90-91`). Two more macros are set per architecture: `QB_LOCKFREE_PTR_COMPRESSION` where the platform has spare virtual-address bits, and `QB_LOCKFREE_DCAS_ALIGNMENT` for 128-bit CAS (`__x86_64__`, `__aarch64__`).

### How one constant reaches the event ceiling

`EventBucket` is the unit an event is measured in, and the mailbox ring's capacity is derived so a bucket count fits a 16-bit field. So the cache-line constant propagates all the way to a user-visible limit — measured here at both settings:

| | 64-byte line | 128-byte line |
|---|---|---|
| `sizeof(EventBucket)` | 64 | 128 |
| `SharedCoreCommunication::MaxRingEvents` = `uint16_t::max() / bucket` | **1023** | 511 |
| `VirtualCore::kMaxDeliverableBuckets` (widest event that can cross a core) | 1023 buckets | 511 buckets |
| that ceiling, in bytes | 65 472 | 65 408 |
| a default `pipe<EventBucket>` at rest | 256 KiB | 512 KiB |

An event wider than the ceiling can never be enqueued into a peer mailbox, because the ring enqueue is all-or-nothing. The flush does not retry it: it logs at `LOG_CRIT`, disposes the event and skips it (`src/qb/core/VirtualCore.cpp:335-338`). That is the practical face of "keep events small" — the number is 1023 buckets, and it comes from here.

Doubling the cache line also doubles every pipe's resting allocation, which is the other half of the [pipe memory profile](./buffers.md#memory-it-grows-and-it-does-not-come-back).

## The rest of `qb/utility/`

Four small headers round out the layer. None of them is a facility you go looking for; all four are things you will read in a stack trace or a compile error.

**`compat.h` — the C++20/23 seam.** `qb::jthread`, `qb::stop_source` and `qb::stop_token` alias the standard ones when the library advertises `__cpp_lib_jthread`, and fall back to qb's own implementations otherwise; `QB_COMPAT_FORCE_THREAD_FALLBACK` forces the fallback (`qb/src/qb/utility/compat.h:60-68`). `qb::expected` / `qb::unexpected` do the same against `<expected>` (`qb/src/qb/utility/compat.h:241-244`). Both routes to the fallback are fingerprinted, by construction — see the jthread axis above. The header also carries `qb::byteswap` (used by [`qb::endian`](./encoding.md#byte-order)) and `qb::to_underlying` (`qb/src/qb/utility/compat.h:211-215`).

**`build_macros.h` — platform detection and the export macro.** Defines `__WIN__SYSTEM__` / `__LINUX__SYSTEM__`, sets `WIN32_LEAN_AND_MEAN` and `NOMINMAX` before any Windows header, and supplies `QB_API` / `QB_GET` — `__declspec(dllexport|dllimport)` under `QB_DYNAMIC` on Windows, empty elsewhere (`qb/src/qb/utility/build_macros.h:32-33`, `:61-74`). It is also where `QB__THROW` / `QB__NO_EXCEPTIONS` branch on `__cpp_exceptions`, the same macro the ABI exceptions axis reads, so the two cannot disagree.

**`branch_hints.h` — optimiser hints.** `qb::likely(expr)` and `qb::unlikely(expr)` wrap `__builtin_expect` and return the expression unchanged on compilers that lack it (`qb/src/qb/utility/branch_hints.h:40-63`). `QB_ASSUME(cond)` is the standard C++23 `[[assume]]` where available, falling back to `__builtin_assume` / `__assume` / a guarded `__builtin_unreachable()` (`qb/src/qb/utility/branch_hints.h:81-85`, `:86-100`). **`QB_ASSUME`'s expression must have no side effects** — unlike an `assert`, it is never evaluated at run time.

**`nocopy.h` — the non-copyable base.** `struct qb::nocopy` deletes both copy *and* both move members, so a class deriving from it is non-copyable **and** non-movable unless it re-enables what it needs (`qb/src/qb/utility/nocopy.h:59-77`). That second half surprises people: `qb::lockfree::spsc::ringbuffer` and `mpsc_unbounded_queue` derive from it, and so cannot be returned by value.

## Pitfalls

- **`KNOWN_L1_CACHE_LINE_SIZE` is a public ABI axis, not a tuning flag.** Setting it on the consumer and not on the library is a link error by design; setting it on both re-lays out `qb::Event`, halves the mailbox event ceiling *in buckets* (1023 → 511; in bytes it barely moves) and doubles every pipe's resting allocation. If you set it, set it for the whole build.
- **A hand-written `-I`/`-l` line will not link.** You get `qb_abi_version_unknown__compile_with_qb_s_cmake_usage_requirements`, which is the intended outcome: without qb's CMake usage requirements you are also missing the feature macros, and the resulting program's inline answers contradict the archive's (`qb/src/qb/utility/abi.h:226-232`).
- **Do not mix `NDEBUG` across translation units.** It is deliberately *not* in the fingerprint, so nothing will tell you; the residual is documented above and in [Building qb](../7_reference/building.md).
- **Do not move a `QB_ABI_ANCHOR` entity out of a header into a `.cpp`.** It silently changes a weak-external TLS descriptor into a private one, and two images then get two copies of something that must be one (`qb/src/qb/utility/abi.h:310-321`).
- **`-fvisibility=hidden` on a plugin needs the anchors.** Without them the event type-id counter is duplicated per image and two event types collide on one id — with no crash and no message (`qb/src/qb/utility/abi.h:298-306`).
- **`QB_ASSUME` is not `assert`.** Its expression is never evaluated; a side effect written inside one simply does not happen (`qb/src/qb/utility/branch_hints.h:73-76`).
- **`qb::nocopy` also deletes the move members** (`qb/src/qb/utility/nocopy.h:71-76`). If you inherit it and want moves, declare them.

## See also

- [Foundations overview](./README.md) — the rest of the layer below the event loop.
- [The pipe](./buffers.md) — `EventBucket`'s main consumer, and the cache-line assertion on `pipe<T>::swap`.
- [Containers](./containers.md) — the configure-time twin of this check, `QB_ABI_UNORDERED_MAP`.
- [Encoding and conversion](./encoding.md) — `qb::byteswap`, which lives in `compat.h`.
- [Building qb](../7_reference/building.md) · [CMake options](../7_reference/cmake_options.md) — the build knobs these axes read.
