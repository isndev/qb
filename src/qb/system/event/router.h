/**
 * @file qb/system/event/router.h
 * @brief Event routing system
 *
 * This file defines a flexible and type-safe event routing system that enables
 * message passing between components. It provides several router implementations
 * for different communication patterns:
 * - Single-Event Single-Handler (SESH) for direct point-to-point communication
 * - Single-Event Multiple-Handler (SEMH) for one-to-many distribution
 * - Multiple-Event Single-Handler (MESH) for many-to-one handling
 * - Multiple-Event Multiple-Handler (MEMH) for fully dynamic event routing
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
 * @ingroup Event
 */

#ifndef QB_EVENT_ROUTER_H
#define QB_EVENT_ROUTER_H
#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>
#include <qb/system/allocator/region.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/abi.h> /* QB_ABI_ANCHOR */
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>

namespace qb::router {
//        struct EventExample {
//            using id_type = uint16_t;
//            using id_handler_type = uint32_t;
//
//            template<typename T>
//            constexpr id_type type_to_id();
//
//            id_type id;
//            id_handler_type dest;
//            id_handler_type source;
//        };

/**
 * @brief Customisation point: how a router key maps onto a small dense integer.
 *
 * @details The two lookups on the per-event dispatch path — event id → resolver in `memh`,
 * handler id → handler in `semh` — used to be two `qb::unordered_map` finds: hash, bucket walk,
 * node dereference, then a virtual `resolve()` between them. Measured on savina/counting at
 * one core (the purest same-core dispatch there is: push + pipe + `consume_all` + route), a
 * qb event cost ~27 ns on MSVC 19.51 and ~40 ns on g++ 14 against a 3 ns raw-thread floor;
 * `perf` put the two lookups at the top of the profile once the accessors had been inlined.
 *
 * Both keys are ALREADY dense integers: `EventId` is assigned from a counter
 * (`Event::type_to_id`), and a `VirtualCore`'s handlers are its own actors, whose slots come
 * from a pool that hands freed slots back first. A router keyed on such an id can be a
 * vector indexed by it — one bounds check and one load where the map cost five dependent
 * ones — without giving up the generic routers: a key with no dense form (the router unit
 * test keys events on `const char *`) keeps the hash map, selected at compile time.
 *
 * Specialise for a key type to enable the dense table. **Contract:** `of(k)` is at most
 * `max_index`, and two keys that can be subscribed on ONE router at the same time yield
 * different indices within their bank — the table stores the full key beside the slot and
 * compares it on every hit, so a key that shares an index with a live subscriber is a MISS
 * (never a misdelivery), and `subscribe()` asserts the collision in a debug build. Unsigned
 * integers of at most 16 bits are dense by identity. `qb::ActorId` projects ordinary slots and
 * service tags into separate banks (ActorId.h); a router only ever holds one core's actors. A specialisation
 * may declare `partitioned = true`, `secondary(key)` and `secondary_max_index` to keep a second
 * dense bank. Then `of(key)` is bounded by the selected bank's maximum, and identical indices
 * in different banks are independent. Neither bank allocates on lookup or erase.
 * A partitioned specialisation may also declare `primary_excludes_secondary = true` when
 * `primary_of(key)` is defined for every key and exceeds `max_index` for every secondary key.
 * Then an index inside the primary bank's bounds proves that no secondary lookup is needed.
 *
 * @tparam Key The router key type (`_RawEvent::id_type` or `_RawEvent::id_handler_type`)
 */
template <typename Key>
struct dense_index {
    static constexpr bool enabled = false;
};

template <typename Key>
requires(std::unsigned_integral<Key> && !std::same_as<Key, bool> && sizeof(Key) <= 2)
struct dense_index<Key> {
    static constexpr bool        enabled   = true;
    static constexpr std::size_t max_index = std::numeric_limits<Key>::max();

    [[nodiscard]] static constexpr std::size_t
    of(Key const key) noexcept {
        return static_cast<std::size_t>(key);
    }
};

namespace internal {

/**
 * @brief A broadcast's handler snapshot, one per thread and router type: a stack of `[base, end)`
 *        frames -- a handler may broadcast again -- held as a region of slots and a logical end.
 * @details An append is a store and an increment. Appended through `push_back` on a `region_stack`
 *          (a `std::vector` on a non-default allocator), each element paid libc++'s construct
 *          transaction, its end pointer spilled to the stack and reloaded (Apple clang 21: the
 *          eight-sink broadcast fan-out +22 % against v3.2.1, which appended to a `std::vector` with
 *          the default allocator). The slots stay a region past 64 KiB (Huly QB-1012), grown by
 *          doubling out of line; a frame is popped by setting `end` back to its base.
 */
template <typename T>
struct snapshot_stack {
    qb::allocator::region_vector<T> slots;
    std::size_t                     end = 0;

    void
    push(T const &value) {
        if (end == slots.size()) [[unlikely]]
            grow();
        slots[end++] = value;
    }

    QB_NOINLINE void
    grow() {
        std::size_t const capacity = slots.size();
        qb::allocator::detail::region_resize(slots, capacity != 0 ? capacity * 2 : 16,
                                             capacity * 4 > 65536 ? capacity * 4 : std::size_t{65536});
    }
};

// The generic table keeps occupancy independent of Value: even Value{} or nullptr
// can be a present entry. Alternative storage must be selected explicitly.
struct explicit_key_occupancy {
    template <typename Key, typename Value>
    struct Slot {
        Key   key{};
        Value value{};
        bool  used = false;

        bool
        occupied() const noexcept {
            return used;
        }
        void
        mark_used() noexcept {
            used = true;
        }
        void
        mark_unused() noexcept {
            used = false;
        }
    };
};

/**
 * @brief The router's key → value table: a vector indexed by `dense_index<Key>::of()` when
 *        the key has a dense form, a `qb::unordered_map` otherwise. Same four operations
 *        either way, so the routers below are written once.
 *
 * @details The dense table grows geometrically on `insert_or_assign` (a core spawning actors
 * one at a time subscribes one new id per spawn; an exact `resize` there would be quadratic)
 * and never shrinks — `erase` clears the slot, and the pool hands a freed slot back before it
 * opens a new one, so the table stays as compact as the core's peak population.
 *
 * @tparam Key   The key type
 * @tparam Value The stored value; `Value{}` is what an erased slot holds
 */
template <typename Key, typename Value, typename Occupancy = explicit_key_occupancy>
class key_table {
    static constexpr bool dense       = dense_index<Key>::enabled;
    static constexpr bool partitioned = [] {
        if constexpr (dense && requires { dense_index<Key>::partitioned; })
            return dense_index<Key>::partitioned;
        else
            return false;
    }();

public:
    /// Secondary projections cannot fall within an allocated primary bank.
    static constexpr bool primary_excludes_secondary = [] {
        if constexpr (partitioned && requires { dense_index<Key>::primary_excludes_secondary; })
            return dense_index<Key>::primary_excludes_secondary;
        else
            return false;
    }();

    /// A partitioned key whose broadcast key lies in the secondary bank (`dense_index<Key>::broadcast_secondary`):
    /// a router can then send a unicast to the primary bank on ONE test, `is_secondary(key)` false, and leave the
    /// broadcast walk and the secondary lookup to its out-of-line half (semh<E, void>::dispatch_event).
    static constexpr bool broadcast_secondary = [] {
        if constexpr (partitioned && requires { dense_index<Key>::broadcast_secondary; })
            return dense_index<Key>::broadcast_secondary;
        else
            return false;
    }();

private:
    using Slot = typename Occupancy::template Slot<Key, Value>;

    using Dense  = qb::allocator::region_vector<Slot>;
    using Sparse = qb::unordered_map<Key, Value>;

    struct PartitionedDense {
        Dense       primary;
        Dense       secondary;
        std::size_t secondary_size = 0;
    };

    // A partitioned key maps its secondary namespace to compact indices instead of growing
    // through a high encoded slot. Other keys retain exactly the original table storage.
    std::conditional_t<partitioned, PartitionedDense, std::conditional_t<dense, Dense, Sparse>> _table;
    // The dense table's slot count, kept beside it: `find()` bounds-checks every lookup, and
    // `_table.size()` is a pointer difference divided by `sizeof(Slot)` -- a multiply by a magic
    // constant on every event routed when the slot is 24 bytes wide (QB-182). One load instead.
    std::size_t _dense_size = 0;

    [[nodiscard]] Dense &
    primary_table() noexcept {
        if constexpr (partitioned)
            return _table.primary;
        else
            return _table;
    }

    [[nodiscard]] Dense const &
    primary_table() const noexcept {
        return const_cast<key_table *>(this)->primary_table();
    }

public:
    key_table()  = default;
    ~key_table() = default;

    /**
     * @brief Look a key up
     * @return A pointer to the stored value, or `nullptr` if the key is not present
     */
    [[nodiscard]] Value *
    find(Key const &key) noexcept {
        if constexpr (dense) {
            if constexpr (partitioned) {
                // The secondary bank answers out of line. Inline beside the primary one, it made this lookup too
                // big for MSVC 19.51 to inline into the resolvers: one call and 22 instructions per routed event
                // where the unpartitioned table was 12 inline (single-stepped, Huly QB-970 perf lot).
                if (dense_index<Key>::secondary(key)) [[unlikely]]
                    return find_secondary(key);
                return find_primary(key);
            }
            const auto idx = dense_index<Key>::of(key);
            if (likely(idx < _dense_size)) {
                auto &slot = primary_table()[idx];
                if (likely(slot.occupied() && slot.key == key))
                    return &slot.value;
            }
            return nullptr;
        } else {
            const auto it = _table.find(key);
            return likely(it != _table.end()) ? &it->second : nullptr;
        }
    }

    [[nodiscard]] Value const *
    find(Key const &key) const noexcept {
        return const_cast<key_table *>(this)->find(key);
    }

    /// A partitioned key's bank (dense_index<Key>::secondary)
    [[nodiscard]] static bool
    is_secondary(Key const &key) noexcept {
        return dense_index<Key>::secondary(key);
    }

    /// Probe a primary projection: bounds, occupancy and complete key are always checked.
    /// Callers either classified the key or proved primary_excludes_secondary for a speculative probe.
    [[nodiscard]] QB_MSVC_FORCEINLINE Value *
    find_primary(Key const &key) noexcept {
        const std::size_t idx = dense_index<Key>::primary_of(key);
        if (likely(idx < _dense_size)) {
            auto &slot = _table.primary[idx];
            if (likely(slot.occupied() && slot.key == key))
                return &slot.value;
        }
        return nullptr;
    }
    [[nodiscard]] QB_MSVC_FORCEINLINE Value const *
    find_primary(Key const &key) const noexcept {
        return const_cast<key_table *>(this)->find_primary(key);
    }

    /// find()'s secondary bank (a partitioned key only: instantiated from that branch alone); out of line
    [[nodiscard]] QB_NOINLINE Value *
    find_secondary(Key const &key) noexcept {
        const std::size_t idx = dense_index<Key>::secondary_of(key);
        if (idx < _table.secondary_size) {
            auto &slot = _table.secondary[idx];
            if (slot.occupied() && slot.key == key)
                return &slot.value;
        }
        return nullptr;
    }
    [[nodiscard]] Value const *
    find_secondary(Key const &key) const noexcept {
        return const_cast<key_table *>(this)->find_secondary(key);
    }

    /// erase()'s secondary bank, out of line
    QB_NOINLINE void
    erase_secondary(Key const &key) noexcept {
        const std::size_t idx = dense_index<Key>::secondary_of(key);
        if (idx < _table.secondary_size) {
            auto &slot = _table.secondary[idx];
            if (slot.occupied() && slot.key == key) {
                slot.value = Value{};
                slot.mark_unused();
            }
        }
    }

    /**
     * @brief Store a value under a key, replacing any value already there
     */
    void
    insert_or_assign(Key const &key, Value value) {
        if constexpr (dense) {
            const auto idx = dense_index<Key>::of(key);
            if constexpr (partitioned) {
                if (dense_index<Key>::secondary(key)) {
                    assert(idx <= dense_index<Key>::secondary_max_index && "dense_index<Key>::of() exceeds secondary_max_index");
                    auto &table = _table.secondary;
                    if (idx >= table.size()) {
                        if (idx >= table.capacity())
                            qb::allocator::detail::region_reserve(table, idx + 1, dense_index<Key>::secondary_max_index + 1);
                        table.resize(idx + 1);
                        _table.secondary_size = table.size();
                    }
                    auto &slot = table[idx];
                    assert((!slot.occupied() || slot.key == key) && "dense_index<Key>: two live keys share one secondary index");
                    slot.key   = key;
                    slot.value = std::move(value);
                    slot.mark_used();
                    return;
                }
            }
            assert(idx <= dense_index<Key>::max_index && "dense_index<Key>::of() exceeds max_index");
            auto &table = primary_table();
            if (idx >= table.size()) {
                // Grow by CAPACITY, never by size: `reserve(size() * 2)` on every insert asked
                // for one more element than the previous doubling had left, so each new id
                // reallocated and copied the whole table -- O(n) per subscribe, O(n^2) per
                // core, times one table per event type. savina/fib (57 313 actors born and
                // dead in one window, seven subscriptions each) measured 43 s where CAF
                // measures 86 ms; with this branch it is the pool's O(1) again.
                if (idx >= table.capacity())
                    qb::allocator::detail::region_reserve(table, idx + 1, dense_index<Key>::max_index + 1);
                table.resize(idx + 1);
                _dense_size = table.size();
            }
            auto &slot = table[idx];
            assert((!slot.occupied() || slot.key == key) && "dense_index<Key>: two live keys share one index");
            slot.key   = key;
            slot.value = std::move(value);
            slot.mark_used();
        } else {
            _table.insert_or_assign(key, std::move(value));
        }
    }

    /**
     * @brief Remove a key; a no-op if it is not present
     */
    void
    erase(Key const &key) noexcept {
        if constexpr (primary_excludes_secondary) {
            // The bound proves the bank for this projection. Keep the secondary classification
            // after the primary bound, so a primary hit needs only the complete-key check below.
            // An ordinary miss still classifies before returning; a service miss reaches its bank.
            const std::size_t idx = dense_index<Key>::primary_of(key);
            if (idx < _dense_size) {
                auto &slot = _table.primary[idx];
                if (slot.occupied() && slot.key == key) {
                    slot.value = Value{};
                    slot.mark_unused();
                }
            } else if (dense_index<Key>::secondary(key)) [[unlikely]] {
                erase_secondary(key);
            }
            return;
        }
        if constexpr (partitioned) {
            // A leaf for the primary bank, the secondary one out of line: with both inline MSVC 19.51 saved six
            // registers on every call, and every actor removal calls this once per event type the core routes
            // (a miss, 29 instructions, where the unpartitioned table took 4 -- Huly QB-970 perf lot).
            if (dense_index<Key>::secondary(key)) [[unlikely]] {
                erase_secondary(key);
                return;
            }
            const std::size_t idx = dense_index<Key>::primary_of(key);
            if (idx < _dense_size) {
                auto &slot = _table.primary[idx];
                if (slot.occupied() && slot.key == key) {
                    slot.value = Value{};
                    slot.mark_unused();
                }
            }
            return;
        }
        if constexpr (dense) {
            if (auto *const value = find(key)) {
                if constexpr (partitioned) {
                    if (dense_index<Key>::secondary(key)) {
                        *value = Value{};
                        _table.secondary[dense_index<Key>::of(key)].mark_unused();
                        return;
                    }
                }
                auto &slot = primary_table()[dense_index<Key>::of(key)];
                *value     = Value{};
                slot.mark_unused();
            }
        } else {
            _table.erase(key);
        }
    }

    /**
     * @brief Visit every present entry as `f(key, value)`, in index order within each dense bank
     * @warning `f` must not insert into or erase from THIS table (see the broadcast snapshot in
     *          `semh::route` for why, and for the idiom).
     */
    template <typename Func>
    void
    for_each(Func &&f) const {
        if constexpr (dense) {
            if constexpr (partitioned) {
                // Preserve the former service-before-ordinary traversal on a core. One loop over the two
                // banks, kept rolled, not one loop per bank: two copies of the body let the AArch64 machine
                // outliner fold them into shared calls, two calls per occupied slot of every broadcast walk
                // (Apple clang 21: the eight-sink broadcast fan-out +15 % at ActorId64's integration).
#if defined(__clang__)
#pragma clang loop unroll(disable)
#elif defined(__GNUC__)
#pragma GCC unroll 1
#endif
                for (Dense const *bank : {&_table.secondary, &_table.primary})
                    for (auto const &slot : *bank)
                        if (slot.occupied())
                            f(slot.key, slot.value);
            } else {
                for (auto const &slot : primary_table())
                    if (slot.occupied())
                        f(slot.key, slot.value);
            }
        } else {
            for (auto const &it : _table)
                f(it.first, it.second);
        }
    }
};

/**
 * @brief An event type that wants to hear of a unicast the router could not deliver (Huly QB-163).
 * @details `qb::Event` declares `static void __undelivered__(Event &) noexcept`: the router calls it
 *          from the branch that drops a unicast with no handler subscribed under the destination id
 *          -- cold already, and out of line. Any other event type leaves the router exactly as it
 *          compiled before. The dispatch that skips a handler whose `is_alive()` is false (an actor
 *          killed earlier in the pass, not yet reaped) reports nothing, deliberately: that branch sits
 *          in the trampoline every event runs through, instantiated in the user's translation unit,
 *          and a call there moved GCC's inlining of the handler -- +4 % on the one-core pass probe,
 *          measured -- for a window that closes at the reap, after which the id is `not_found` here.
 */
template <typename _Event>
concept reports_undelivered = requires(_Event &event) {
    { _Event::__undelivered__(event) } noexcept;
};

/**
 * @brief An event type whose broadcast id names a core, and that is asked, before a broadcast is fanned
 *        out, whether it names the calling one (Huly QB-268).
 * @details `qb::Event` declares `static bool __broadcast_here__(Event &) noexcept`: `false` for a
 *          `BroadcastId(c)` of a core the engine does not run, which aliases the first core's queues --
 *          the event is then reported as a dead letter and reaches no handler. Asked only on the
 *          broadcast branch, never by a unicast; any other event type routes exactly as it compiled
 *          before.
 */
template <typename _Event>
concept scopes_broadcast = requires(_Event &event) {
    { _Event::__broadcast_here__(event) } noexcept -> std::same_as<bool>;
};

/**
 * @brief Raise `alive` on an event about to be destroyed, so that no later walk destroys it again.
 * @details The flag already means "do not destroy these bytes" -- `reply()`/`forward()` raise it on an
 *          original whose bytes moved on. Raised on every disposed event, it lets a core that stops on a
 *          handler's exception dispose whatever is left of an interrupted batch by walking ALL of it
 *          (`~VirtualCore`): the events already routed are skipped, the rest are destroyed once -- with
 *          no bookkeeping, no try block and no guard on the receive loop, which MSVC charged 3 to 7 %
 *          per event (TinyEvent, 19.51). Written before the destructor, never after it.
 */
template <typename _Event>
inline void
mark_disposed(_Event &event) noexcept {
    if constexpr (requires { event.__retire__(); })
        event.__retire__();
}

/**
 * @brief Base policy for event handling
 *
 * Defines common event handling operations like invocation and disposal.
 */
class EventPolicy {
public:
    EventPolicy() = default;

    ~EventPolicy() = default;

protected:
    /**
     * @brief Invoke a handler with an event
     *
     * If the event type satisfies qb::has_is_alive, handler liveness
     * is checked before dispatch.
     *
     * @tparam _Handler Handler type
     * @tparam _Event Event type
     * @param handler The handler to invoke
     * @param event The event to pass to the handler
     */
    template <typename _Handler, typename _Event>
    inline void
    invoke(_Handler &handler, _Event &event) const {
        // C++20: use concept directly instead of trait with ::value
        if constexpr (qb::has_is_alive<_Event>) {
            if (handler.is_alive())
                handler.on(event);
        } else
            handler.on(event);
    }

    /**
     * @brief Dispose of an event if necessary
     *
     * Non-trivially destructible events are explicitly destructed.
     * If the event satisfies qb::has_is_alive, destruction only occurs
     * when is_alive() returns false.
     *
     * @tparam _Event Event type
     * @param event The event to dispose
     */
    template <typename _Event>
    inline void
    dispose(_Event &event) const noexcept {
        if constexpr (!std::is_trivially_destructible_v<_Event>) {
            // C++20: use concept directly instead of trait with ::value
            if constexpr (qb::has_is_alive<_Event>) {
                if (!event.is_alive()) {
                    mark_disposed(event);
                    event.~_Event();
                }
            } else
                event.~_Event();
        }
    }
};

} // namespace internal

/**
 * @brief Single-Event Single-Handler router
 *
 * Routes a specific event type to a single handler.
 *
 * @tparam _RawEvent The event type
 * @tparam _Handler The handler type
 */
template <typename _RawEvent, typename _Handler>
class sesh : public internal::EventPolicy {
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    _Handler &_handler;

public:
    sesh() = delete;

    /**
     * @brief Constructs a SESH router with the given handler
     *
     * @param handler The handler that will receive events
     */
    explicit sesh(_Handler &handler) noexcept
        : _handler(handler) {}

    /**
     * @brief Routes an event to the handler
     *
     * @tparam _CleanEvent Whether to clean up the event after routing
     * @param event The event to route
     */
    template <bool _CleanEvent = true>
    void
    route(_RawEvent &event) {
        invoke(_handler, event);
        if constexpr (_CleanEvent)
            dispose(event);
    }
};

/**
 * @brief Single-Event Multiple-Handler router (generic version)
 *
 * Routes a specific event type to multiple handlers based on destination IDs.
 *
 * @tparam _RawEvent The event type
 * @tparam _Handler The handler type (void for heterogeneous handlers)
 */
template <typename _RawEvent, typename _Handler = void>
class semh : public internal::EventPolicy {
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    internal::key_table<_HandlerId, _Handler *> _subscribed_handlers;

public:
    semh() = default;

    ~semh() = default;

    /**
     * @brief Routes an event to the appropriate handler
     *
     * If the event has a broadcast destination, it's sent to all handlers.
     * Otherwise, it's sent to the handler that matches the destination ID.
     *
     * @tparam _CleanEvent Whether to clean up the event after routing
     * @param event The event to route
     */
    template <bool _CleanEvent = true>
    void
    route(_RawEvent &event) {
        if constexpr (_CleanEvent && !std::is_trivially_destructible_v<_RawEvent>) {
            // A destructor, not a try/catch: it disposes on return AND while a handler's exception
            // unwinds (dispose() applies the is_alive rule itself), and unlike a try block it does not
            // stop MSVC from inlining this function into its caller.
            struct dispose_on_exit {
                semh      &router;
                _RawEvent &event;
                ~dispose_on_exit() {
                    router.dispose(event);
                }
            } const guard{*this, event};
            dispatch_event(event);
        } else {
            dispatch_event(event);
        }
    }

private:
    // The dispatch of `route()`: a member forced inline on MSVC, not a lambda -- MSVC 19.51 compiled
    // the lambda out of line, a call per routed event (the default events' resolvers: 191 instructions
    // inline before, an 11-instruction stub plus a 193-instruction call after).
    QB_MSVC_FORCEINLINE void
    dispatch_event(_RawEvent &event) {
        // C++20: use concept directly
        if constexpr (qb::has_is_broadcast<_HandlerId>) {
            if (event.getDestination().is_broadcast()) {
                if constexpr (internal::scopes_broadcast<_RawEvent>) {
                    if (!_RawEvent::__broadcast_here__(event))
                        return; // another core's: reported, disposed by route() (Huly QB-268)
                }
                // Snapshot before dispatch: a handler may (un)subscribe on THIS table.
                static thread_local internal::snapshot_stack<_Handler *> bcast_snapshot;
                const std::size_t                                        base = bcast_snapshot.end;
                struct RestoreSnapshot {
                    internal::snapshot_stack<_Handler *> &entries;
                    std::size_t                           base;
                    ~RestoreSnapshot() {
                        entries.end = base;
                    }
                } restore{bcast_snapshot, base};
                _subscribed_handlers.for_each([](auto const &, _Handler *const handler) { bcast_snapshot.push(handler); });
                const std::size_t end = bcast_snapshot.end;
                for (std::size_t i = base; i < end; ++i)
                    invoke(*bcast_snapshot.slots[i], event);
                return;
            }
        }

        if (auto *const handler = _subscribed_handlers.find(event.dest); likely(handler != nullptr)) [[likely]]
            invoke(**handler, event);
        else [[unlikely]] {
            if constexpr (internal::reports_undelivered<_RawEvent>)
                _RawEvent::__undelivered__(event);
        }
    }

public:
    /**
     * @brief Subscribe a handler to receive events
     *
     * @param handler The handler to subscribe
     */
    void
    subscribe(_Handler &handler) noexcept {
        _subscribed_handlers.insert_or_assign(handler.id(), &handler);
    }

    /**
     * @brief Unsubscribe a handler by ID
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) noexcept {
        _subscribed_handlers.erase(id);
    }
};

/**
 * @brief Single-Event Multiple-Handler router (heterogeneous version).
 *
 * Specialization that supports different handler types for the same event type.
 *
 * @tparam _RawEvent The event type
 *
 * @details
 * **Dispatch strategy (finding 2.7):** instead of storing a virtual
 * `IHandlerResolver` per subscribed handler — which costs one heap allocation
 * per subscription and one vtable lookup per dispatch — this implementation
 * stores a pair `{ void* handler, trampoline_fn }`. The trampoline is a
 * `static` function instantiated per handler type that performs the
 * `static_cast<_Handler*>` and calls `handler.on(event)` directly.
 *
 * This saves:
 * - 1 heap allocation per `subscribe<_Handler>()` call.
 * - 1 indirect virtual call per routed event.
 * - A vtable pointer (8 bytes) in every subscription entry.
 */
template <typename _RawEvent>
class semh<_RawEvent, void> : public internal::EventPolicy {
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    using Trampoline = void (*)(void *, _RawEvent &);

    struct Entry {
        void      *handler  = nullptr;
        Trampoline dispatch = nullptr;
    };

    // Only subscribe() creates a live Entry, with a non-null typed trampoline.
    // Erasure assigns Entry{}, so the dispatch pointer is an exact occupancy bit.
    // Keep this opt-in private: arbitrary key_table values may legitimately be null.
    struct EntryOccupancy {
        template <typename Key, typename Value>
        struct Slot {
            static_assert(std::is_same_v<Value, Entry>);
            Key   key{};
            Value value{};

            bool
            occupied() const noexcept {
                return value.dispatch != nullptr;
            }
            void
            mark_used() const noexcept {
                assert(value.dispatch != nullptr);
            }
            void
            mark_unused() const noexcept {
                assert(value.dispatch == nullptr);
            }
        };
    };

    using Table = internal::key_table<_HandlerId, Entry, EntryOccupancy>;

    /**
     * @brief Typed trampoline: performs the `void*` → `_Handler*` recast and
     *        invokes the handler's `on(event)` method, honouring the
     *        `has_is_alive` protocol.
     */
    template <typename _Handler>
    static void
    dispatch_trampoline(void *opaque_handler, _RawEvent &event) {
        auto &handler = *static_cast<_Handler *>(opaque_handler);
        if constexpr (qb::has_is_alive<_RawEvent>) {
            if (handler.is_alive())
                handler.on(event);
        } else {
            handler.on(event);
        }
    }

    Table _subscribed_handlers;

public:
    semh()           = default;
    ~semh() noexcept = default;

    /**
     * @brief Routes an event to the appropriate handler
     *
     * If the event has a broadcast destination, it's sent to all handlers.
     * Otherwise, it's sent to the handler that matches the destination ID.
     *
     * @tparam _CleanEvent Whether to clean up the event after routing
     * @param event The event to route
     */
    template <bool _CleanEvent = false>
    void
    route(_RawEvent &event) const {
        if constexpr (_CleanEvent && !std::is_trivially_destructible_v<_RawEvent>) {
            // A destructor, not a try/catch (see the generic semh::route): disposes on return and
            // on a handler throw, and keeps this function inlinable on MSVC.
            struct dispose_on_exit {
                semh const &router;
                _RawEvent  &event;
                ~dispose_on_exit() {
                    router.dispose(event);
                }
            } const guard{*this, event};
            dispatch_event(event);
        } else {
            dispatch_event(event);
        }
    }

private:
    // The unicast/broadcast dispatch of `route()`. A member forced inline on MSVC, not a lambda in
    // route(): MSVC 19.51 compiled that lambda out of line, so every event of every type -- trivial
    // ones included -- jumped to it from route() (TinyEvent 1-core, part of +12.9 %).
    QB_MSVC_FORCEINLINE void
    dispatch_event(_RawEvent &event) const {
        if constexpr (qb::has_is_broadcast<_HandlerId> && Table::broadcast_secondary) {
            if constexpr (Table::primary_excludes_secondary) {
                // This trait proves that every secondary projection lies beyond the primary bank.
                // A successful primary lookup has already checked bounds, occupancy and the FULL key.
                // Classify the bank only on a miss; broadcasts and service instances still use
                // the existing secondary dispatch, and stale primary keys remain undelivered.
                auto const *const entry = _subscribed_handlers.find_primary(event.getDestination());
                if (likely(entry != nullptr)) {
                    dispatch_entry(entry, event);
                } else if (Table::is_secondary(event.getDestination())) {
                    dispatch_secondary(event);
                } else {
                    dispatch_entry(nullptr, event);
                }
            } else {
                if (unlikely(Table::is_secondary(event.getDestination()))) {
                    dispatch_secondary(event);
                    return;
                }
                dispatch_entry(_subscribed_handlers.find_primary(event.getDestination()), event);
            }
        } else {
            if constexpr (qb::has_is_broadcast<_HandlerId>) {
                // Keep the broadcast walk out of line so unicast has no snapshot prologue.
                if (unlikely(event.getDestination().is_broadcast())) {
                    route_broadcast(event);
                    return;
                }
            }
            dispatch_entry(_subscribed_handlers.find(event.getDestination()), event);
        }
    }

    QB_MSVC_FORCEINLINE void
    dispatch_entry(Entry const *const entry, _RawEvent &event) const {
        if (likely(entry != nullptr)) [[likely]] {
            const auto  dispatch = entry->dispatch;
            auto *const target   = entry->handler;
            QB_ASSUME(dispatch != nullptr);
            dispatch(target, event);
        } else [[unlikely]] {
            if constexpr (internal::reports_undelivered<_RawEvent>)
                _RawEvent::__undelivered__(event);
        }
    }

    // dispatch_event()'s secondary half: the broadcast walk, or a key of the secondary bank
    QB_NOINLINE void
    dispatch_secondary(_RawEvent &event) const {
        if (event.getDestination().is_broadcast()) {
            route_broadcast(event);
            return;
        }
        dispatch_entry(_subscribed_handlers.find_secondary(event.getDestination()), event);
    }

public:
    /**
     * @brief The broadcast half of `route()`: every subscribed handler, from a snapshot.
     * @details A handler invoked here may (un)subscribe on THIS table — e.g. spawning an actor
     *          registers KillEvent, inserting a new entry. That insert can grow the dense vector
     *          (and, on the hash-map fallback, rehash the release flat map), REALLOCATING the
     *          entry array under a live range-for iterator (heap-use-after-free — invisible to
     *          the debug std::unordered_map the sanitizer presets used to select, so it only bit
     *          in release). Snapshot first, then dispatch: handlers are not destroyed until
     *          end-of-frame (removal is deferred) and the trampoline re-checks is_alive(), so
     *          the snapshotted pointers stay valid. A thread_local buffer with base/restore
     *          keeps nested broadcasts allocation-free and correct (each nested route
     *          pushes/pops its own [base,end)).
     */
    QB_NOINLINE QB_COLD void
    route_broadcast(_RawEvent &event) const {
        if constexpr (internal::scopes_broadcast<_RawEvent>) {
            if (!_RawEvent::__broadcast_here__(event))
                return; // another core's: reported, disposed by route() (Huly QB-268)
        }
        static thread_local internal::snapshot_stack<Entry> bcast_snapshot;
        const std::size_t                                   base = bcast_snapshot.end;
        struct RestoreSnapshot {
            internal::snapshot_stack<Entry> &entries;
            std::size_t                      base;
            ~RestoreSnapshot() {
                entries.end = base;
            }
        } restore{bcast_snapshot, base};
        _subscribed_handlers.for_each([](auto const &, Entry const &entry) { bcast_snapshot.push(entry); });
        const std::size_t end = bcast_snapshot.end;
        for (std::size_t i = base; i < end; ++i) {
            // `subscribe()` always sets both fields atomically, so the function pointer is
            // never null for a live entry (finding 2.17, C++23 `[[assume]]`); load into
            // locals so the predicate is side-effect-free.
            const auto  dispatch = bcast_snapshot.slots[i].dispatch;
            auto *const target   = bcast_snapshot.slots[i].handler;
            QB_ASSUME(dispatch != nullptr);
            dispatch(target, event);
        }
    }

    /**
     * @brief Subscribe a handler to receive events
     *
     * @tparam _Handler The handler type
     * @param handler The handler to subscribe
     */
    template <typename _Handler>
    void
    subscribe(_Handler &handler) noexcept {
        _subscribed_handlers.insert_or_assign(handler.id(), Entry{static_cast<void *>(&handler), &dispatch_trampoline<_Handler>});
    }

    /**
     * @brief Unsubscribe a handler by ID
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) noexcept {
        _subscribed_handlers.erase(id);
    }
};

/**
 * @brief Multiple-Event Single-Handler router
 *
 * Routes multiple event types to a single handler based on event type IDs.
 *
 * @tparam _RawEvent The raw event base type
 * @tparam _Handler The handler type
 * @tparam _CleanEvent Whether to clean up events after routing
 */
template <typename _RawEvent, typename _Handler, bool _CleanEvent = true>
class mesh {
public:
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

private:
    /**
     * @brief Interface for event resolution
     *
     * Abstracts the process of resolving and handling events
     * of different types.
     */
    class IEventResolver {
    public:
        virtual ~IEventResolver() = default;

        /**
         * @brief Resolve and invoke a handler for the event
         *
         * @param handler The handler to invoke
         * @param event The event to route
         */
        virtual void resolve(_Handler &handler, _RawEvent &event) const = 0;
    };

    /**
     * @brief Concrete event resolver for a specific event type
     *
     * @tparam _Event The specific event type
     */
    template <typename _Event>
    class EventResolver
        : public IEventResolver
        , public internal::EventPolicy {
    public:
        EventResolver() = default;

        /**
         * @brief Resolve and invoke a handler for the event
         *
         * @param handler The handler to invoke
         * @param event The event to route
         */
        void
        resolve(_Handler &handler, _RawEvent &event) const final {
            auto &revent = reinterpret_cast<_Event &>(event);
            invoke(handler, revent);
            if constexpr (_CleanEvent)
                dispose(revent);
        }
    };

    _Handler                                                    &_handler;
    qb::unordered_map<_EventId, std::unique_ptr<IEventResolver>> _registered_events;

public:
    mesh() = delete;

    /**
     * @brief Constructs a MESH router with the given handler
     *
     * @param handler The handler that will receive events
     */
    explicit mesh(_Handler &handler) noexcept
        : _handler(handler) {}

    ~mesh() noexcept = default;

    /**
     * @brief Routes an event to the handler
     *
     * Unregistered dynamic events risk leaking memory, so this
     * intentionally throws via .at() on missing IDs.
     *
     * @param event The event to route
     */
    void
    route(_RawEvent &event) {
        _registered_events.at(event.getID())->resolve(_handler, event);
    }

    /**
     * @brief Subscribe to events of a specific type
     *
     * @tparam _Event The event type to subscribe to
     */
    template <typename _Event>
    void
    subscribe() {
        _registered_events.try_emplace(_RawEvent::template type_to_id<_Event>(), std::make_unique<EventResolver<_Event>>());
    }

    /**
     * @brief Unsubscribe from events of a specific type
     *
     * @tparam _Event The event type to unsubscribe from
     */
    template <typename _Event>
    void
    unsubscribe() {
        _registered_events.erase(_RawEvent::template type_to_id<_Event>());
    }

    /**
     * @brief Unsubscribe from all event types
     */
    void
    unsubscribe() {
        _registered_events.clear();
    }
};

/**
 * @brief Multiple-Event Multiple-Handler router (generic version)
 *
 * Routes multiple event types to multiple handlers based on event type and handler IDs.
 *
 * @tparam _RawEvent The raw event base type
 * @tparam _CleanEvent Whether to clean up events after routing
 * @tparam _Handler The handler type (void for heterogeneous handlers)
 */
template <typename _RawEvent, bool _CleanEvent = true, typename _Handler = void>
class memh {
public:
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

private:
    /**
     * @brief Interface for event resolution
     *
     * Abstracts the process of resolving and handling events of different types.
     */
    class IEventResolver {
    public:
        virtual ~IEventResolver() = default;

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         */
        virtual void resolve(_RawEvent &event) = 0;

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        virtual void unsubscribe(_HandlerId const &id) = 0;
    };

    /**
     * @brief Concrete event resolver for a specific event type
     *
     * @tparam _Event The specific event type
     */
    template <typename _Event>
    class EventResolver
        : public IEventResolver
        , public semh<_Event, _Handler> {
        using _HandlerId = typename _RawEvent::id_handler_type;

    public:
        /**
         * @brief Constructor
         */
        EventResolver() noexcept
            : semh<_Event, _Handler>() {}

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         */
        void
        resolve(_RawEvent &event) final {
            auto &revent = reinterpret_cast<_Event &>(event);
            semh<_Event, _Handler>::template route<_CleanEvent>(revent);
        }

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        void
        unsubscribe(_HandlerId const &id) final {
            semh<_Event, _Handler>::unsubscribe(id);
        }
    };

    internal::key_table<_EventId, std::unique_ptr<IEventResolver>> _registered_events;

public:
    memh()           = default;
    ~memh() noexcept = default;

    /**
     * @brief Routes an event to the appropriate handlers with error handling
     *
     * @tparam _Func Type of the error handling function
     * @param event The event to route
     * @param onError Function to call if the event type is not registered
     */
    template <typename _Func>
    void
    route(_RawEvent &event, _Func const &onError) const {
        if (auto *const entry = _registered_events.find(event.getID()); likely(entry != nullptr)) {
            // Registered entries always own a valid unique_ptr; materialise the
            // raw pointer into a local so `QB_ASSUME` sees a side-effect-free
            // predicate (Clang's `-Wassume`) and can elide the null check.
            auto *const resolver = entry->get();
            QB_ASSUME(resolver != nullptr);
            resolver->resolve(event);
        } else {
            onError(event);
        }
    }

    /**
     * @brief Subscribe a handler to events of a specific type
     *
     * @tparam _Event The event type to subscribe to
     * @param handler The handler to subscribe
     */
    template <typename _Event>
    void
    subscribe(_Handler &handler) {
        const auto id = _RawEvent::template type_to_id<_Event>();
        if (auto *const entry = _registered_events.find(id)) {
            // The entry keyed by `type_to_id<_Event>()` was created by this function with
            // `EventResolver<_Event>` and nothing else ever writes the table: a static_cast
            // is exact, and the RTTI walk it replaces ran once per subscription -- seven
            // per actor -- in an actor's lifetime cost.
            static_cast<EventResolver<_Event> *>(entry->get())->subscribe(handler);
        } else {
            auto resolver = std::make_unique<EventResolver<_Event>>();
            resolver->subscribe(handler);
            _registered_events.insert_or_assign(id, std::move(resolver));
        }
    }

    /**
     * @brief Unsubscribe a handler from events of a specific type
     *
     * @tparam _Event The event type to unsubscribe from
     * @param handler The handler to unsubscribe
     */
    template <typename _Event>
    void
    unsubscribe(_Handler &handler) const {
        if (auto *const entry = _registered_events.find(_RawEvent::template type_to_id<_Event>()))
            (*entry)->unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler from all event types
     *
     * @param handler The handler to unsubscribe
     */
    void
    unsubscribe(_Handler const &handler) const {
        unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler by ID from all event types
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) const {
        _registered_events.for_each([&id](auto const &, auto const &resolver) { resolver->unsubscribe(id); });
    }
};

/**
 * @brief Multiple-Event Multiple-Handler router (heterogeneous version)
 *
 * Specialization that supports different handler types for multiple event types.
 *
 * @tparam _RawEvent The raw event base type
 * @tparam _CleanEvent Whether to clean up events after routing
 */
template <typename _RawEvent, bool _CleanEvent>
class memh<_RawEvent, _CleanEvent, void> {
public:
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

private:
    /**
     * @brief Interface for event disposal
     *
     * Abstracts the process of cleaning up events of different types.
     */
    class IDisposer {
    public:
        virtual ~IDisposer() = default;

        /**
         * @brief Dispose of an event
         *
         * @param event The event to dispose
         */
        virtual void dispose(_RawEvent *event) = 0;
    };

    /**
     * @brief Concrete disposer for a specific event type
     *
     * @tparam T The specific event type
     */
    template <typename T>
    class Disposer : public IDisposer {
    public:
        /**
         * @brief Dispose of an event
         *
         * Calls the destructor for non-trivially destructible event types.
         *
         * @param event The event to dispose
         */
        void
        dispose(_RawEvent *event) final {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                auto *typed = reinterpret_cast<T *>(event);
                if constexpr (qb::has_is_alive<T>) {
                    if (!typed->is_alive()) {
                        internal::mark_disposed(*typed); // see internal::mark_disposed
                        typed->~T();
                    }
                } else {
                    typed->~T();
                }
            } else {
                (void) event;
            }
        }
    };

    QB_ABI_ANCHOR static inline qb::unordered_map<_EventId, std::unique_ptr<IDisposer>> _disposers;
    QB_ABI_ANCHOR static inline std::mutex                                              _disposers_mtx;

    /**
     * @brief Per-router memo of resolved disposers, so the shared map is consulted once per type.
     * @details `_disposers` is `static` — one map for the whole process — and every read of it used
     *          to take `_disposers_mtx`. That lock sits on a path reached at full event rate: an
     *          event whose type has no resolver on this core falls into `route()`'s `else` branch,
     *          and `broadcast<E>()` produces exactly that on every core where no actor subscribed
     *          to `E` (`VirtualCore::__receive_events__`'s `onError` treats it as normal — it only
     *          logs for non-broadcast destinations). So a broadcasting system serialised all of its
     *          cores through one process-global mutex, which is the one thing a
     *          thread-per-`VirtualCore` engine must never do. Measured cost of a single lookup:
     *          9.0 ns at 1 thread, 24.3 at 2, 50.0 at 4, **146.7 ns at 8** — against a same-core
     *          dispatch budget of ~40-70 ns per event, and getting worse with every core added.
     *
     *          Caching makes the steady state a plain uncontended hash hit (~1-2 ns, flat in core
     *          count). It is sound because disposers are only ever ADDED — `try_emplace` into a
     *          `static` map of `unique_ptr` that lives for the whole program — so a memoised raw
     *          pointer can never dangle, and a type not yet registered is simply not cached and is
     *          retried on the next event.
     *
     *          Not synchronised, deliberately: `memh` is already a single-threaded-per-instance
     *          class (`_registered_events` is a plain table mutated by `subscribe`/`unsubscribe`);
     *          the `static` disposer map was its *only* shared state. Each `VirtualCore` owns its
     *          own router and touches it exclusively from its own thread.
     */
    mutable internal::key_table<_EventId, IDisposer *> _disposer_cache;

    /**
     * @brief Resolve the disposer for @p id, consulting the shared map at most once per type.
     * @return The disposer, or `nullptr` if none is registered yet.
     */
    [[nodiscard]] IDisposer *
    find_disposer(_EventId const &id) const {
        if (auto *const cached = _disposer_cache.find(id); likely(cached != nullptr))
            return *cached;

        IDisposer *resolved = nullptr;
        {
            std::lock_guard lk(_disposers_mtx);
            if (const auto dit = _disposers.find(id); dit != _disposers.cend())
                resolved = dit->second.get();
        }
        // Only memoise a hit: a miss means the type's disposer has not been registered yet, and
        // caching the null would make this router blind to it forever.
        if (resolved)
            _disposer_cache.insert_or_assign(id, resolved);
        return resolved;
    }

public:
    /**
     * @brief Interface for event resolution
     *
     * Abstracts the process of resolving and handling events of different types.
     * Public so an owner can `install<_Event>()` a resolver of its own for a type whose handlers
     * it keeps elsewhere — `VirtualCore` does this for the five default actor events, which it
     * dispatches through its actor registry instead of a per-type handler table.
     */
    class IEventResolver {
    public:
        /**
         * @param owns_handlers true for a resolver whose handlers live in this router and are
         *        removed through `unsubscribe(id)`; false for an installed resolver that keeps
         *        no handler of its own, which `unsubscribe(id)` then never visits and
         *        `subscribe<_Event>()` refuses to treat as an `EventResolver<_Event>`.
         */
        explicit IEventResolver(bool owns_handlers = true) noexcept
            : owns_handlers(owns_handlers) {}
        virtual ~IEventResolver() = default;

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         * @details With `_CleanEvent=true`, the resolver owns disposal on both
         *          normal return and exception. The built-in EventResolver does
         *          this through `semh::route<true>`; an installed resolver must
         *          uphold the same contract.
         */
        virtual void resolve(_RawEvent &event) const = 0;

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        virtual void unsubscribe(_HandlerId const &id) = 0;

        const bool owns_handlers;
    };

private:
    /**
     * @brief Concrete event resolver for a specific event type
     *
     * @tparam _Event The specific event type
     */
    template <typename _Event>
    class EventResolver
        : public IEventResolver
        , public semh<_Event> {
    public:
        /**
         * @brief Constructor
         */
        EventResolver() noexcept
            : semh<_Event>() {}

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         */
        void
        resolve(_RawEvent &event) const final {
            auto &revent = reinterpret_cast<_Event &>(event);
            semh<_Event>::template route<_CleanEvent>(revent);
        }

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        void
        unsubscribe(typename _RawEvent::id_handler_type const &id) final {
            semh<_Event>::unsubscribe(id);
        }
    };

    internal::key_table<_EventId, std::unique_ptr<IEventResolver>> _registered_events;

    /**
     * @brief The resolvers that hold handlers — every `EventResolver<_Event>` this router created,
     *        in creation order, and none of the installed ones.
     * @details `unsubscribe(id)` walks this instead of the whole table: a handler's removal must
     *          visit every table it might be in, and the installed resolvers are by definition
     *          tables it is in nowhere. Raw pointers are safe because the table never erases a
     *          resolver and a `unique_ptr`'s pointee does not move when the dense vector grows.
     */
    std::vector<IEventResolver *> _owning;

public:
    /**
     * @brief Helper to ensure safe disposal of events
     *
     * Registers a disposer for a specific event type.
     *
     * @tparam T The event type to register a disposer for
     */
    template <typename T>
    struct SafeDispose {
        SafeDispose() {
            std::lock_guard lk(_disposers_mtx);
            _disposers.try_emplace(_RawEvent::template type_to_id<T>(), std::make_unique<Disposer<T>>());
        }
        ~SafeDispose() = default;
    };

    memh()           = default;
    ~memh() noexcept = default;

    /**
     * @brief Routes an event to the appropriate handlers with error handling
     *
     * @tparam _Func Type of the error handling function
     * @param event The event to route
     * @param onError Function to call if the event type is not registered
     */
    template <typename _Func>
    void
    route(_RawEvent &event, _Func const &onError) const {
        if (auto *const entry = _registered_events.find(event.getID()); likely(entry != nullptr)) {
            // Every entry inserted via `subscribe()` owns a valid resolver:
            // materialise the raw pointer into a local so `QB_ASSUME` sees a
            // side-effect-free predicate and can elide the null check around
            // the virtual call (finding 2.17).
            auto *const resolver = entry->get();
            QB_ASSUME(resolver != nullptr);
            resolver->resolve(event);
        } else if constexpr (_CleanEvent) {
            // Free the payload of an event nobody subscribed to, after `onError` returns AND while
            // an exception it threw unwinds -- a destructor, not a try/catch: a try block here, even
            // on this cold branch, stopped MSVC from inlining route() into
            // VirtualCore::__receive_events__ (a call per event, TinyEvent +12.9 % on MSVC 19.51).
            //
            // The lookup is deliberately tolerant (find, never `.at()`): `.at()` would throw
            // std::out_of_range, and that exception propagates out of VirtualCore::__receive_events__
            // / __workflow__ — start_thread() catches it and flags ExceptionThrown, killing the whole
            // VirtualCore (every actor on it) for one misaddressed event.
            //
            // A missing disposer used to be the NORM here, not a corner case: disposers were
            // registered only by `subscribe<T>()`, so a type that was pushed but never subscribed
            // anywhere had none and this branch silently leaked its `std::string` / `std::vector`
            // members on every such event — unbounded, and reachable by an ordinary refactor mistake.
            // `router::ensure_disposer<Event, T>()` now runs at every enqueue funnel
            // (VirtualCore::push/send, Pipe::push/allocated_push), so the disposer always exists by
            // the time an event can reach this branch. The find-and-skip stays as the no-throw
            // safety net.
            //
            // This is the hot one: `broadcast<E>()` lands here on every core with no subscriber for
            // `E`. Goes through the per-router memo — see `_disposer_cache`.
            struct dispose_on_exit {
                memh const &router;
                _RawEvent  &event;
                ~dispose_on_exit() {
                    router.dispose(event);
                }
            } const guard{*this, event};
            onError(event);
        } else {
            onError(event);
        }
    }

    /**
     * @brief Destroy the payload of an event that will NOT be routed.
     * @param event The event whose (possibly non-trivial) members must be destroyed.
     * @details Runs the same type-erased disposer `route()` applies after handling, for callers
     *          that own a raw event and decide to drop it without delivery — e.g.
     *          `VirtualCore::__pump_activations__` discarding an activation stash when an actor's
     *          async `onInit()` fails or it is killed during init, or dropping an event that
     *          overflowed the stash cap. Without it, a `push`'d event carrying a `std::string` /
     *          `std::vector` would leak its heap storage. The disposer is guaranteed to exist for
     *          any enqueued non-trivially-destructible type by `router::ensure_disposer<>()` at the
     *          enqueue funnels; the absent-disposer branch remains only as a no-throw safety net.
     */
    void
    dispose(_RawEvent &event) const {
        if (auto *const disposer = find_disposer(event.getID()))
            disposer->dispose(&event);
    }

    /**
     * @brief Subscribe a handler to events of a specific type
     *
     * @tparam _Event The event type to subscribe to
     * @tparam _Handler The handler type
     * @param handler The handler to subscribe
     */
    template <typename _Event, typename _Handler>
    QB_ABI_ANCHOR void
    subscribe(_Handler &handler) {
        static const SafeDispose<_Event> o{};

        const auto id = _RawEvent::template type_to_id<_Event>();
        if (auto *const entry = _registered_events.find(id)) {
            // Same invariant as the typed memh's subscribe above: the table is keyed by the
            // exact event type and only ever holds `EventResolver<_Event>` under that key —
            // unless the owner `install()`ed its own resolver there, in which case subscribing
            // through the table is the owner's mistake (the recast below would be UB), not a
            // routing decision this router can make for it.
            assert((*entry)->owns_handlers
                   && "memh::subscribe<_Event>: an installed resolver owns this event type; "
                      "register its handlers through the owner, not the router");
            static_cast<EventResolver<_Event> *>(entry->get())->subscribe(handler);
        } else {
            auto resolver = std::make_unique<EventResolver<_Event>>();
            resolver->subscribe(handler);
            _owning.push_back(resolver.get());
            _registered_events.insert_or_assign(id, std::move(resolver));
        }
    }

    /**
     * @brief Install a caller-owned resolver for `_Event`, replacing any resolver under that key.
     * @tparam _Event The event type the resolver answers for.
     * @param resolver Resolves every `_Event` routed here; `route()` calls it exactly as it calls
     *        an `EventResolver<_Event>`, with `_CleanEvent` disposal being the resolver's own
     *        business. Its `owns_handlers` decides whether `unsubscribe(id)` visits it.
     * @details Registers `_Event`'s disposer on the shared map like `subscribe()` does, so an
     *          installed type is disposable on every core. Must be called before the first
     *          `subscribe<_Event>()` on this router — replacing a resolver that already holds
     *          handlers drops those handlers on the floor, and the assertion in `subscribe()`
     *          only guards the other order.
     */
    template <typename _Event>
    void
    install(std::unique_ptr<IEventResolver> resolver) {
        static const SafeDispose<_Event> o{};

        assert(resolver != nullptr && "memh::install<_Event>: null resolver");
        const auto id = _RawEvent::template type_to_id<_Event>();
        assert(_registered_events.find(id) == nullptr && "memh::install<_Event>: a resolver for this event type already exists");
        if (resolver->owns_handlers)
            _owning.push_back(resolver.get());
        _registered_events.insert_or_assign(id, std::move(resolver));
    }

    /**
     * @brief Unsubscribe a handler from events of a specific type
     *
     * @tparam _Event The event type to unsubscribe from
     * @tparam _Handler The handler type
     * @param handler The handler to unsubscribe
     */
    template <typename _Event, typename _Handler>
    void
    unsubscribe(_Handler const &handler) const {
        if (auto *const entry = _registered_events.find(_RawEvent::template type_to_id<_Event>()))
            (*entry)->unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler from all event types
     *
     * @tparam _Handler The handler type
     * @param handler The handler to unsubscribe
     */
    template <typename _Handler>
    void
    unsubscribe(_Handler const &handler) const {
        unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler by ID from all event types
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) const {
        for (IEventResolver *const resolver : _owning)
            resolver->unsubscribe(id);
    }
};

/**
 * @brief Guarantee a type-erased disposer exists for a NON-TRIVIALLY-DESTRUCTIBLE event type.
 *
 * The disposer registry that frees an event the framework must DROP (undeliverable residue at
 * shutdown, a stash overflow, or an event whose type no actor on the destination core subscribed
 * to) is populated only as a side effect of `memh::subscribe<E>()`. A type that is enqueued but
 * never subscribed anywhere therefore has NO disposer, so every drop path silently leaks its
 * `std::string` / `std::vector` members — reachable by ordinary means (a push to an actor that
 * does not handle that type). Call this at every ENQUEUE site: that is the one place which
 * statically knows each type entering the system.
 *
 * `if constexpr` emits NOTHING for the common trivially-destructible event; a non-trivial type
 * pays one relaxed load of a flag and a well-predicted branch per enqueue -- not a guarded
 * `static const`: a guard's acquire load costs seven instructions a call and keeps every store
 * before it alive across it, so the event header's zero-initialised fields were stored twice
 * (`qb::detail::type_id_for`, Event.h, measured the same shape on the push path).
 */
template <typename _RawEvent, typename _Event>
QB_NOINLINE QB_COLD void register_disposer(std::atomic<bool> &registered) noexcept;

template <typename _RawEvent, typename _Event>
QB_ABI_ANCHOR inline void
ensure_disposer() noexcept {
    if constexpr (!std::is_trivially_destructible_v<_Event>) {
        static std::atomic<bool> registered{false}; // constant-initialised: no guard
        if (!registered.load(std::memory_order_relaxed)) [[unlikely]]
            register_disposer<_RawEvent, _Event>(registered);
    }
}

/// The cold half of `ensure_disposer`: the one registration (the guarded static that was on the hot path,
/// idempotent under the disposers' lock, so racing threads agree), then the flag. Out of line: the enqueue
/// sites are the hot path of every event, and this runs once per type.
template <typename _RawEvent, typename _Event>
QB_NOINLINE QB_COLD void
register_disposer(std::atomic<bool> &registered) noexcept {
    static const typename memh<_RawEvent>::template SafeDispose<_Event> once{};
    (void) once;
    registered.store(true, std::memory_order_relaxed);
}

} // namespace qb::router

#endif // QB_EVENT_ROUTER_H
