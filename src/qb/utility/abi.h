/**
 * @file qb/utility/abi.h
 * @brief Link-time configuration fingerprint: turns a silent header/archive mismatch
 *        into an undefined symbol.
 *
 * @details
 * ## The problem
 *
 * qb ships as **public headers + a compiled archive**. Several of qb's headers change the
 * *layout* of public types, or the *body* of an inline entity the archive also defines,
 * according to macros the **consumer** sets — while `libqb-io.a` / `libqb-core.a` were
 * compiled with whatever the *library's* build set. Nothing detects the disagreement: not
 * the compiler (each translation unit is internally consistent), not the linker (vague-linkage
 * bodies merge silently and the winner is decided by link order). The program compiles, links,
 * runs — and corrupts memory or routes events into the wrong slot.
 *
 * Measured instance, from a consumer compiled against the *installed* headers and linked
 * against the *installed* archive, with `-DKNOWN_L1_CACHE_LINE_SIZE=128` (a documented public
 * knob, and this host's real `hw.cachelinesize`):
 *
 * ```
 *                                as shipped        -DKNOWN_L1_CACHE_LINE_SIZE=128
 * QB_LOCKFREE_CACHELINE_BYTES    64                128
 * sizeof(qb::Event)              64  align 64      128  align 128
 * CoroutineFrameAllocator::kAlign 64               128
 * ```
 * Two translation units then disagree about which pool bucket a coroutine frame belongs to:
 * `AddressSanitizer: heap-buffer-overflow WRITE of size 200`, exit 134. Zero diagnostics
 * before that point.
 *
 * ## The mechanism
 *
 * The **name** of a symbol encodes the configuration. The archive *defines* one symbol per
 * axis, named after the value **it** was compiled with. Every consumer translation unit that
 * parses a qb header *references* one symbol per axis, named after the value **it** is being
 * compiled with. Equal configuration ⇒ the references resolve. Different configuration ⇒ an
 * **undefined symbol at link**, naming the axis and this translation unit's value:
 *
 * ```
 * Undefined symbols for architecture arm64:
 *   "_qb_abi_cacheline_128", referenced from:
 *       qb::detail::abi_fingerprint in main.o
 * ```
 *
 * The archive's side of the story is one command away, and needs no demangler:
 *
 * ```sh
 * nm -g <prefix>/lib/libqb-io.a | grep qb_abi        # -> _qb_abi_cacheline_64
 * strings <prefix>/lib/libqb-io.a | grep '^qb-abi '  # -> qb-abi qb=3.0.0 cacheline=64 ...
 * ```
 *
 * On MSVC and clang-cl the same axes are additionally emitted as `#pragma detect_mismatch`
 * records, which the Microsoft linker reports as `LNK2038` naming **both** values. That pragma
 * is a documented no-op on Mach-O and ELF (verified: it compiles, emits no linker-option
 * section, and does not fail a mismatched link), so it is a bonus on Windows, never the
 * mechanism.
 *
 * **There is no opt-out macro, deliberately.** Every axis below is a configuration in which
 * the two sides are provably unsound together; the fix is to rebuild qb with the same setting,
 * not to silence the check.
 *
 * ## What is in the fingerprint, and why
 *
 * An axis qualifies only if a difference between archive and consumer is (a) *possible* in a
 * build that otherwise compiles and links, (b) *silent* — no diagnostic from any tool, and
 * (c) *unsound* — it changes the layout of a public type, or the body of an entity the archive
 * also defines.
 *
 * | axis | symbol | why it qualifies (measured) |
 * |---|---|---|
 * | qb version | `qb_abi_version_M_m_p` | installed headers and archive are one unit; nothing else detects skew, and it is the axis that catches a consumer compiled without qb's CMake usage requirements at all |
 * | cache line | `qb_abi_cacheline_N` | `sizeof(qb::Event)` 64 -> 128, `CoroutineFrameAllocator::kAlign` 64 -> 128 |
 * | exceptions | `qb_abi_exceptions_[01]` | `-fno-exceptions` forks 8 inline bodies, of which `router::memh<Event,true,void>::subscribe` and `nanolog::NanoLogLine::operator<< <uint16_t>` are *also* defined by the archive |
 * | coroutine debug | `qb_abi_coroutine_debug_[01]` | `QB_DEBUG_COROUTINES` grows `task<T>::promise_type` 32 -> 40 |
 * | jthread source | `qb_abi_std_jthread_[01]` | `QB_COMPAT_FORCE_THREAD_FALLBACK` (or a standard library without `__cpp_lib_jthread`) swaps `qb::jthread` 16 -> 24 and moves every member after it: `qb::Main` 88 -> 96, `qb::VirtualCore` 8648 -> 8656 |
 *
 * Deliberately **excluded**, each for a reason that must be re-argued before it changes:
 *
 * - **`NDEBUG`** — measured to change **no** layout, and kept that way by
 *   `qb/scripts/check-abi-macro-split.py`. A Debug consumer against a Release archive is a
 *   *supported, CI-tested* configuration (`.github/workflows/install-consume.yml` builds
 *   Release, Debug **and** an unset `CMAKE_BUILD_TYPE` against one Release install), and an
 *   unset `CMAKE_BUILD_TYPE` is CMake's **default**. Putting `NDEBUG` in the fingerprint would
 *   turn the default consumer configuration into a hard link failure.
 *
 *   The residual is **open, not fixed** — say so here rather than describing a remedy as though it
 *   had been applied. 39 `assert(` sites and 7 `#if*NDEBUG` blocks still sit inside `inline` and
 *   template bodies in shipped headers, so two translation units that disagree about `NDEBUG` emit
 *   two bodies under one vague-linkage symbol and **object order alone** decides which survives.
 *   Measured with the same two objects on macOS/ld-prime and Linux/GNU ld 2.44:
 *   `main.o tu_dbg.o` -> `exit=0` (no assert), `tu_dbg.o main.o` -> `exit=134`
 *   (`async_mutex::unlock`, `sync.h:535`).
 *
 *   It stays open because every candidate fix costs more than it saves: compiling the asserts
 *   unconditionally puts a branch on `schedule_via_current` and `generator<T>::iterator::operator*`
 *   and redefines what `-DNDEBUG` means for users; an `inline namespace` ABI tag keyed on `NDEBUG`
 *   re-mangles every symbol per build mode and breaks the very configuration this exclusion exists
 *   to protect. The one remedy with no release cost — keying the header asserts off the *archive's*
 *   build mode through a generated installed header — is a design change, not a patch.
 *   The constraint a user can act on is documented where a user will see it:
 *   `qb/readme/7_reference/building.md`, "Do not mix `NDEBUG` across translation units".
 * - **`__cpp_rtti`** — `-fno-rtti` cannot compile qb at all (`Event.h` uses `typeid`). Already
 *   loud, at the first translation unit.
 * - **`-std=c++20` vs `-std=c++23`** — measured: identical layout for every public type,
 *   including `qb::expected`. qb exports `cxx_std_20` as a *minimum* and builds both.
 * - **`QB_HAS_SSL` / `QB_HAS_QUIC` / `QB_HAS_COMPRESSION` / `QB_WITH_LOGGING`** — measured: they
 *   gate whole types (`qb::crypto::base64`, `qb::io::use<>::ssl`), never a member of a type that
 *   exists in both configurations, so a mismatch is a *compile* error, not silent corruption.
 *   The one genuinely silent case (a hand-written `-I`/`-l` consumer seeing
 *   `quic::available() == false` while the archive reports `native_backend_ready() == true`) is
 *   caught here through the version axis, and its own fix is the `#error` treatment
 *   `qb/io/compression.h` and `qb/io/crypto_jwt.h` already use.
 * - **`QB_ENABLE_UDS`** — `qb/io/config.h` defines it unconditionally and every test on it is
 *   `defined(QB_ENABLE_UDS)`, so it cannot differ between two translation units. Verified:
 *   `-DQB_ENABLE_UDS=0` leaves `sizeof(qb::io::endpoint)` at 108.
 * - **Standard-library hardening / debug modes** (`_GLIBCXX_DEBUG`, `_LIBCPP_HARDENING_MODE`, …)
 *   — real ABI axes, but not qb's to arbitrate; the standard libraries carry their own ABI tags.
 * - **Architecture, pointer size, target triple** — the linker already refuses those.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Utility
 */

#ifndef QB_UTILS_ABI_H
#define QB_UTILS_ABI_H

// <version> and nothing else: this header is included from qb/utility/build_macros.h and
// qb/utility/prefix.h, i.e. from the very bottom of the include graph, and must stay free of
// any qb dependency (it is the *source* of two of the values it fingerprints, not a consumer
// of them). <version> is macros only and is what advertises __cpp_lib_jthread.
#include <version>

// ---------------------------------------------------------------------------------------------
// Axis values. Each is derived here, once, from the raw knob -- and qb's own switch is then
// defined FROM the value below (qb/utility/prefix.h for the cache line, qb/utility/compat.h for
// jthread). The fingerprint therefore cannot drift from the thing it fingerprints: there is only
// one derivation.
// ---------------------------------------------------------------------------------------------

/**
 * @def QB_ABI_CACHELINE_BYTES
 * @brief Cache-line size qb lays its types out on. Source of `QB_LOCKFREE_CACHELINE_BYTES`.
 * @details Defaults to 64, the standard line size on every platform qb supports. Overridable by
 *          the documented public knob `KNOWN_L1_CACHE_LINE_SIZE` -- which is exactly why it is
 *          fingerprinted: it re-lays-out `qb::Event`, `EventBucket` and the coroutine frame pool,
 *          and a consumer that sets it against an archive that did not gets a heap overflow.
 */
#ifdef KNOWN_L1_CACHE_LINE_SIZE
#define QB_ABI_CACHELINE_BYTES KNOWN_L1_CACHE_LINE_SIZE
#else
#define QB_ABI_CACHELINE_BYTES 64
#endif

/**
 * @def QB_ABI_EXCEPTIONS
 * @brief 1 when the translation unit is compiled with C++ exceptions, 0 under `-fno-exceptions`.
 * @details Read from `__cpp_exceptions`, the same macro `qb/utility/build_macros.h` branches on
 *          for `QB__NO_EXCEPTIONS` / `QB__THROW`, so the two cannot disagree.
 */
#if defined(__cpp_exceptions)
#define QB_ABI_EXCEPTIONS 1
#else
#define QB_ABI_EXCEPTIONS 0
#endif

/**
 * @def QB_ABI_CORO_DEBUG
 * @brief 1 when `QB_DEBUG_COROUTINES` is set, which adds a trace id to `task<T>::promise_type`.
 */
#ifdef QB_DEBUG_COROUTINES
#define QB_ABI_CORO_DEBUG 1
#else
#define QB_ABI_CORO_DEBUG 0
#endif

/**
 * @def QB_ABI_STD_JTHREAD
 * @brief 1 when `qb::jthread` / `qb::stop_token` alias the standard ones, 0 for qb's fallback.
 * @details Source of `QB_COMPAT_HAS_STD_JTHREAD` (`qb/utility/compat.h`). Both routes to a 0 are
 *          fingerprinted by construction: the `QB_COMPAT_FORCE_THREAD_FALLBACK` knob, and a
 *          standard library that does not advertise `__cpp_lib_jthread`.
 */
#if !defined(QB_COMPAT_FORCE_THREAD_FALLBACK) && defined(__cpp_lib_jthread) && \
    __cpp_lib_jthread >= 201911L
#define QB_ABI_STD_JTHREAD 1
#else
#define QB_ABI_STD_JTHREAD 0
#endif

// ---------------------------------------------------------------------------------------------
// Token pasting. The two-level indirection is the standard one: the outer macro expands its
// arguments, the inner one pastes them.
// ---------------------------------------------------------------------------------------------
#define QB_ABI_PASTE_(a, b) a##b
#define QB_ABI_PASTE(a, b) QB_ABI_PASTE_(a, b)
#define QB_ABI_VERSION_SYM_(a, b, c) qb_abi_version_##a##_##b##_##c
#define QB_ABI_VERSION_SYM(a, b, c) QB_ABI_VERSION_SYM_(a, b, c)
#define QB_ABI_STR_(x) #x
#define QB_ABI_STR(x) QB_ABI_STR_(x)

#define QB_ABI_SYM_CACHELINE QB_ABI_PASTE(qb_abi_cacheline_, QB_ABI_CACHELINE_BYTES)
#define QB_ABI_SYM_EXCEPTIONS QB_ABI_PASTE(qb_abi_exceptions_, QB_ABI_EXCEPTIONS)
#define QB_ABI_SYM_CORO_DEBUG QB_ABI_PASTE(qb_abi_coroutine_debug_, QB_ABI_CORO_DEBUG)
#define QB_ABI_SYM_STD_JTHREAD QB_ABI_PASTE(qb_abi_std_jthread_, QB_ABI_STD_JTHREAD)

// The version comes from qb's CMake usage requirements (QB_VERSION_* -- see
// qb/cmake/qbConfig.cmake, sourced from QB_FRAMEWORK_VERSION), never from a literal written here:
// every hand-written copy of the version in this tree went stale. A translation unit compiled
// WITHOUT those usage requirements -- a hand-written `-I`/`-l` line -- is not merely missing the
// version, it is also missing QB_HAS_SSL / QB_HAS_QUIC / QB_HAS_COMPRESSION, and gets a program
// whose inline feature answers contradict the archive's out-of-line ones. So that case gets a
// symbol whose name says what to do about it rather than a misleading `0_0_0`.
#if defined(QB_VERSION_MAJOR) && defined(QB_VERSION_MINOR) && defined(QB_VERSION_PATCH)
#define QB_ABI_SYM_VERSION \
    QB_ABI_VERSION_SYM(QB_VERSION_MAJOR, QB_VERSION_MINOR, QB_VERSION_PATCH)
#define QB_ABI_VERSION_TEXT                                                    \
    QB_ABI_STR(QB_VERSION_MAJOR) "." QB_ABI_STR(QB_VERSION_MINOR) "." QB_ABI_STR( \
        QB_VERSION_PATCH)
#else
#define QB_ABI_SYM_VERSION qb_abi_version_unknown__compile_with_qb_s_cmake_usage_requirements
#define QB_ABI_VERSION_TEXT "unknown"
#endif

/**
 * @def QB_ABI_FINGERPRINT_TEXT
 * @brief Human-readable spelling of this translation unit's fingerprint.
 * @details The archive stores its own copy under the `qb_abi_fingerprint` symbol, so
 *          `strings <archive> | grep '^qb-abi '` answers "what was this built with?" with no
 *          demangler and no qb source tree. That question had no answer before 3.0.
 */
#define QB_ABI_FINGERPRINT_TEXT                                                       \
    "qb-abi qb=" QB_ABI_VERSION_TEXT " cacheline=" QB_ABI_STR(QB_ABI_CACHELINE_BYTES) \
    " exceptions=" QB_ABI_STR(QB_ABI_EXCEPTIONS) " coroutine_debug=" QB_ABI_STR(      \
        QB_ABI_CORO_DEBUG) " std_jthread=" QB_ABI_STR(QB_ABI_STD_JTHREAD)

#ifdef __cplusplus

// `used` is what makes the check per-translation-unit rather than per-optimiser-whim: without it
// a constant-initialised, never-read pointer array is discarded at -O1 and above, and a lone
// oddly-configured TU would slip through. With it, EVERY TU that parses a qb header carries the
// references. Cost: one 5-word COMDAT array the linker folds to a single copy, and no static
// initialiser (the addresses are constant expressions -- qb/core/Event.cpp deliberately carries
// no __mod_init_func and this must not change that).
//
// `retain` is NOT optional on ELF, and this was measured, not assumed. `used` only stops the
// *compiler* from dropping the array; GNU ld's `--gc-sections` discards the unreferenced
// `.data.rel.ro._ZN2qb6detail15abi_fingerprintE` section before it resolves the relocations
// inside it, so every mismatched configuration linked cleanly under
// `-ffunction-sections -fdata-sections -Wl,--gc-sections` -- with clang-19 AND g++-14, ld 2.44.
// That is a common consumer setting, so without `retain` (SHF_GNU_RETAIN, binutils >= 2.36) the
// check would be silently defeated for exactly the builds most likely to use it. On Mach-O
// `used` already implies no_dead_strip and `-Wl,-dead_strip` does not defeat it.
#ifdef __has_attribute
#if __has_attribute(used)
#define QB_ABI_USED __attribute__((used))
#endif
#if __has_attribute(retain)
#define QB_ABI_RETAIN __attribute__((retain))
#endif
#endif
#ifndef QB_ABI_USED
#define QB_ABI_USED
#endif
#ifndef QB_ABI_RETAIN
#define QB_ABI_RETAIN
#endif

/**
 * @def QB_ABI_ANCHOR
 * @brief Marks an entity that must exist **exactly once per process**, whatever the consumer's
 *        visibility setting or the number of images qb is linked into.
 *
 * @details
 * ## What it is for
 *
 * A handful of qb entities are *identity*: the event type-id counter, the per-type magic static
 * that draws from it, the router's disposer table, the `ServiceActor` registry, the per-thread
 * `listener` and `VirtualCore`, the coroutine frame pool, the `no_protocol()` null-object
 * sentinel (compared by **address**). Every one of them is a vague-linkage entity — an `inline`
 * variable, a static data member of a class template, or a function-local `static`. Vague
 * linkage means "N definitions, the linker keeps one", and *which* linker does the keeping
 * matters: the static linker folds them within one image, and the dynamic linker coalesces the
 * remaining weak definitions **across** images. That second step is what `-fvisibility=hidden`
 * turns off.
 *
 * ## Measured, in the shape that fails
 *
 * Host executable + `dlopen`ed plugin, both statically linking qb, plugin compiled
 * `-fvisibility=hidden` (RTLD_LOCAL and RTLD_GLOBAL alike):
 *
 * ```
 *  plugin default visibility (control)   plugin -fvisibility=hidden
 *  &_type_id_counter 0x100e18000 (both)  &_type_id_counter 0x1021b0000 / 0x10744c158
 *  host: KillEvent=1  SignalEvent=2      host:   KillEvent=1  SignalEvent=2
 *  plug: KillEvent=1  Noop=7             plug:   KillEvent=1  Noop=2   <-- COLLISION
 * ```
 *
 * Two distinct event types hold id 2, so `router::memh` routes them to the same slot. Both runs
 * exit 0 and print nothing: this is the failure `qb/core/Event.h` already warns "silently breaks
 * event routing", made unreachable here rather than documented harder.
 *
 * `visibility("default")` on the anchor restores the coalescing — measured on the same harness,
 * with the same plugin still compiled `-fvisibility=hidden`.
 *
 * ## The second job: one instance across images
 *
 * The attribute also decides whether an anchor can be shared at all. A `thread_local` **defined
 * out of line in a .cpp** (as `listener::current` was) emits a `non-external` TLS descriptor —
 * private to its image by construction, so two images that each link `libqb-io.a` get two
 * per-thread event loops on the *same* thread and neither dyld nor the linker says a word. The
 * same entity defined `inline` in the header emits a **weak-external** descriptor that dyld
 * coalesces. That is why these anchors live in headers, not in `.cpp` files, and why moving one
 * back would silently reintroduce the split.
 *
 * @note Not a substitute for an export macro. qb annotates no symbol for `__declspec(dllimport)`
 *       today, so `QB_ABI_ANCHOR` is empty on MSVC; a Windows shared build needs the export-macro
 *       work first. On ELF and Mach-O it is exactly the annotation `-fvisibility=hidden` requires.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#define QB_ABI_ANCHOR
#elif defined(__has_attribute)
#if __has_attribute(visibility)
#define QB_ABI_ANCHOR __attribute__((visibility("default")))
#endif
#endif
#ifndef QB_ABI_ANCHOR
#define QB_ABI_ANCHOR
#endif

// Annotated for the same reason as the identity anchors above: a shared qb built
// `-fvisibility=hidden` would otherwise stop exporting these six and every consumer would fail to
// link. Loud rather than silent, but total. A static archive was never at risk -- hidden
// visibility governs dynamic export, not static-link resolution.
extern "C" {
/** @brief Defined by libqb-io, named after the qb version the archive was built from. */
QB_ABI_ANCHOR extern const char QB_ABI_SYM_VERSION;
/** @brief Defined by libqb-io, named after the archive's `QB_LOCKFREE_CACHELINE_BYTES`. */
QB_ABI_ANCHOR extern const char QB_ABI_SYM_CACHELINE;
/** @brief Defined by libqb-io, named after the archive's `__cpp_exceptions` state. */
QB_ABI_ANCHOR extern const char QB_ABI_SYM_EXCEPTIONS;
/** @brief Defined by libqb-io, named after the archive's `QB_DEBUG_COROUTINES` state. */
QB_ABI_ANCHOR extern const char QB_ABI_SYM_CORO_DEBUG;
/** @brief Defined by libqb-io, named after the archive's `QB_ABI_STD_JTHREAD`. */
QB_ABI_ANCHOR extern const char QB_ABI_SYM_STD_JTHREAD;
/** @brief Defined by libqb-io: `QB_ABI_FINGERPRINT_TEXT` as the archive spells it. */
QB_ABI_ANCHOR extern const char qb_abi_fingerprint[];
}

namespace qb::detail {
/**
 * @brief The reference that makes a configuration mismatch a link error.
 * @details Named plainly on purpose: the linker prints it on the `referenced from:` line, and
 *          `abi_fingerprint` is the one grep that leads to this file and its explanation.
 */
QB_ABI_USED QB_ABI_RETAIN inline const void *const abi_fingerprint[] = {
    &QB_ABI_SYM_VERSION, &QB_ABI_SYM_CACHELINE, &QB_ABI_SYM_EXCEPTIONS, &QB_ABI_SYM_CORO_DEBUG,
    &QB_ABI_SYM_STD_JTHREAD};
} // namespace qb::detail

#endif /* __cplusplus */

// Windows only, and strictly additional: the Microsoft linker reports a detect_mismatch conflict
// as `LNK2038: mismatch detected for 'qb_cacheline': value '64' doesn't match value '128' in
// main.obj`, i.e. it names BOTH sides, which no ELF/Mach-O linker will do for an undefined
// symbol. Verified to be an inert no-op under Apple clang on Mach-O (compiles, emits no
// linker-option load command, does not fail a mismatched link), hence the guard: it is a bonus
// where it works, never the mechanism. UNVERIFIED on MSVC -- no Windows toolchain is reachable
// from this project's development hosts; see dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0.md section 7.
//
// It goes through `_Pragma`, not `#pragma`, and that is load-bearing rather than stylistic: a
// `#pragma` line's arguments are NOT macro-expanded. Measured here -- `clang -E` on
// `#pragma detect_mismatch("qb_cacheline", QB_ABI_STR(QB_ABI_CACHELINE_BYTES))` emits that text
// verbatim, macro and all, so both sides of a comparison would read `QB_ABI_STR(...)` and could
// never differ. `_Pragma(#x)` behind an expanding wrapper is the standard way to get the value in,
// and it is the same DO_PRAGMA idiom qb/utility/build_macros.h already uses for warning control.
#if defined(_MSC_VER)
#define QB_ABI_DO_PRAGMA(x) _Pragma(#x)
#define QB_ABI_DETECT_MISMATCH(name, value) QB_ABI_DO_PRAGMA(detect_mismatch(name, value))
QB_ABI_DETECT_MISMATCH("qb_version", QB_ABI_STR(QB_ABI_SYM_VERSION))
QB_ABI_DETECT_MISMATCH("qb_cacheline", QB_ABI_STR(QB_ABI_CACHELINE_BYTES))
QB_ABI_DETECT_MISMATCH("qb_exceptions", QB_ABI_STR(QB_ABI_EXCEPTIONS))
QB_ABI_DETECT_MISMATCH("qb_coroutine_debug", QB_ABI_STR(QB_ABI_CORO_DEBUG))
QB_ABI_DETECT_MISMATCH("qb_std_jthread", QB_ABI_STR(QB_ABI_STD_JTHREAD))
#endif

#endif /* QB_UTILS_ABI_H */
