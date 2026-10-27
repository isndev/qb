/**
 * @file qb/core/Actor.cpp
 * @brief Implementation of the Actor class for the QB framework
 *
 * This file contains the implementation of the Actor class which forms the foundation
 * of the actor model in the QB framework. It includes event handling, actor lifecycle
 * management, and inter-actor communication mechanisms.
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
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <qb/core/Actor.h>
#include <qb/core/VirtualCore.h> // also carries Actor's template bodies (was qb/core/Actor.tpp); <windows.h> on Windows
#include <qb/io/async/listener.h>
#include <qb/system/time.h>

namespace qb {

// ---------------------------------------------------------------------------
// ask() pattern — per-worker-thread correlation registry (Layer 3).
// Strictly mono-thread per VirtualCore: a plain thread_local table, no locks.
// Slots are owned by the awaiter living in the asking coroutine's frame; the
// registry only stores raw pointers, in a slot table the correlation id indexes.
// ---------------------------------------------------------------------------
namespace detail {
namespace {
/**
 * @brief The pending-ask registry: a slot table the correlation id INDEXES, one per worker thread.
 * @details Every `qb::ask` takes an entry before it sends and gives it back when it resumes, so
 *          this is the one container on the ask hot path. It used to be a hash table keyed by a
 *          per-core counter — a multiply and a probe per insert, per lookup and per erase, plus
 *          the backward shift an erase pays to leave no tombstone — and `perf` on
 *          savina/bank-transaction (1c, g++-14) put it at 10–12 % of the core. Nothing about the
 *          id needed hashing: the registry hands the id out itself, so it can make the id SAY
 *          where the entry is. A correlation id is
 *
 *              [ core index : 16 ][ generation : 26 ][ slot index : 22 ]
 *
 *          — the low bits index this table directly, the generation is the entry's reuse count
 *          at the time it was taken (a stale id, one whose entry has since been released, misses
 *          on the compare), and the high 16 bits name the owning core, as before, so an id can
 *          never resolve an entry of another core's table. Take = pop the free list AND bind the
 *          owner's slot (one call, not a take followed by a register: `perf` priced every
 *          out-of-line trip through here, TLS guard included, at about as much as the work it
 *          did), look-up = index + compare, release = push; no multiply, no probe, no allocation
 *          once warm. An entry belongs permanently to one actor SLOT of one core, and records the
 *          exact incarnation (the complete 64-bit `ActorId`) that last held it: a late reply must
 *          still identify its owner after the awaiter is gone AND after later asks reuse the entry
 *          (Huly QB-1104). Each slot has its own FIFO, which the slot's next incarnations reuse --
 *          a late reply addressed to a dead incarnation never reaches them, since an exact identity
 *          never resolves a replacement (QB-971) -- and a new slot takes only unassigned entries,
 *          never another slot's retired ones. Thus storage follows the sum of the slots'
 *          high-water demands, rather than just the maximum simultaneous demand of the core; the
 *          pool reuses freed slots first, so the slot count stays the core's peak population. The
 *          slot queues are paged and lazy; a warm exchange allocates nothing. An exhausted
 *          generation retires the entry permanently, never wrapping its id. Id 0 stays reserved
 *          ("not an ask"). Strictly mono-thread, like everything else in here.
 */
class ask_table {
public:
    static constexpr unsigned      slot_bits = 22;
    static constexpr unsigned      gen_bits  = 26;
    static constexpr std::uint64_t slot_mask = (std::uint64_t{1} << slot_bits) - 1;
    static constexpr std::uint32_t gen_max   = (std::uint32_t{1} << gen_bits) - 1;

    // Constant-initialised: every member has a constant default, so `tls_ask_slots` is `constinit` and a
    // registry trip is a plain TLS load -- no `__tls_guard` test and no on-demand initialiser on the three trips
    // an ask makes (take, release, deliver). A std::map member made the 3.3.0 table dynamically initialised and
    // MSVC guarded every access (read on the /FAs listings, Huly QB-1114). The directories below are raw
    // pointers allocated on the cold path for the same reason; the destructor frees them.
    constexpr ask_table() noexcept          = default;
    ask_table(const ask_table &)            = delete;
    ask_table &operator=(const ask_table &) = delete;
    ~ask_table();

    /// Take a free entry for an ask owned by `owner`, bound to the owner's
    /// `slot` (which may be null: such an entry resolves nothing); returns its correlation id.
    [[nodiscard]] std::uint64_t
    take(ActorId const owner, ask_slot *slot) {
        assert(!slot || slot->owner == owner);
        // The warm path is the owner's FIFO on the current core: one core compare, the band test, a page load,
        // a pop -- resolved in ONE inline step (owner_queue); another core, a grown directory or a page not yet
        // created are the out-of-line calls on its cold branches, each run once. A first cut of this registry
        // split the resolution in two (a lookup returning null, then an out-of-line `admit`): on Apple clang 21
        // / arm64 that shape cost +12-14 % per ask on the Savina bank-transaction cell at one core, with the
        // same instruction count, the PMU samples landing on the asking coroutine's return site (Huly QB-1154).
        free_queue   &queue = owner_queue(owner);
        std::uint32_t i     = queue.head;
        if (i == none) [[unlikely]] {
            i = take_unassigned();
        } else {
            queue.head = _tab[i].next;
            if (queue.head == none)
                queue.tail = none;
        }
        entry &e = _tab[i];
        if (slot) {
            e.data.slot = slot;
            e.next      = busy;
        } else {
            // The codec tests and detail callers may take an entry without an awaiter.
            e.data.owner = owner.raw64();
            e.next       = unbound;
        }
        return (static_cast<std::uint64_t>(owner.index()) << 48) | (static_cast<std::uint64_t>(e.gen) << slot_bits) | i;
    }

    [[nodiscard]] ask_slot *
    find(std::uint64_t const id) const noexcept {
        const auto i = id & slot_mask;
        if (i >= _assigned)
            return nullptr;
        const entry &e = _tab[i];
        if (e.next != busy || e.gen != static_cast<std::uint32_t>((id >> slot_bits) & gen_max))
            return nullptr;
        return e.data.slot;
    }

    /// Cold miss classification: an issued generation of an entry this exact incarnation holds or last released.
    [[nodiscard]] QB_NOINLINE bool
    obsolete(std::uint64_t const id, ActorId const owner) const noexcept {
        const auto i          = id & slot_mask;
        const auto generation = static_cast<std::uint32_t>((id >> slot_bits) & gen_max);
        if (generation == 0 || i >= _assigned)
            return false;
        const entry &e = _tab[i];
        if (entry_owner(e) != owner)
            return false;
        // The free entry's current generation has not been issued yet. An exhausted
        // entry's final generation HAS been issued, and must remain recognisable.
        return generation < e.gen || (generation == e.gen && e.next == exhausted);
    }

    /// Give the entry back. Idempotent: a released (or never taken) id is a no-op.
    void
    release(std::uint64_t const id) noexcept {
        entry *const e = live(id);
        if (!e)
            return;
        const ActorId owner = entry_owner(*e);
        if ((id >> 48) != owner.index())
            return;
        // Capture before ending the pointer member's lifetime; the awaiter may be
        // destroyed as soon as unregister returns. No retired raw pointer survives.
        e->data.owner = owner.raw64();
        if (e->gen == gen_max) [[unlikely]] {
            e->next = exhausted;
            return;
        }
        ++e->gen;
        const auto i = static_cast<std::uint32_t>(id & slot_mask);
        if (_current_core != owner.index()) [[unlikely]] {
            release_other_core(owner, i);
            return;
        }
        push_free(owner_queue(owner), i);
    }

private:
    static constexpr std::uint32_t none       = ~std::uint32_t{0};
    static constexpr std::uint32_t busy       = none - 1;
    static constexpr std::uint32_t unbound    = none - 2;
    static constexpr std::uint32_t exhausted  = none - 3;
    static constexpr std::uint32_t unassigned = none - 4;

    struct entry {
        union payload {
            ask_slot     *slot;
            std::uint64_t owner; ///< `ActorId::raw64()` of the incarnation that last held the entry
            constexpr payload()
                : owner(0) {}
        } data;
        std::uint32_t gen  = 1;
        std::uint32_t next = unassigned;
    };
    static_assert(std::is_trivially_copyable_v<entry>);
    static_assert(sizeof(entry) == sizeof(entry::payload) + 2 * sizeof(std::uint32_t));

    struct free_queue {
        std::uint32_t head = none;
        std::uint32_t tail = none;
    };
    using queue_page = std::array<free_queue, 256>;
    // One FIFO per actor slot of a core, in pages of 256 created on first use: ordinary slots (24 bits) in a
    // directory that grows with the highest slot that asked, service tags (16 bits) in a fixed one -- the
    // service band's encoded slots sit at the top of the 24-bit range and would otherwise size the first.
    // A page never moves once created (the directory array may), so a queue address stays valid for the thread.
    struct owner_directory {
        queue_page  **ordinary       = nullptr;
        std::uint32_t ordinary_pages = 0; ///< the length of `ordinary`
        queue_page   *services[256]  = {};
    };
    /// A core this thread has served, with its directory: a worker serves one, the codec tests several.
    struct core_directory {
        std::uint16_t    core;
        owner_directory *dir;
    };

    [[nodiscard]] static ActorId
    entry_owner(entry const &e) noexcept {
        return e.next == busy ? e.data.slot->owner : ActorId::from_raw(e.data.owner);
    }

    [[nodiscard]] static std::size_t
    slot_of(ActorId const owner) noexcept {
        return owner.is_service() ? owner.service_tag() : owner.slot();
    }

    /// The FIFO of `owner`'s slot on its core, resolved inline: the core's directory (selected on its first ask),
    /// the slot's page (the ordinary directory grown when the slot is past its end, the page created on first
    /// use) and the FIFO inside it. The three cold branches call out of line and run once per core or page.
    // `QB_MSVC_FORCEINLINE`: MSVC left the lookup of the first cut out of line in `take` (a call on every ask,
    // read on the listing) while inlining it in `release`; forced, both callers carry it inline. `inline`
    // elsewhere -- and ONE function, not a null-returning lookup plus an out-of-line admission: the split shape
    // cost +12-14 % per ask at one core on Apple clang / arm64 (Huly QB-1154, the A/B in the ticket).
    [[nodiscard]] QB_MSVC_FORCEINLINE free_queue &
    owner_queue(ActorId const owner) {
        if (_current_core != owner.index()) [[unlikely]]
            select_core(owner.index());
        owner_directory  &dir  = *_current_owners;
        const std::size_t slot = slot_of(owner);
        const std::size_t page = slot >> 8;
        queue_page      **where;
        if (owner.is_service()) [[unlikely]] {
            where = &dir.services[page];
        } else {
            if (page >= dir.ordinary_pages) [[unlikely]]
                grow_directory(dir, page + 1);
            where = &dir.ordinary[page];
        }
        if (!*where) [[unlikely]]
            create_page(where);
        return (**where)[slot & 255];
    }

    QB_NOINLINE static void
    create_page(queue_page **const where) {
        *where = new queue_page();
    }

    QB_NOINLINE void
    select_core(std::uint16_t const core) {
        for (std::uint32_t k = 0; k < _ncores; ++k) {
            if (_cores[k].core == core) {
                _current_owners = _cores[k].dir;
                _current_core   = core;
                return;
            }
        }
        auto *const t = static_cast<core_directory *>(std::realloc(_cores, (_ncores + 1) * sizeof(core_directory)));
        if (!t)
            std::abort();
        _cores          = t;
        _cores[_ncores] = core_directory{core, new owner_directory()};
        _current_owners = _cores[_ncores].dir;
        _current_core   = core;
        ++_ncores;
    }

    QB_NOINLINE static void
    grow_directory(owner_directory &dir, std::size_t const need) {
        std::size_t count = dir.ordinary_pages ? dir.ordinary_pages : 4;
        while (count < need)
            count *= 2;
        auto *const t = static_cast<queue_page **>(std::realloc(dir.ordinary, count * sizeof(queue_page *)));
        if (!t)
            std::abort();
        for (std::size_t k = dir.ordinary_pages; k < count; ++k)
            t[k] = nullptr;
        dir.ordinary       = t;
        dir.ordinary_pages = static_cast<std::uint32_t>(count);
    }

    /// A slot's FIFO is empty: the next never-assigned entry (the table doubles when it runs out).
    QB_NOINLINE std::uint32_t
                take_unassigned() {
        if (_assigned == _cap)
            grow();
        return _assigned++;
    }

    /// A release on another thread's core (a codec test, successive engines): the entry's core has a directory
    /// and its slot a page, both created when the entry was admitted.
    QB_NOINLINE void
    release_other_core(ActorId const owner, std::uint32_t const i) noexcept {
        owner_directory *dir = nullptr;
        for (std::uint32_t k = 0; k < _ncores; ++k) {
            if (_cores[k].core == owner.index()) {
                dir = _cores[k].dir;
                break;
            }
        }
        assert(dir);
        const std::size_t slot = slot_of(owner);
        queue_page *const page = owner.is_service() ? dir->services[slot >> 8] : dir->ordinary[slot >> 8];
        assert(page);
        push_free((*page)[slot & 255], i);
    }

    [[nodiscard]] entry *
    live(std::uint64_t const id) const noexcept {
        const std::uint64_t i = id & slot_mask;
        if (i >= _assigned)
            return nullptr;
        entry *const e = _tab + i;
        if ((e->next != busy && e->next != unbound) || e->gen != static_cast<std::uint32_t>((id >> slot_bits) & gen_max))
            return nullptr;
        return e;
    }

    void
    push_free(free_queue &queue, std::uint32_t const i) noexcept {
        _tab[i].next = none;
        if (queue.tail == none)
            queue.head = i;
        else
            _tab[queue.tail].next = i;
        queue.tail = i;
    }

    void
    grow() {
        // The token keeps its existing 22-bit slot domain. Permanent ownership
        // and exhausted generations count toward this retained-capacity bound.
        if (_cap > slot_mask)
            std::abort();
        const std::uint32_t old_cap = _cap;
        _cap                        = _cap ? _cap * 2 : 64;
        auto *const t               = static_cast<entry *>(std::realloc(_tab, _cap * sizeof(entry)));
        if (!t)
            std::abort();
        _tab = t;
        for (std::uint32_t i = old_cap; i < _cap; ++i) {
            _tab[i] = entry{};
        }
    }

    entry           *_tab            = nullptr;
    std::uint32_t    _cap            = 0;
    std::uint32_t    _assigned       = 0;
    core_directory  *_cores          = nullptr;
    std::uint32_t    _ncores         = 0;
    owner_directory *_current_owners = nullptr;
    std::uint32_t    _current_core   = none;
};

ask_table::~ask_table() {
    for (std::uint32_t k = 0; k < _ncores; ++k) {
        owner_directory *const dir = _cores[k].dir;
        for (std::uint32_t p = 0; p < dir->ordinary_pages; ++p)
            delete dir->ordinary[p];
        std::free(dir->ordinary);
        for (queue_page *page : dir->services)
            delete page;
        delete dir;
    }
    std::free(_cores);
    std::free(_tab);
}

constinit thread_local ask_table tls_ask_slots;
// Set of event type-ids known to derive from AskEvent (i.e. carry `correlation_id`),
// populated lazily by `qb::ask<E>`. Lets the activation gate recognise an ask reply
// without RTTI and read `correlation_id` at the AskEvent base offset safely.
thread_local std::unordered_set<Event::id_type> tls_ask_types;

/**
 * @brief The clocks behind a core's `deadline_list` (see `request_deadline`).
 * @details Two clocks. The PRECISE one is `qb::mono_now()` -- the vDSO `clock_gettime` on Linux,
 *          `QueryPerformanceCounter` on Windows, `mach_absolute_time` on macOS: the TSC, ~15-20 ns
 *          a read -- and it is what a deadline is set and fired against, so a timeout keeps the
 *          sub-microsecond precision the libev timer had. The COARSE one is the scheduler tick --
 *          `CLOCK_MONOTONIC_COARSE`, `GetTickCount64`, `CLOCK_MONOTONIC_RAW_APPROX`: a memory read
 *          of the value the kernel updates on each timer interrupt, ~3-5 ns -- and its ONLY use is
 *          the pre-check a busy pass makes: it lags the precise clock by less than one tick and
 *          both advance at one rate, so `precise_now < ref_precise + (coarse_now - ref_coarse) +
 *          tick`, and a pass whose bound falls before the earliest deadline reads nothing else.
 *          Only differences of the coarse clock are ever taken (on Windows it does not even share
 *          the precise clock's origin). The tick bound is deliberately loose -- 20 ms on Linux and
 *          Windows (a 100 Hz jiffy, a 15.625 ms timer period), 50 ms on macOS, whose approximate
 *          clock states no bound -- it only decides how long before a deadline the precise reads
 *          begin, never whether a deadline is judged precisely. A platform with no coarse clock
 *          reads the precise one on every pass, which is what the libev timer cost anyway.
 */
[[nodiscard]] inline std::uint64_t
mono_ns() noexcept {
    return static_cast<std::uint64_t>(qb::mono_now().time_since_epoch().count());
}

#if defined(__linux__)
constexpr std::uint64_t coarse_tick_ns = 20'000'000; // CONFIG_HZ >= 100: a jiffy is at most 10 ms
[[nodiscard]] inline std::uint64_t
coarse_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
#elif defined(_WIN32)
constexpr std::uint64_t coarse_tick_ns = 20'000'000; // the timer period is at most 15.625 ms
[[nodiscard]] inline std::uint64_t
coarse_ns() noexcept {
    return static_cast<std::uint64_t>(GetTickCount64()) * 1'000'000ull;
}
#elif defined(__APPLE__)
constexpr std::uint64_t coarse_tick_ns = 50'000'000; // "approximate": no stated bound, tens of ms at worst
[[nodiscard]] inline std::uint64_t
coarse_ns() noexcept {
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX);
}
#else
constexpr std::uint64_t coarse_tick_ns = 0; // no coarse clock: the pre-check is the precise read
[[nodiscard]] inline std::uint64_t
coarse_ns() noexcept {
    return mono_ns();
}
#endif

inline void
deadline_unlink(deadline_list &l, request_deadline &d) noexcept {
    if (d.prev)
        d.prev->next = d.next;
    else
        l.head = d.next;
    if (d.next)
        d.next->prev = d.prev;
    else
        l.tail = d.prev;
    d.prev = d.next = nullptr;
}
} // namespace

void
deadline_arm(request_deadline &d, qb::duration const timeout, void (*fire)(void *) noexcept, void *ctx) noexcept {
    // The calling thread's core: an ask, a stream, a discovery can only be armed from an actor
    // context, i.e. on a live core thread.
    auto &l = VirtualCore::_handler->_deadlines;
    if (d.armed())
        deadline_unlink(l, d);
    // One precise read: the deadline is absolute in the precise clock. A non-positive timeout is
    // one nanosecond, so that a deadline armed from a firing one (`at` >= this pass's reading)
    // fires on the NEXT pass rather than inside the check that is running.
    const std::uint64_t now = mono_ns();
    d.at                    = now + (timeout.count() > 0 ? static_cast<std::uint64_t>(timeout.count()) : 1u);
    d.fire                  = fire;
    d.ctx                   = ctx;
    // Sorted insert from the tail: equal timeouts armed in order -- the common case -- land
    // after the tail in O(1); a shorter one walks back past the longer ones ahead of it.
    request_deadline *after = l.tail;
    while (after && after->at > d.at)
        after = after->prev;
    d.prev = after;
    d.next = after ? after->next : l.head;
    if (d.next)
        d.next->prev = &d;
    else
        l.tail = &d;
    if (after)
        after->next = &d;
    else
        l.head = &d;
}

void
deadline_disarm_armed(request_deadline &d) noexcept {
    // A frame destroyed after its core thread has withdrawn (`_handler` null: the listener's
    // teardown runs at thread exit) has no list to leave; clearing the node is the whole job.
    if (VirtualCore *const core = VirtualCore::_handler; core)
        deadline_unlink(core->_deadlines, d);
    d.prev = d.next = nullptr;
    d.fire          = nullptr;
    d.ctx           = nullptr;
}

bool
deadlines_armed() noexcept {
    const VirtualCore *const core = VirtualCore::_handler;
    return core && core->_deadlines.armed();
}

std::uint64_t
deadlines_next_in(const deadline_list &l, std::uint64_t const now_ns) noexcept {
    const request_deadline *const h = l.head;
    if (!h)
        return ~std::uint64_t{0};
    return h->at > now_ns ? h->at - now_ns : 0u;
}

void
deadlines_check(deadline_list &l, std::uint64_t now_ns) noexcept {
    if (!l.head)
        return;
    if (now_ns == 0) {
        const std::uint64_t coarse = coarse_ns();
        if (l.have_ref) {
            // The latest the precise clock can read now (see deadline_list): if even that is
            // before the earliest deadline, nothing can have expired -- no precise read.
            const std::uint64_t bound = l.ref_precise_ns + (coarse - l.ref_coarse_ns) + coarse_tick_ns;
            if (bound < l.head->at)
                return;
        }
        now_ns           = mono_ns();
        l.ref_precise_ns = now_ns;
        l.ref_coarse_ns  = coarse;
        l.have_ref       = true;
    }
    // Fire in deadline order. The node is unlinked and disarmed BEFORE its callback runs, so the
    // callback may arm or disarm anything, itself included; the head is re-read after each.
    while (l.head && l.head->at <= now_ns) {
        request_deadline &d = *l.head;
        deadline_unlink(l, d);
        auto *const fire = d.fire;
        void *const ctx  = d.ctx;
        d.fire           = nullptr;
        d.ctx            = nullptr;
        fire(ctx);
    }
}

std::uint64_t
ask_take(qb::ActorId const owner, ask_slot *slot) noexcept {
    // The id names the OWNER actor's core index in its high 16 bits, so correlation
    // ids are globally unique across VirtualCores: two cores' independent registries
    // would otherwise hand out identical (generation, slot) pairs, and `ask_deliver`
    // matches on (id, owner) only — a cross-core request carrying a colliding id
    // would resolve the receiver's OWN pending slot and hand its `ask_awaiter<E>` an
    // event of the wrong type (type confusion). `ask_deliver` refuses an id whose
    // core is not the delivering actor's before it even indexes the table. The low
    // 48 bits locate the entry (see ask_table); 0 stays reserved for "not an ask".
    // The entry is taken AND bound here, before the request is sent — its owner (the
    // awaiter, the discovery awaiter, the `stream`) already exists, and that owner's
    // destructor is what gives the entry back, through `ask_unregister`, on every path.
    return tls_ask_slots.take(owner, slot);
}

void
ask_unregister(std::uint64_t const id) noexcept {
    tls_ask_slots.release(id);
}

bool
ask_deliver(std::uint64_t const id, ActorId const owner, Event &resp) noexcept {
    // The core in the id is the table the entry lives in; anything else is a miss by
    // construction (as is id 0, "not an ask", whose entry can never be taken).
    if ((id >> 48) != owner.index())
        return false;
    ask_slot *slot = tls_ask_slots.find(id);
    // Only the owning actor may resolve its slot, and only once.
    if (!slot) [[unlikely]]
        return tls_ask_slots.obsolete(id, owner);
    if (!(slot->owner == owner))
        return false;
    if (slot->done)
        return true; // timeout/cancel can finish before the awaiter unregisters.
    // A null `deliver` is a self-ask's (Huly QB-1100): the same test as before, a cold target.
    if (!slot->deliver) [[unlikely]]
        return ask_self_arrival(id, *slot, resp);
    slot->deliver(slot->self, resp);
    return true;
}

namespace {
/**
 * @brief A self-ask in flight (Huly QB-1100): the delivery `ask_self_begin` parked, and where its request was
 *        handed to the handler.
 * @details The request and the answer of a self-ask carry the same correlation id to the same actor, and the
 *          answer may be a byte copy of the request, so no field of the event can tell them apart. The ORDER can:
 *          the request is the first event with the id to reach its owner (nothing else carries a fresh id), and
 *          the answer arrives in a later dispatch. While the request's handler runs, it may check it more than
 *          once (`qb::answer` resolves before answering, after a handler that already did): those calls see the
 *          same event in the same dispatch -- the core's received-event count, which the pass bumps after each
 *          handler, and the event's address, which no other live event shares.
 */
struct self_ask_record {
    std::uint64_t id;
    void (*deliver)(void *self, Event &resp) noexcept;
    std::uint64_t request_dispatch = 0;       ///< the core's received count while the request's handler ran
    const Event  *request          = nullptr; ///< the request, once it arrived
};
// A handful at most (one per self-ask in flight); cold: only a self-ask reaches it.
thread_local std::vector<self_ask_record> tls_self_asks;
} // namespace

bool
ask_self_begin(std::uint64_t const id, ask_slot &slot) noexcept {
    const VirtualCore *const core = VirtualCore::_handler;
    if (core && core->__in_init__(slot.owner))
        return false;
    // Drop the records of self-asks that ended without an answer (a timeout, a cancel): their entry is gone.
    std::erase_if(tls_self_asks, [](self_ask_record const &r) { return tls_ask_slots.find(r.id) == nullptr; });
    tls_self_asks.push_back(self_ask_record{id, slot.deliver});
    slot.deliver = nullptr;
    return true;
}

bool
ask_self_arrival(std::uint64_t const id, ask_slot &slot, Event &resp) noexcept {
    const auto it = std::find_if(tls_self_asks.begin(), tls_self_asks.end(), [id](self_ask_record const &r) { return r.id == id; });
    if (it == tls_self_asks.end())
        return false; // not a self-ask's slot: nothing bound to deliver to
    const VirtualCore *const core     = VirtualCore::_handler;
    const std::uint64_t      dispatch = core ? core->_metrics._nb_event_received : 0;
    if (it->request == nullptr) { // the first arrival is the request: its handler answers it
        it->request          = &resp;
        it->request_dispatch = dispatch;
        return false;
    }
    if (it->request == &resp && it->request_dispatch == dispatch)
        return false; // the same request, checked again while its handler runs
    // The answer, whatever its form. The slot goes back to the plain path first (a stream's next chunks take
    // it), and nothing of the slot or the record is touched after `deliver`, which may end the ask.
    slot.deliver = it->deliver;
    tls_self_asks.erase(it);
    slot.deliver(slot.self, resp);
    return true;
}

ev::loop_ref
ask_loop() noexcept {
    return qb::io::async::listener::current.loop();
}

void
ask_register_type(Event::id_type const type) noexcept {
    tls_ask_types.insert(type);
}

bool
ask_try_deliver_reply(Event &ev, ActorId const dest) noexcept {
    // Only correlated-reply types (registered by ask / ask_stream / require) carry a
    // `correlation_id`.
    if (tls_ask_types.find(ev.getID()) == tls_ask_types.end())
        return false;
    // The type-id match proves `ev` derives from CorrelatedEvent (it is the first base), so
    // `correlation_id` lives at the CorrelatedEvent base subobject offset.
    auto &ce = static_cast<CorrelatedEvent &>(ev);
    return ask_deliver(ce.correlation_id, dest, ev);
}
} // namespace detail

Actor::Actor() noexcept
    : _id((assert(VirtualCore::_handler != nullptr
                  && "Actor must be constructed from within a VirtualCore worker thread "
                     "(use Main::core(idx).addActor<T>(...) or addRefActor<T>()), never "
                     "from the main thread or an arbitrary user thread."),
           VirtualCore::_handler->__generate_id__())) {
    VirtualCore::__initialize_default_events__(*this);
}

Actor::Actor(ActorId const id) noexcept
    : _id(id) {
    assert(VirtualCore::_handler != nullptr
           && "Service actors must be constructed from within their owning VirtualCore "
              "worker thread.");
    VirtualCore::__initialize_default_events__(*this);
}

Actor::Actor(no_default_events_t) noexcept
    : _id((assert(VirtualCore::_handler != nullptr && "Actor must be constructed from within a VirtualCore worker thread."),
           VirtualCore::_handler->__generate_id__())) {
    // Intentionally no default event registrations — derived class owns its wiring. The minimum for
    // shutdown is SignalEvent, NOT KillEvent — see `qb::no_default_events_t` in Actor.h for why.
}

void
Actor::on(PingEvent const &event) noexcept {
    // type 0 is the wildcard liveness probe (any live actor replies — qb::ping); otherwise the
    // ping is a typed discovery (qb::require / legacy require<>()). Echo the correlation id so the
    // coroutine helpers can match the reply.
    if (event.type == 0 || event.type == id_type)
        send<RequireEvent>(event.source, event.type, event.correlation_id);
}

void
Actor::on(RequireEvent &event) noexcept {
    // Default: deliver the reply to a pending co_await qb::ping / qb::require. A correlation_id of 0
    // (legacy fire-and-forget require<>()) resolves nothing here — override on(RequireEvent&) to use
    // the legacy is<T>() dance.
    (void) resolve_require(event);
}

bool
Actor::resolve_require(RequireEvent &e) const noexcept {
    return qb::detail::ask_deliver(e.correlation_id, id(), e);
}

void
Actor::on(KillEvent const &) noexcept {
    kill();
}

void
Actor::on(SignalEvent const &event) noexcept {
    // Terminal signals only. SIGINT and SIGTERM both mean "stop": SIGTERM is what Docker,
    // Kubernetes and systemd send first, so swallowing it made a registered SIGTERM a no-op —
    // the process stayed alive and the supervisor had to escalate to SIGKILL, losing the very
    // graceful teardown `registerSignal`'s contract promises ("Registered signals will trigger
    // a graceful shutdown of all actors", core/Main.h).
    //
    // Everything else stays NON-terminal on purpose: SIGHUP / SIGUSR1 are the documented
    // "register your own signal" cases (config reload, stats dump). An actor that wants to act
    // on them overrides `on(SignalEvent&)`; killing every actor on a reload signal would be a
    // far worse regression than the bug this closes.
    // Pinned by `SignalShutdown.*` in system/engine/sigterm-shutdown.cpp.
    if (event.signum == SIGINT || event.signum == SIGTERM)
        kill();
}

void
Actor::on(UnregisterCallbackEvent const &) noexcept {
    VirtualCore::_handler->__unregisterCallback(id());
}

uint64_t
Actor::time() const noexcept {
    return VirtualCore::_handler->time();
}

qb::wall_time
Actor::now() const noexcept {
    return qb::wall_from_unix_nanos(static_cast<std::int64_t>(time()));
}

// is_alive() is defined in-class: the router's dispatch trampoline calls it on every event
// from the user's TU, where an out-of-line definition is a call into the archive.

bool
Actor::is_active() const noexcept {
    return _alive && _activated;
}

CoreId
Actor::getIndex() const noexcept {
    return VirtualCore::_handler->getIndex();
}

std::string_view
Actor::getName() const noexcept {
    return name;
}

const CoreIdSet &
Actor::getCoreSet() const noexcept {
    return VirtualCore::_handler->getCoreSet();
}

void
Actor::unregisterCallback() const noexcept {
    VirtualCore::_handler->unregisterCallback(id());
}

void
Actor::__resolve_coro_scheduler__() const noexcept {
    // Finding 2.D.4: revalidate the cached scheduler pointer on every spawn by
    // comparing it against the current TLS scheduler. Caching alone is unsafe because
    // the listener-owned scheduler can be torn down and rebuilt (tests calling
    // `listener::reset_coro_scheduler()`, or a core destroyed and re-initialized).
    // Revalidation is essentially free: `current_ptr()` is a plain `thread_local*`
    // load and the comparison fits in one cmp+jne.
    auto *expected = qb::io::async::CoroutineScheduler::current_ptr();
    if (likely(expected != nullptr)) {
        if (unlikely(coro_scheduler_ != expected))
            coro_scheduler_ = expected;
    } else if (unlikely(!coro_scheduler_)) {
        // Extremely rare: spawn called before any TLS scheduler exists on this thread.
        coro_scheduler_ = &qb::io::async::listener::current.coro_scheduler();
    }
    // Finding 2.D.5: debug-only guard against cross-thread spawn. The actor system is
    // strictly mono-thread per VirtualCore; spawning from another thread is UB. We
    // cannot tell which VirtualCore owns this actor here, but we CAN verify a TLS
    // scheduler exists on the caller thread, excluding unrelated std::thread contexts.
    assert(qb::io::async::CoroutineScheduler::current_ptr() != nullptr
           && "Actor::spawn_detached/spawn called from a thread without a coroutine "
              "scheduler — are you calling this from outside the VirtualCore?");
}

void
Actor::__ensure_coro_scope__() const {
    if (!_coro_scope)
        _coro_scope = qb::io::async::cancellation_token{}; // allocate the real token once.
}

void
Actor::__cancel_coro_scope__() const noexcept {
    if (_coro_scope)
        _coro_scope.cancel();
}

bool
Actor::is_actor_alive(ActorId const id) const noexcept {
    return VirtualCore::_handler->isActorAlive(id);
}

void
Actor::kill() const noexcept {
    // Idempotent: the second kill() of one actor has nothing left to do, and returning here
    // is what lets the core keep its kill queue as a plain vector -- one entry per actor,
    // no set to deduplicate against. `_alive` has no other writer.
    if (!_alive)
        return;
    _alive = false;
    // Cancel-on-kill: wake any scoped coroutine awaiting a cancellation-aware op so it
    // unwinds promptly instead of blocking on a long timeout/I/O. Idempotent + no-op if
    // no scoped coroutine was ever spawned.
    __cancel_coro_scope__();
    VirtualCore::_handler->killActor(id());
}

namespace {

QB_NOINLINE void
warn_broadcast_reply(Actor const &actor) noexcept {
    QB_LOG_WARN(actor << " failed to reply broadcast event");
}

QB_NOINLINE void
warn_broadcast_forward(Actor const &actor) noexcept {
    QB_LOG_WARN(actor << " failed to forward broadcast event");
}

} // namespace

void
Actor::reply(Event &event) const noexcept {
    if (unlikely(event.dest.is_broadcast())) {
        warn_broadcast_reply(*this);
        return;
    }
    VirtualCore::_handler->reply(event);
}

void
Actor::forward(ActorId const dest, Event &event) const noexcept {
    // Do not overwrite event.source: reply() routes via swap(dest, source); the
    // original sender must remain the logical client (matches Actor.h contract).
    if (unlikely(event.dest.is_broadcast())) {
        warn_broadcast_forward(*this);
        return;
    }
    VirtualCore::_handler->forward(dest, event);
}

// The push-family answers (Huly QB-976): the same guard as reply()/forward(), the pipe instead of the ring.
void
Actor::push_reply(Event &event) const noexcept {
    if (unlikely(event.dest.is_broadcast())) {
        QB_LOG_WARN(*this << " failed to reply broadcast event");
        return;
    }
    VirtualCore::_handler->push_reply(event);
}

void
Actor::push_forward(ActorId const dest, Event &event) const noexcept {
    if (unlikely(event.dest.is_broadcast())) {
        QB_LOG_WARN(*this << " failed to forward broadcast event");
        return;
    }
    VirtualCore::_handler->push_forward(dest, event);
}

// OpenApi : internal future use
void
Actor::send(Event const &event) const noexcept {
    VirtualCore::_handler->send(event);
}

void
Actor::push(Event const &event) const noexcept {
    VirtualCore::_handler->push(event);
}

bool
Actor::try_send(Event const &event) const noexcept {
    VirtualCore *const core = VirtualCore::_handler;
    if (!core->try_send(event))
        return false;
    ++core->_metrics._nb_event_sent; // a mailbox publish no flush will count (Huly QB-162)
    core->_metrics._nb_bucket_sent += event.bucket_size;
    return true;
}

uint64_t
CoroContext::time() const noexcept {
    return VirtualCore::_handler->time();
}

Service::Service(ServiceId const sid) noexcept
    : Actor(VirtualCore::_handler->__generate_service_id__(sid)) {}

namespace detail {

// Declared in VirtualCore.h next to the two spawn wrappers that call it; see the note there for
// why a spawned coroutine's exception has nowhere to go and was being dropped in silence.
//
// TWO CHANNELS. `QB_LOG_CRIT` is the structured one; it is a complete no-op unless the build
// defines QB_WITH_LOGGING or QB_STDOUT_LOGGING. With QB_WITH_LOGGING it writes to nanolog,
// which LogInitializer opens before main(); the stdout-only fallback writes to qb::io::cout().
// The independent stderr report therefore remains available when logging is disabled or its
// file could not be opened. `qb::io::cerr()` is qb's always-compiled, mutex-guarded stderr
// wrapper. Main's failed-core-init diagnostic also goes to stderr, through std::cerr rather
// than this wrapper. Nothing here is on a hot path: reaching this function at all means a
// coroutine body threw.
void
report_unhandled_coroutine_exception(ActorId const owner, char const *const api, std::exception_ptr ep) noexcept {
    const auto emit = [&](char const *const what) {
        QB_LOG_CRIT("Actor(" << owner.index() << '.' << owner.sid() << "@" << owner.generation() << ") " << api
                             << "() coroutine body let an exception escape: " << what
                             << " -- the frame unwound and the exception was DISCARDED.");
        qb::io::cerr() << "CRITICAL: qb Actor(" << owner.index() << '.' << owner.sid() << "@" << owner.generation() << ") " << api
                       << "() coroutine body let an exception escape, and it was DISCARDED: " << what
                       << " -- a spawned coroutine has no caller to receive it. Catch it in the body and report through an event." << std::endl;
    };
    // noexcept: this runs inside a catch handler while a coroutine frame unwinds, so it must not
    // itself throw. The outer try covers `emit` -- including a throw out of the inner HANDLER,
    // which the inner catch clauses cannot take.
    try {
        try {
            if (ep)
                std::rethrow_exception(ep);
            emit("<no exception object>");
        } catch (std::exception const &e) {
            // Emit from INSIDE the handler, and never hold `e.what()` past it. `rethrow_exception`
            // is not required to rethrow the same object: libstdc++ and libc++ bump a refcount, but
            // MSVC throws a fresh COPY, and `std::exception`'s copy constructor duplicates the
            // message buffer there (`__std_exception_copy`). So a `char const *` carried out of this
            // block would dangle on Windows only -- the platform with no CI (see verify-windows.ps1).
            emit(e.what());
        } catch (...) {
            emit("<exception not derived from std::exception>");
        }
    } catch (...) {
        // Reporting must never become the failure. There is nothing left to try.
    }
}

// The activation waiters of `ActorHandle::ready_async` (Huly QB-62). Cold, and at the END of the
// file for the same reason `__fire_activation_waiters__` is at the end of VirtualCore.cpp: placed
// among the hot bodies they moved every address after them.
activation_state
activation_wait(qb::ActorId const id, activation_waiter &w, void (*fire)(void *, bool) noexcept, void *ctx) noexcept {
    // The calling thread's core: a handle is resolved on the core that owns the actor, and
    // `ready_async` is awaited from an actor context on that core.
    VirtualCore *const core = VirtualCore::_handler;
    if (!core || !id.is_valid())
        return activation_state::gone;
    Actor *const actor = core->__actor_slot__(id);
    if (actor == nullptr || !actor->is_alive())
        return activation_state::gone; // not on this core, or killed (an Activating one included: it dies)
    if (actor->is_active())
        return activation_state::active;
    const auto it = core->_activating.find(id);
    if (it == core->_activating.end())
        return activation_state::gone; // alive, not active, not Activating: a failed init awaiting its reap
    // Link at the head: order among waiters is immaterial (each is resumed through the scheduler).
    auto &head = it->second.waiters;
    w.prev     = nullptr;
    w.next     = head;
    w.fire     = fire;
    w.ctx      = ctx;
    if (head)
        head->prev = &w;
    head = &w;
    return activation_state::activating;
}

void
activation_unwait(qb::ActorId const id, activation_waiter &w) noexcept {
    if (!w.linked())
        return; // fired, detached, or never linked: the common case on every exit path
    // Still linked, so the entry still exists on this core (an entry never goes away with a
    // linked waiter: it fires or detaches them first). A core that is gone has no entry.
    if (VirtualCore *const core = VirtualCore::_handler; core) {
        if (const auto it = core->_activating.find(id); it != core->_activating.end()) {
            if (w.prev)
                w.prev->next = w.next;
            else
                it->second.waiters = w.next;
            if (w.next)
                w.next->prev = w.prev;
        }
    }
    w.prev = w.next = nullptr;
    w.fire          = nullptr;
    w.ctx           = nullptr;
}

} // namespace detail

// Cold (an actor asks for it), so at the end of the file with the other bodies the event path never calls.
CoreStats
Actor::getCoreStats() const noexcept {
    return VirtualCore::_handler->getCoreStats();
}

// Death watch (Huly QB-51): cold, called when an actor decides to watch, at the end for the same reason.
void
Actor::watch(ActorId const target) const noexcept {
    VirtualCore::_handler->__watch__(id(), target);
}

void
Actor::unwatch(ActorId const target) const noexcept {
    VirtualCore::_handler->__unwatch__(id(), target);
}
} // namespace qb

#ifdef QB_WITH_LOGGING
qb::io::log::stream &
qb::operator<<(qb::io::log::stream &os, qb::Actor const &actor) {
    os << "Actor[" << actor.getName() << "](" << actor.id().index() << "." << actor.id().sid() << "@" << actor.id().generation() << ")";
    return os;
}
#endif

std::ostream &
qb::operator<<(std::ostream &os, qb::Actor const &actor) {
    os << "Actor[" << actor.getName() << "](" << actor.id().index() << "." << actor.id().sid() << "@" << actor.id().generation() << ")";
    return os;
}
