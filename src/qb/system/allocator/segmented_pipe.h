/**
 * @file qb/system/allocator/segmented_pipe.h
 * @brief Segmented FIFO buffer that grows by appending a segment and never moves what it holds
 *
 * This file defines `segment_pool` and `segmented_pipe`, the storage under every event pipe of a
 * `VirtualCore` (`qb::VirtualPipe`). It replaces `allocator::pipe<EventBucket>` there, and
 * ONLY there: `pipe<char>` stays the byte buffer of the qb-io streams, whose consumers want one
 * contiguous range and pay for it knowingly.
 *
 * Why a second buffer type. `pipe<T>` is one contiguous block that doubles on growth: a fresh
 * allocation plus a memcpy of everything live, and `reorder()` memmoves the live range down when
 * the freed prefix is large enough. For a byte stream that is the right trade. For an event queue
 * it is the wrong one three times over — measured on savina/counting at one core with a 1 M-event
 * burst, the run copied 64 MB of events it never needed to move, first-touched twice that in
 * transient doublings, and turned every reference `Actor::push()` had handed out into a dangling
 * one at the next push. A segmented pipe removes all three: growth links one more segment (no
 * copy, no move, no memcpy), a segment that has been consumed goes back to a per-owner pool the
 * next burst draws from (so the steady state of a burst regime is cache-resident rather than a
 * fresh 64 MB every pass), and an element stays at the address it was allocated at until the
 * segment holding it is popped — which is what makes the reference `push<>` returns STABLE.
 *
 * Shape:
 *  - a segment is `SegmentItems` items (256 KB of `EventBucket` by default — the same
 *    4096-bucket step `pipe<T>` starts from, and sized so that the segment being read and the
 *    segment being written both sit in a core's L2 at once, which is what makes a sustained
 *    burst cache-resident once its segments are pooled) with a header living in its first item
 *    slot(s), carved eight to a 2 MB slab that the pool draws from the process-wide
 *    `slab_cache` (`slab.h`: mapped by the platform, huge-page-backed and prefaulted where the
 *    kernel can, and kept warm across pools and engines);
 *  - an allocation never straddles two segments: when the tail cannot hold it the remainder of
 *    the tail is skipped and a new segment is linked, so every allocated range is contiguous and
 *    a walk visits whole ranges;
 *  - a request wider than a segment gets a dedicated, exactly-sized segment (an `allocated_push`
 *    of a jumbo event), returned to the allocator rather than the pool when popped;
 *  - the read side is a cursor over the head segment: `front()` is the live range of the head,
 *    `consume_front(n)` / `pop_front()` advance it, and popped segments go to the pool;
 *  - the pool is owned by whoever owns the pipes (one per `VirtualCore`), is NOT thread-safe, and
 *    retains segments at high-water — the same policy `pipe<T>` had, minus the per-pipe copy;
 *    `shrink()` hands whole idle slabs back to the cache, and the cache alone talks to the OS.
 *
 * Nothing here is thread-safe. A pipe and its pool belong to one thread, the thread that owns the
 * `VirtualCore`; the cross-core hop is the mailbox ring, which copies out of these segments.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - C++ Actor Framework (cpp.actor)
 * @ingroup Container
 */

#ifndef QB_SEGMENTED_PIPE_H
#define QB_SEGMENTED_PIPE_H

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>
#include <qb/system/allocator/pipe.h> // getItemSize
#include <qb/system/allocator/slab.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/nocopy.h>

namespace qb::allocator {

namespace detail {

/**
 * @struct segment_header
 * @brief The bookkeeping of one segment, stored IN the segment's first item slot(s).
 *
 * `next` is the FIFO link while the segment belongs to a pipe and the free-list link while it
 * sits in a pool. `begin` is the first item slot a pipe may use: the slot after the header, plus
 * the segment's STAGGER (see `segment_pool`), so `capacity` counts the items from `begin` to the
 * segment's end. `end` is the committed live end (relative to `begin`) of a segment that is no
 * longer the tail -- the tail's live end is the pipe's write cursor -- and is undefined for the
 * tail and for a pooled segment. `standard` tells a pooled segment from a dedicated one.
 *
 * `capacity` and `end` are 32-bit so the header stays within 32 bytes -- one slot of a 64-byte
 * item, which is what keeps `header_items()` at 1 for `EventBucket`. A dedicated segment wider
 * than that is refused with an assert; nothing in qb can ask for one (`bucket_size` is 16-bit).
 */
template <typename T>
struct segment_header {
    segment_header *next;
    T              *begin;
    std::uint32_t   capacity;
    std::uint32_t   end;
    bool            standard;

    /// Number of `T` slots the header occupies at the front of a segment. A function rather
    /// than a static data member: its initializer needs `sizeof(segment_header)`, and MSVC
    /// evaluates a static member's initializer while the class is still incomplete (C2027),
    /// where a function body is a complete-class context on every compiler.
    [[nodiscard]] static constexpr std::size_t
    header_items() noexcept {
        return getItemSize<segment_header, T>();
    }

    /// The first slot after the header -- where `begin` points when the stagger is zero.
    [[nodiscard]] T *
    items() const noexcept {
        return reinterpret_cast<T *>(const_cast<segment_header *>(this)) + header_items();
    }

    /// Items between the header and `begin`: this segment's stagger.
    [[nodiscard]] std::size_t
    stagger() const noexcept {
        return static_cast<std::size_t>(begin - items());
    }
};

} // namespace detail

/**
 * @class segment_pool
 * @brief Owner-thread free list of standard-sized segments, shared by every `segmented_pipe`
 *        of one owner, carved from process-wide slabs.
 *
 * `acquire(n)` hands out a segment able to hold `n` items: a pooled standard segment when
 * `n <= SegmentItems`, a dedicated exactly-sized one otherwise. `release()` returns a standard
 * segment to the free list (LIFO, so the next acquire gets the warmest one) and gives a dedicated
 * one back to the allocator at once. The pool never shrinks on its own: what a burst allocated
 * stays available for the next burst, which is the point.
 *
 * Standard segments are not allocated one by one. The pool draws 2 MB slabs from
 * `slab_cache` and carves them into `segments_per_slab` segments as growth asks for them, so
 * growing past the high water costs one slab acquisition per eight segments — and a slab comes
 * back from the cache already mapped and, on Linux, already faulted as a huge page, which is
 * the difference between ~1 µs and ~0 per 4 KB on the growth path (see `slab.h`). Dedicated
 * segments are wider than a slab could sensibly pool and go through `std::allocator`.
 *
 * `shrink()` returns to the cache every slab none of whose segments is lent — retained
 * segments are dropped from the free list slab by slab, never one at a time, because the slab
 * is the unit the platform knows. The destructor does the same; every pipe drawing from the
 * pool must be gone by then (`outstanding()` is what a test asserts on, and a pool destroyed
 * with a segment still lent leaks that slab rather than release memory in use).
 *
 * **Segments are STAGGERED.** Carved on a fixed stride out of aligned slabs, every segment's
 * item area would otherwise start at the same offset within a 4 KB page — so item `k` of one
 * pipe and item `k` of another share their low twelve address bits, exactly. That is the
 * pattern an actor's reply produces on every delivery: the received event is byte-copied into
 * the outbound pipe at the same index it was read from, and any load from the received event
 * that follows the copy (the receive loop's own width read, or a handler touching a field after
 * `reply()`) trails a store to an address the core cannot tell apart from it until the store
 * has committed. Intel calls it 4K aliasing; measured on savina/big at one core, that one stall
 * per event DOUBLED the run (52 ms to 107 ms) against the malloc-laid-out pipes it replaced,
 * whose blocks happened to sit at unrelated offsets. So the pool gives the `i`-th segment it
 * carves a stagger of `(i * stagger_stride) % stagger_slots` cache lines: `begin` moves forward
 * by that many lines and `capacity` shrinks by the same, so consecutive segments — which is
 * what the two pipes of a core hold — never share a page offset. The slot count is 64 (a page
 * of 64-byte lines) when a segment can spare it, the largest power of two within a quarter of
 * the segment otherwise, and 1 for a geometry too small to stagger; `segment_capacity` is the
 * capacity every pooled segment is GUARANTEED to have, i.e. after the widest stagger, which is
 * the widest single `allocate_back` that is pooled rather than dedicated.
 *
 * The stride is 27, not 1, and that too is measured. A stagger of ONE line between consecutive
 * segments moves the reply's store off item `k` and onto item `k + 1` — the load the receive
 * loop issues next, for a one-line event, while the store is still in the buffer — and cost
 * savina/big 8% (52 to 56 ms). What a stagger must keep apart is not two items but two
 * STREAMS advancing in step, so the distance has to be large against the event width: 27 is
 * odd (every slot is visited once per 64 carves) and puts adjacent segments 27 lines apart,
 * any two of six consecutively carved ones at least 7 apart, circularly. A two-line event then
 * meets the store 13 events later, an eight-line one 3 events later; a wider event's copy
 * already outweighs the stall it could meet.
 *
 * @tparam T            Item type (`EventBucket` for the event pipes).
 * @tparam SegmentItems Items per standard segment, header included.
 */
template <typename T, std::size_t SegmentItems>
class segment_pool
    : nocopy
    , std::allocator<T> {
    using base_type = std::allocator<T>;
    using header    = detail::segment_header<T>;
    static_assert(SegmentItems > header::header_items(), "a segment must hold at least one item after its header");
    static_assert(alignof(header) <= alignof(T) || alignof(header) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                  "the segment header is placed at the start of a T-aligned allocation");

public:
    /// Items a standard segment holds after its header, before any stagger.
    static constexpr std::size_t raw_capacity = SegmentItems - header::header_items();
    /// Items per cache line (at least one): the unit a stagger is expressed in.
    static constexpr std::size_t line_items = sizeof(T) >= 64 ? 1 : 64 / sizeof(T);
    /// Distinct staggers the pool cycles through: 64 lines (one page) when a segment can spare
    /// them, the largest power of two within a quarter of the segment otherwise, 1 when it
    /// cannot stagger at all. A power of two so any odd stride walks every slot.
    static constexpr std::size_t stagger_slots =
        std::bit_floor(std::min<std::size_t>(64, std::max<std::size_t>(1, raw_capacity / (4 * line_items))));
    /// Slots between the staggers of consecutively carved segments (see the class comment).
    static constexpr std::size_t stagger_stride = 27;
    /// The stagger slot of the `i`-th standard segment a pool carves.
    [[nodiscard]] static constexpr std::size_t
    stagger_slot(std::size_t const i) noexcept {
        return (i * stagger_stride) % stagger_slots;
    }
    /// The widest stagger a standard segment carries, in items.
    static constexpr std::size_t max_stagger = (stagger_slots - 1) * line_items;
    /// Items EVERY standard segment can hold: the widest single `allocate_back` that is pooled.
    static constexpr std::size_t segment_capacity = raw_capacity - max_stagger;
    static_assert(segment_capacity >= 1 && segment_capacity <= UINT32_MAX);
    /// Bytes of one standard segment, header included.
    static constexpr std::size_t segment_bytes = SegmentItems * sizeof(T);
    static_assert(segment_bytes <= slab_cache::slab_bytes, "a standard segment must fit a slab");
    /// Standard segments carved from one slab (the slab's remainder, if any, is never used).
    static constexpr std::size_t segments_per_slab = slab_cache::slab_bytes / segment_bytes;

private:
    header             *_free      = nullptr;  /**< LIFO free list of standard segments */
    std::size_t         _retained  = 0;        /**< segments on the free list */
    std::size_t         _allocated = 0;        /**< segments alive, retained or lent, standard or dedicated */
    std::vector<void *> _slabs;                /**< slabs this pool holds, in acquisition order */
    void               *_carve_slab = nullptr; /**< the slab still being carved, or nullptr */
    std::size_t         _carve_left = 0;       /**< segments not yet carved out of `_carve_slab` */
    std::size_t         _carved     = 0;       /**< standard segments carved so far: the stagger clock */

    [[nodiscard]] static std::uintptr_t
    addr(void const *const p) noexcept {
        return reinterpret_cast<std::uintptr_t>(p);
    }

    [[nodiscard]] static bool
    in_slab(void const *const p, void const *const slab) noexcept {
        return addr(p) >= addr(slab) && addr(p) < addr(slab) + segments_per_slab * segment_bytes;
    }

    // Next standard segment: from the slab being carved, or from a fresh one.
    [[nodiscard]] header *
    carve() {
        if (!_carve_left) {
            _slabs.reserve(_slabs.size() + 1); // so the push_back below cannot throw
            void *const slab = slab_cache::acquire();
            _slabs.push_back(slab);
            _carve_slab = slab;
            _carve_left = segments_per_slab;
        }
        auto *const seg = reinterpret_cast<header *>(static_cast<char *>(_carve_slab) + (segments_per_slab - _carve_left) * segment_bytes);
        --_carve_left;
        std::size_t const stagger = stagger_slot(_carved++) * line_items;
        seg->next                 = nullptr;
        seg->begin                = seg->items() + stagger;
        seg->capacity             = static_cast<std::uint32_t>(raw_capacity - stagger);
        seg->end                  = 0;
        seg->standard             = true;
        ++_allocated;
        return seg;
    }

    // Dedicated segment: exactly sized, its own allocation, never pooled.
    [[nodiscard]] header *
    make_dedicated(std::size_t const capacity) {
        assert(capacity <= UINT32_MAX && "a dedicated segment is bounded by the header's 32-bit capacity");
        auto *const seg = reinterpret_cast<header *>(base_type::allocate(header::header_items() + capacity));
        seg->next       = nullptr;
        seg->begin      = seg->items();
        seg->capacity   = static_cast<std::uint32_t>(capacity);
        seg->end        = 0;
        seg->standard   = false;
        ++_allocated;
        return seg;
    }

    void
    destroy_dedicated(header *const seg) noexcept {
        base_type::deallocate(reinterpret_cast<T *>(seg), header::header_items() + seg->capacity);
        --_allocated;
    }

public:
    segment_pool() = default;

    ~segment_pool() noexcept {
        // Every pipe drawing from this pool must already be destroyed: a segment still lent out
        // would be released into a dead pool, and its slab stays mapped (leaked, not freed).
        shrink();
        assert(_slabs.empty() && "segment_pool destroyed with segments still lent");
    }

    /**
     * @brief Hand out a segment able to hold `items` items.
     * @throws std::bad_alloc when a slab or a dedicated segment has to be obtained and the
     *         platform or the allocator refuses.
     */
    [[nodiscard]] header *
    acquire(std::size_t const items) {
        if (likely(items <= segment_capacity)) {
            if (header *const seg = _free) {
                _free     = seg->next;
                seg->next = nullptr;
                --_retained;
                return seg;
            }
            return carve();
        }
        return make_dedicated(items);
    }

    /**
     * @brief Take a segment back: a standard one joins the free list, a dedicated one is freed.
     */
    void
    release(header *const seg) noexcept {
        if (likely(seg->standard)) {
            seg->next = _free;
            _free     = seg;
            ++_retained;
            return;
        }
        destroy_dedicated(seg);
    }

    /**
     * @brief Return to the slab cache every slab none of whose segments is lent.
     *
     * A slab is releasable when each of its segments is either on the free list or not yet
     * carved. Lent segments pin their slab, and the free-listed segments of a pinned slab stay
     * retained. Cold: walks the free list once per slab.
     */
    void
    shrink() noexcept {
        for (std::size_t i = 0; i < _slabs.size();) {
            void *const       slab     = _slabs[i];
            std::size_t const uncarved = slab == _carve_slab ? _carve_left : 0;
            std::size_t       free     = 0;
            for (header const *s = _free; s; s = s->next)
                free += in_slab(s, slab);
            if (free + uncarved != segments_per_slab) {
                ++i;
                continue;
            }
            for (header **link = &_free; *link;) {
                if (in_slab(*link, slab)) {
                    *link = (*link)->next;
                    --_retained;
                    --_allocated;
                } else {
                    link = &(*link)->next;
                }
            }
            if (uncarved) {
                _carve_slab = nullptr;
                _carve_left = 0;
            }
            slab_cache::release(slab);
            _slabs[i] = _slabs.back();
            _slabs.pop_back();
        }
    }

    /// Standard segments sitting on the free list.
    [[nodiscard]] std::size_t
    retained() const noexcept {
        return _retained;
    }

    /// Segments currently lent to pipes (standard or dedicated).
    [[nodiscard]] std::size_t
    outstanding() const noexcept {
        return _allocated - _retained;
    }

    /// Slabs this pool holds (carving, retained or lent).
    [[nodiscard]] std::size_t
    slabs() const noexcept {
        return _slabs.size();
    }
};

/**
 * @class segmented_pipe
 * @brief Growable FIFO of `T` items whose growth appends a segment and whose items never move.
 *
 * The write side is `allocate_back(n)` — a contiguous range of `n` items at the tail, at an
 * address that stays valid until the segment holding it is popped — and `free_back(n)`, which
 * undoes the LAST `allocate_back` (the shape `VirtualCore::send` needs: allocate, try the ring,
 * give the range back if the ring took it). The read side is the head segment's live range,
 * `front()`, advanced by `consume_front(n)` and `pop_front()`; a pipe is walked segment by
 * segment, and a popped segment goes straight back to the pool so the producer's next growth
 * reuses it while it is still warm. `reset()` drops everything.
 *
 * One standard segment stays resident once a pipe has held anything, so the common case of a
 * pipe that empties every pass costs no pool traffic at all.
 *
 * Not thread-safe; see the file comment.
 *
 * @tparam T            Item type.
 * @tparam SegmentItems Items per standard segment, header included (256 KB of `T` by default).
 */
template <typename T, std::size_t SegmentItems = (256u * 1024u) / sizeof(T)>
class segmented_pipe : nocopy {
public:
    using value_type = T;
    using pool_type  = segment_pool<T, SegmentItems>;
    /// Items a standard segment holds: the widest single `allocate_back` that is pooled.
    static constexpr std::size_t segment_capacity = pool_type::segment_capacity;

private:
    using header = detail::segment_header<T>;

    pool_type *_pool;
    header    *_head = nullptr; /**< oldest segment with live items, or nullptr */
    header    *_tail = nullptr; /**< segment allocations go into, or nullptr */
    T         *_rcur = nullptr; /**< read cursor inside `_head` */
    T         *_wcur = nullptr; /**< write cursor inside `_tail` */
    T         *_wend = nullptr; /**< one past the last item `_tail` can hold */

    [[nodiscard]] T *
    front_end() const noexcept {
        return _head == _tail ? _wcur : _head->begin + _head->end;
    }

    // The tail becomes the only segment and is rewound; a dedicated (oversize) tail is not
    // worth keeping resident, so it is released and the pipe goes back to holding nothing.
    void
    rewind_tail() noexcept {
        if (unlikely(!_tail->standard)) {
            _pool->release(_tail);
            _head = _tail = nullptr;
            _rcur = _wcur = _wend = nullptr;
            return;
        }
        _head = _tail;
        _rcur = _wcur = _tail->begin;
    }

    // Drop a tail that holds no live item, so that no empty segment ever sits in the chain and
    // `front()` is empty exactly when the pipe is. Reached only from the slow path: a standard
    // tail with nothing in it fits any pooled width on the fast path, so this is either an
    // oversize request or a dedicated tail whose range `free_back` took back -- rare, which is
    // why walking the chain for the predecessor is fine here.
    void
    unlink_empty_tail() noexcept {
        header *const empty = _tail;
        if (_head == _tail) {
            _head = _tail = nullptr;
            _rcur = _wcur = _wend = nullptr;
        } else {
            header *prev = _head;
            while (prev->next != empty)
                prev = prev->next;
            prev->next = nullptr;
            _tail      = prev;
            _wcur = _wend = prev->begin + prev->end;
        }
        _pool->release(empty);
    }

    // Slow half of allocate_back: commit the tail's live end, link a segment that can hold `n`.
    [[nodiscard]] T *
    allocate_back_slow(std::size_t const n) {
        header *const seg = _pool->acquire(n); // may throw; nothing changed yet
        if (_tail && _wcur == _tail->begin)
            unlink_empty_tail();
        if (_tail) {
            _tail->end  = static_cast<std::uint32_t>(_wcur - _tail->begin);
            _tail->next = seg;
        } else {
            _head = seg;
            _rcur = seg->begin;
        }
        _tail      = seg;
        _wcur      = seg->begin;
        _wend      = _wcur + seg->capacity;
        T *const p = _wcur;
        _wcur += n;
        return p;
    }

public:
    explicit segmented_pipe(pool_type &pool) noexcept
        : _pool(&pool) {}

    segmented_pipe(segmented_pipe &&rhs) noexcept
        : _pool(rhs._pool)
        , _head(std::exchange(rhs._head, nullptr))
        , _tail(std::exchange(rhs._tail, nullptr))
        , _rcur(std::exchange(rhs._rcur, nullptr))
        , _wcur(std::exchange(rhs._wcur, nullptr))
        , _wend(std::exchange(rhs._wend, nullptr)) {}

    segmented_pipe &
    operator=(segmented_pipe &&rhs) noexcept {
        if (this != &rhs) {
            release_all();
            _pool = rhs._pool;
            _head = std::exchange(rhs._head, nullptr);
            _tail = std::exchange(rhs._tail, nullptr);
            _rcur = std::exchange(rhs._rcur, nullptr);
            _wcur = std::exchange(rhs._wcur, nullptr);
            _wend = std::exchange(rhs._wend, nullptr);
        }
        return *this;
    }

    ~segmented_pipe() noexcept {
        release_all();
    }

    /**
     * @brief Reserve `n` contiguous items at the tail.
     * @return The first item of the range. Its address is stable until the segment holding it
     *         is popped (`pop_front`, `consume_front` reaching it, `reset`, destruction).
     * @throws std::bad_alloc when a new segment is needed and cannot be allocated; the pipe is
     *         unchanged in that case.
     */
    [[nodiscard]] T *
    allocate_back(std::size_t const n) {
        assert(n > 0 && "allocate_back(0) would hand out a range the walk cannot see");
        T *const p = _wcur;
        if (likely(static_cast<std::size_t>(_wend - p) >= n)) {
            _wcur = p + n;
            return p;
        }
        return allocate_back_slow(n);
    }

    /**
     * @brief Copy `size` items' worth of bytes from `data` into a fresh tail range.
     * @tparam U The object type at `data`; the copy is byte-wise, so U must be trivially
     *           relocatable (an Event's bucket range is -- see `Event.h`).
     * @param data The source object.
     * @param size Its width in items.
     * @return The copy, at an address that is stable until its segment is popped.
     */
    template <typename U>
    U &
    recycle_back(U const &data, std::size_t const size) {
        return *reinterpret_cast<U *>(std::memcpy(allocate_back(size), &data, size * sizeof(T)));
    }

    /**
     * @brief Give back the range the LAST `allocate_back` returned.
     * @param n The width that `allocate_back` was called with. Nothing may have been allocated
     *          since.
     */
    void
    free_back(std::size_t const n) noexcept {
        assert(_tail && static_cast<std::size_t>(_wcur - _tail->begin) >= n && "free_back wider than the tail's live range");
        _wcur -= n;
    }

    /// True when no live item remains.
    [[nodiscard]] bool
    empty() const noexcept {
        return _rcur == _wcur && _head == _tail;
    }

    /**
     * @brief Live range of the head segment: the items to consume next, in FIFO order.
     * @details Empty only when the pipe is empty. Never spans two segments, so every
     *          `allocate_back` range lies whole inside one `front()`.
     */
    [[nodiscard]] std::span<T>
    front() noexcept {
        if (!_head)
            return {};
        return {_rcur, front_end()};
    }

    /**
     * @brief Drop the head segment: it goes back to the pool, or is rewound if it is also the
     *        tail (so a pipe that empties keeps one standard segment resident).
     */
    void
    pop_front() noexcept {
        if (!_head)
            return;
        if (_head == _tail) {
            rewind_tail();
            return;
        }
        header *const next = _head->next;
        _pool->release(_head);
        _head = next;
        _rcur = next->begin;
    }

    /**
     * @brief Consume the first `n` items of `front()`; pops the segment when that exhausts it.
     * @param n At most `front().size()`.
     */
    void
    consume_front(std::size_t const n) noexcept {
        assert(_head && static_cast<std::size_t>(front_end() - _rcur) >= n && "consume_front past the head's live range");
        _rcur += n;
        if (_rcur == front_end())
            pop_front();
    }

    /// Drop every live item. The tail stays resident (rewound) unless it is a dedicated one.
    void
    reset() noexcept {
        while (_head != _tail) {
            header *const next = _head->next;
            _pool->release(_head);
            _head = next;
        }
        if (_tail)
            rewind_tail();
    }

    /// Drop every live item AND the resident segment.
    void
    release_all() noexcept {
        while (_head) {
            header *const next = _head->next;
            _pool->release(_head);
            _head = next;
        }
        _tail = nullptr;
        _rcur = _wcur = _wend = nullptr;
    }

    /// Exchange contents with `rhs`, O(1). Both pipes may draw from any pool.
    void
    swap(segmented_pipe &rhs) noexcept {
        std::swap(_pool, rhs._pool);
        std::swap(_head, rhs._head);
        std::swap(_tail, rhs._tail);
        std::swap(_rcur, rhs._rcur);
        std::swap(_wcur, rhs._wcur);
        std::swap(_wend, rhs._wend);
    }

    /// Live items, summed over the segments — O(segments); the hot paths use `empty()`.
    [[nodiscard]] std::size_t
    size() const noexcept {
        if (!_head)
            return 0;
        std::size_t n = 0;
        for (header const *seg = _head; seg != _tail; seg = seg->next)
            n += seg->end;
        n += static_cast<std::size_t>(_wcur - _tail->begin);
        return n - static_cast<std::size_t>(_rcur - _head->begin);
    }

    /// Segments currently linked (live or resident) — what a walk visits.
    [[nodiscard]] std::size_t
    segments() const noexcept {
        std::size_t n = 0;
        for (header const *seg = _head; seg; seg = seg->next)
            ++n;
        return n;
    }

    /// The pool this pipe draws from.
    [[nodiscard]] pool_type &
    pool() const noexcept {
        return *_pool;
    }
};

} // namespace qb::allocator

#endif // QB_SEGMENTED_PIPE_H
