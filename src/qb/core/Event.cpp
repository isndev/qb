/**
 * @file qb/core/Event.cpp
 * @brief Side registry mapping an assigned `qb::TypeId` back to a human-readable type name.
 *
 * `qb::Event::id_type` is a 16-bit `EventId` in every build mode (3.0.0) so that the event
 * header has one layout regardless of `NDEBUG` — cross-core events are memcpy-relocated, and a
 * consumer built with the other `NDEBUG` used to read `dest` at the wrong offset, silently.
 * Before 3.0 the Debug id *was* `typeid(T).name()`, so a mis-routing log line printed a readable
 * type name for free; this registry is what gives that name back, in **both** modes.
 *
 * Storage is a dense, direct-indexed table with one slot per `TypeId` value. That is exact by
 * construction: ids are handed out by `qb::detail::_type_id_counter` as a dense sequence and
 * `TypeId` is 16-bit, so `_type_names[id]` is in range for *every* representable id and needs no
 * bounds check, no allocation, no mutex, no growth step and no CAS — the intrusive-list shape
 * this replaced was O(number of registered types) per lookup and inlined a pointer-chasing walk
 * into `VirtualCore::__receive_events__`'s unroutable-event branch. Measured on this machine,
 * the direct-indexed lookup contributes 9 instructions to that branch; the steady-state hit path
 * of `__receive_events__` and the enqueue site `Pipe::push<T>()` are unchanged from 2.x — see
 * `dev/analysis/EVENT-ID-ABI-3.0.md`.
 *
 * The table lives here rather than in `Event.h` on purpose. A header definition is a *weak*
 * definition, and Mach-O will not place weak data in zero-fill: measured on this machine, an
 * `inline constinit std::array<std::atomic<char const *>, 65536>` lands in `__DATA,__data` and
 * costs **+512 KiB of file size in every linked binary**, while the strong definition below is
 * `__bss` — 0 file bytes, 512 KiB of virtual address space, and one resident page for a program
 * with a few hundred types. Keeping it out of line also keeps it out of qb-core's exported
 * surface, which matters for `QB_BUILD_SHARED_LIBS=ON` on Windows, where an exported *data*
 * symbol needs `__declspec(dllimport)` at every use and qb-core annotates no symbol today.
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
 * @ingroup Core
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <qb/core/Event.h>

#ifndef NDEBUG
#include <cassert>
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#define QB_TYPE_ID_SLOT_STACK_CHECK 1
#include <pthread.h>
// Under AddressSanitizer with `detect_stack_use_after_return=1` -- which qb's own `sanitize` test
// preset sets -- locals do NOT live on the real thread stack. ASan moves them to a heap-allocated
// "fake stack", so a bounds test against `pthread_get_stackaddr_np` says false for an address that
// is very much an automatic, and the check below silently stops checking. That was measured, not
// anticipated: the death test pinning this went from OK to "failed to die" under the sanitize
// preset alone. ASan exposes the mapping, so ask it rather than guess.
#if defined(__SANITIZE_ADDRESS__)
#define QB_TYPE_ID_SLOT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QB_TYPE_ID_SLOT_ASAN 1
#endif
#endif
#ifdef QB_TYPE_ID_SLOT_ASAN
extern "C" void *__asan_get_current_fake_stack(void);
extern "C" void *__asan_addr_is_in_fake_stack(void *fake_stack, void *addr, void **beg,
                                              void **end);
#endif
#endif
#endif

namespace qb::detail {
namespace {

#ifdef QB_TYPE_ID_SLOT_STACK_CHECK
/// @brief True when @p p lies inside the *calling thread's* stack.
/// @details Enforcement for the one contract `register_type_id` cannot express in its signature:
///          the slot it publishes must outlive the process, so it has to come from static
///          storage. That is documented in `Event.h` ("Storage donated by the caller's
///          block-scope static") and was still got wrong on the first try — a regression test
///          added in 3.0.0 passed an automatic and left a dangling node in the registry, which
///          `--gtest_shuffle` turned into `rc=139` on 3 of 6 seeds while **ASan stayed silent**
///          (the reader is in the un-instrumented archive, so `stack-use-after-return` never
///          armed). A signature cannot reject an automatic — there is no storage-duration trait,
///          and taking the address is the whole point of the API — so the next best thing is to
///          detect the measured shape at the moment of publication and abort loudly in Debug.
///
///          Heap and static storage are both outside the stack range, so this never fires on
///          them; a heap slot is also wrong but is not the shape that has actually occurred, and
///          no portable predicate distinguishes heap from static. Compiled out entirely under
///          `NDEBUG`, and reached only on the cold once-per-type minting path, so it costs the
///          shipped build nothing. Windows has no `pthread_*` equivalent wired up here (MSVC is
///          deferred), so the check is simply absent there rather than wrong.
bool
address_is_on_this_thread_stack(void const *const p) noexcept {
#ifdef QB_TYPE_ID_SLOT_ASAN
    // Ask ASan first: with a fake stack active this is the ONLY thing that can answer, because the
    // address is in the heap as far as pthread is concerned.
    if (void *const fake = __asan_get_current_fake_stack())
        if (__asan_addr_is_in_fake_stack(fake, const_cast<void *>(p), nullptr, nullptr) != nullptr)
            return true;
#endif
    pthread_t const self = pthread_self();
#if defined(__APPLE__)
    // Darwin hands back the stack BASE (highest address); the stack is [base - size, base).
    void *const       base = pthread_get_stackaddr_np(self);
    std::size_t const size = pthread_get_stacksize_np(self);
    if (base == nullptr || size == 0)
        return false;
    auto const hi = reinterpret_cast<std::uintptr_t>(base);
    auto const lo = hi - static_cast<std::uintptr_t>(size);
#else
    pthread_attr_t attr;
    if (pthread_getattr_np(self, &attr) != 0)
        return false;
    void       *low  = nullptr;
    std::size_t size = 0;
    // glibc hands back the LOWEST address; the stack is [low, low + size).
    int const   rc   = pthread_attr_getstack(&attr, &low, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || low == nullptr || size == 0)
        return false;
    auto const lo = reinterpret_cast<std::uintptr_t>(low);
    auto const hi = lo + static_cast<std::uintptr_t>(size);
#endif
    auto const addr = reinterpret_cast<std::uintptr_t>(p);
    return addr >= lo && addr < hi;
}
#endif /* QB_TYPE_ID_SLOT_STACK_CHECK */


/// One slot per representable `TypeId`, so every id a program can hand out has a home and
/// `_type_names[id]` can never be out of range.
///
/// `constinit` is load-bearing, not decoration. It is what makes "no dynamic initialiser" a
/// compile-time guarantee instead of an observation: types register from magic statics that can
/// run during static initialisation, on any thread, so the table must already be usable before
/// the first dynamic initialiser in the program. Being trivially destructible it also needs no
/// `__cxa_atexit`, so it stays valid through static destruction. Verified on the shipped object:
/// `(__DATA,__bss) __ZN2qb6detail12_GLOBAL__N_111_type_namesE` — zero-fill, 0 file bytes.
constexpr std::size_t kTypeNameSlots = static_cast<std::size_t>(std::numeric_limits<TypeId>::max()) + 1u;

constinit std::array<std::atomic<char const *>, kTypeNameSlots> _type_names{};

static_assert(std::is_trivially_destructible_v<decltype(_type_names)>,
              "the registry must outlive every static destructor: a type can be registered, and a "
              "name looked up on a log path, while other statics are being torn down");

static_assert(std::atomic<char const *>::is_always_lock_free,
              "the type-name registry must not take a lock: it is written from magic-static "
              "initialisers that can run during static initialisation, on any thread");

} // namespace

TypeId
register_type_id(type_id_slot &slot, char const *const name) noexcept {
    // Cold path: once per type per image, from the outlined magic-static initialiser. The spin is
    // what makes "one id per type" hold across images too -- the magic static only serialises the
    // callers inside ITS image, so without this two images could miss the walk simultaneously and
    // draw two ids for one type, which is the collision this whole registry exists to prevent.
    while (_type_id_registry_lock.test_and_set(std::memory_order_acquire))
        ;

    TypeId result = 0;
    for (type_id_slot *s = _type_id_registry.load(std::memory_order_relaxed); s != nullptr; s = s->next) {
        // Pointer equality first: within one image `typeid(T).name()` is a single link-time
        // constant, so the strcmp is only paid across images (where the addresses differ).
        if (s->name == name || std::strcmp(s->name, name) == 0) {
            result = s->id;
            break;
        }
    }

    if (result == 0) {
#ifdef QB_TYPE_ID_SLOT_STACK_CHECK
        // About to publish `&slot` permanently. A slot with automatic storage becomes a dangling
        // node the moment the caller returns, and every later registration walks it.
        assert(!address_is_on_this_thread_stack(&slot) &&
               "qb::detail::register_type_id: the slot is published into a process-wide list and "
               "must therefore have static storage duration -- this one is on the stack. Use a "
               "block-scope `static type_id_slot`, as qb::detail::type_id_for<T>() does.");
#endif
        slot.name = name;
        slot.id   = static_cast<TypeId>(_type_id_counter.fetch_add(1, std::memory_order_relaxed) + 1);
        register_type_name(slot.id, name);
        slot.next = _type_id_registry.load(std::memory_order_relaxed);
        _type_id_registry.store(&slot, std::memory_order_release);
        result = slot.id;
    }

    _type_id_registry_lock.clear(std::memory_order_release);
    return result;
}

TypeId
register_type_name(TypeId const id, char const *const name) noexcept {
    // Distinct types get distinct ids, so concurrent first instantiations write distinct slots;
    // the same type is already serialised by the magic-static init barrier. Release pairs with
    // the acquire in `type_name_for`, which is belt-and-braces: the id itself only reaches
    // another thread through the mailbox pipe, whose own release/acquire already orders this
    // store before any reader can observe the id.
    _type_names[id].store(name, std::memory_order_release);
    return id;
}

char const *
type_name_for(TypeId const id) noexcept {
    char const *const name = _type_names[id].load(std::memory_order_acquire);
    return name != nullptr ? name : "<unregistered>";
}

} // namespace qb::detail
