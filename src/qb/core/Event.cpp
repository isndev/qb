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
#include <limits>
#include <type_traits>
#include <qb/core/Event.h>

namespace qb::detail {
namespace {

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
