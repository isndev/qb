/**
 * @file qb/core/Pipe.h
 * @brief Low-level actor communication channel for the QB Actor Framework.
 *
 * Defines `qb::Pipe`, the direct communication channel between two actors.
 * A `Pipe` object is typically obtained via `Actor::getPipe(dest)` and provides
 * fine-grained control over event sending — including support for pre-sized
 * buffer allocation for large payloads.
 *
 * ### When to use `Pipe` directly
 * - Sending **multiple events** to the same destination in one logical operation:
 *   the pipe lookup (hash-map access) is done once, then all events are pushed
 *   through the same channel.
 * - Sending **large events** whose dynamic payload size is known ahead of time:
 *   `allocated_push()` reserves the exact amount of pipe buffer space to avoid
 *   internal reallocations.
 * - Low-level integration where the caller needs direct buffer access.
 *
 * For the common case of sending a single event, prefer `Actor::push<E>(dest, ...)`
 * or the fluent `Actor::to(dest).push<E>(...)` interface.
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

#ifndef QB_PROXYPIPE_H
#define QB_PROXYPIPE_H
#include "ActorId.h"
#include "Event.h"

namespace qb {

/*!
 * @class Pipe
 * @ingroup PipeCore
 * @brief Low-level, typed communication channel between two actors.
 *
 * @details
 * A `Pipe` is the underlying object that routes events from a source actor
 * to a destination actor. It wraps a `VirtualPipe` (the lock-free ring-buffer
 * segment owned by the source `VirtualCore`) together with the identity of
 * both endpoints.
 *
 * Instances are obtained via `Actor::getPipe(dest)`. Holding a `Pipe` object
 * avoids repeated map lookups when sending several events to the same destination
 * within a single event handler or callback.
 *
 * ### Example — bulk send via Pipe
 * @code
 * // Inside an actor event handler
 * qb::Pipe channel = getPipe(worker_id);
 *
 * channel.push<StartEvent>();
 * channel.push<DataEvent>(buffer, length);
 * channel.push<DoneEvent>(status_code);
 * @endcode
 *
 * ### Example — large payload with `allocated_push`
 * @code
 * // 1 MB of bulk data, kept on the heap so the event stays small.
 * auto large_vec = std::make_shared<std::vector<char>>(1024 * 1024);
 * // ... fill large_vec ...
 *
 * qb::Pipe pipe = getPipe(processor_id);
 * // `size` is the TRAILING bytes reserved AFTER the event, not the total footprint:
 * // allocated_push adds sizeof(LargeDataEvent) itself. The vector is owned by the
 * // shared_ptr, so the event carries only the pointer — no trailing bytes needed.
 * auto& ev = pipe.allocated_push<LargeDataEvent>(0, large_vec);
 * // ev is fully constructed in the pre-allocated buffer slot.
 * @endcode
 *
 * @note A default-constructed `Pipe` is invalid (its internal pointer is null).
 *       Always initialise a `Pipe` via `Actor::getPipe()`.
 *
 * @see Actor::getPipe
 * @see Actor::to
 * @see Actor::push
 */
class Pipe {
    friend class VirtualCore;

    VirtualPipe *pipe;
    ActorId      dest;
    ActorId      source;

    Pipe(VirtualPipe &i_pipe, ActorId i_dest, ActorId i_source) noexcept
        : pipe(&i_pipe)
        , dest(i_dest)
        , source(i_source) {}

public:
    Pipe()                        = default;
    Pipe(Pipe const &)            = default;
    Pipe &operator=(Pipe const &) = default;

    /*!
     * @brief Construct and enqueue an event of type `_Event` through this pipe.
     *
     * @tparam _Event  Event type to construct and send (must derive from `qb::Event`).
     * @tparam _Args   Types of constructor arguments forwarded to `_Event`.
     * @param  args    Arguments forwarded to the `_Event` constructor.
     * @return Mutable reference to the newly constructed event in the pipe buffer.
     *         The caller may modify event fields via this reference before the event
     *         is consumed by the destination actor.
     *
     * @attention **The reference is invalidated by the very next event queued into this pipe** —
     *            the pipe is a growable buffer, so a later `push`/`allocated_push` (including one
     *            issued indirectly by `Actor::push`/`send`/`broadcast` to any actor on the same
     *            destination core) either reallocates it or compacts it in place. Compaction is
     *            the dangerous case: the stale reference stays inside a live allocation and
     *            silently aliases a different event. Finish populating the event before queueing
     *            anything else. Pinned by `PipeAllocatorContract.*` in
     *            `qb-io/tests/unit/core/pipe-allocator.cpp`.
     *
     * @details
     * Events pushed through `Pipe::push()` are delivered in FIFO order relative to
     * other `push()` calls from the same source to the same destination. This mirrors
     * the ordering guarantees of `Actor::push()`.
     *
     * @note Use `allocated_push()` instead when the event carries a large, dynamically
     *       sized payload and you want to avoid internal buffer reallocation.
     *
     * @warning **noexcept + allocation.** This method is `noexcept`, yet it grows the
     *          pipe buffer (which may `throw std::bad_alloc` on memory exhaustion) and
     *          constructs the event in place (whose constructor may also throw, e.g. an
     *          event holding a `std::string`/`std::vector` under low memory). Because a
     *          throw cannot escape a `noexcept` function, **any such failure calls
     *          `std::terminate()` and aborts the whole process** — it is not a
     *          recoverable error. This is by design: events are expected to be small,
     *          quasi-POD messages on a system provisioned with enough memory, and an
     *          allocation failure in the messaging hot path is treated as fatal. Keep
     *          event constructors allocation-light (or move heap data in via
     *          already-allocated `shared_ptr`s) if process-abort-on-OOM is unacceptable.
     *          The same contract applies to `allocated_push()`, `Actor::push()`,
     *          `Actor::send()` and `Actor::broadcast()`.
     */
    template <typename _Event, typename... _Args>
    _Event &push(_Args &&...args) const noexcept;

    /*!
     * @brief Construct and enqueue an event with a caller-supplied buffer size hint.
     *
     * @tparam _Event  Event type to construct and send (must derive from `qb::Event`).
     * @tparam _Args   Types of constructor arguments forwarded to `_Event`.
     * @param  size    **Extra** bytes to reserve *after* the event object — NOT the total
     *                 footprint. The implementation adds `sizeof(_Event)` itself
     *                 (below: `size += sizeof(T)`) and then rounds the sum up to whole
     *                 `QB_LOCKFREE_EVENT_BUCKET_BYTES` buckets. So pass only the trailing
     *                 payload bytes: `allocated_push<E>(0)` reserves exactly one event, and
     *                 `allocated_push<E>(n)` reserves `ceil((sizeof(E) + n) / bucket)` buckets.
     *                 Passing `sizeof(_Event) + n` — as an earlier revision of this comment
     *                 wrongly advised — over-reserves by a whole event and **halves the usable
     *                 cross-core size ceiling** documented in the @warning below.
     * @param  args    Arguments forwarded to the `_Event` constructor.
     * @return Mutable reference to the newly constructed event in the pre-allocated slot.
     *         Invalidated by the next event queued into this pipe — same contract as `push()`.
     *
     * @details
     * Use this overload instead of `push()` when the event's **effective in-pipe size**
     * exceeds `sizeof(_Event)` — for example when `_Event` owns a `shared_ptr` to a
     * large heap-allocated container that you want the framework to account for during
     * buffer growth decisions.
     *
     * Ordering guarantees are identical to `push()`.
     *
     * ### Example
     * @code
     * // 1 MB binary blob forwarded to a processing actor
     * auto blob = std::make_shared<std::vector<std::byte>>(1024 * 1024);
     * // ... fill blob ...
     *
     * struct BlobEvent : qb::Event {
     *     std::shared_ptr<std::vector<std::byte>> data;
     *     explicit BlobEvent(std::shared_ptr<std::vector<std::byte>> d)
     *         : data(std::move(d)) {}
     * };
     *
     * qb::Pipe pipe = getPipe(processor_id);
     * // `size` is the TRAILING bytes only — sizeof(BlobEvent) is added internally.
     * // Here the blob lives on the heap behind a shared_ptr, so the event's in-pipe
     * // footprint is just the event: no trailing bytes are needed at all.
     * auto& ev = pipe.allocated_push<BlobEvent>(0, blob);
     * @endcode
     *
     * @note `size == 0` is the normal case and reserves exactly the event
     *       (`ceil(sizeof(_Event) / QB_LOCKFREE_EVENT_BUCKET_BYTES)` buckets) — the event
     *       itself is never under-allocated. Only pass a non-zero `size` when you intend to
     *       write raw bytes into the region immediately following the event object, as
     *       `qb-core`'s messaging tests do
     *       (`allocated_push<TestEvent>(32)` + a 32-byte tail).
     *       The exact bucket arithmetic is pinned by
     *       `EventHeader.AllocatedPushRoundingMatchesCeilDivide` in
     *       `source/core/tests/unit/core/event-header.cpp`.
     *
     * @warning **Maximum event size.** An event's total in-pipe footprint must not
     *          exceed the per-core mailbox ring capacity, which is
     *          `(std::numeric_limits<uint16_t>::max() / QB_LOCKFREE_EVENT_BUCKET_BYTES)`
     *          buckets — i.e. **~1023 buckets (≈64 KiB with the default 64-byte,
     *          cache-line bucket)**. A larger event cannot be enqueued into another
     *          core's mailbox, so the cross-core flush **drops it**: it is disposed,
     *          the pipe advances past it, and a `QB_LOG_CRIT` names the source,
     *          destination and bucket count. Dropping is deliberate — the event is
     *          undeliverable by construction rather than by timing, so retrying it
     *          would hold the whole outbound stream to that core hostage
     *          (head-of-line) and `Main::join()` would never return. The
     *          `bucket_size` header field is also a `uint16_t`, so an event spanning
     *          ≥ 65536 buckets wraps it; the value 65536 truncates to 0, and a
     *          zero-width event cannot be walked past, so the flush logs `QB_LOG_CRIT`
     *          and **discards the rest of that pipe**. Either way the events are
     *          lost, not delivered — the engine stays live, but the messages do not
     *          arrive. For large payloads, put the data on the heap (e.g. a
     *          `std::shared_ptr<std::vector<T>>` member) and keep the event itself
     *          small — do **not** size `allocated_push` to the payload bytes.
     *          Pinned by `OversizeEvent.OversizedEventDoesNotWedgeTheEngine` in
     *          `source/core/tests/system/messaging/oversize-event-probe.cpp`.
     */
    template <typename _Event, typename... _Args>
    [[nodiscard]] _Event &allocated_push(std::size_t size, _Args &&...args) const noexcept;

    /*!
     * @brief Get the destination actor ID
     * @return ActorId of the destination
     */
    [[nodiscard]] inline ActorId
    getDestination() const noexcept {
        return dest;
    }

    /*!
     * @brief Get the source actor ID
     * @return ActorId of the source
     */
    [[nodiscard]] inline ActorId
    getSource() const noexcept {
        return source;
    }
};

/**
 * @typedef pipe
 * @brief Alias for the Pipe class
 * @details Provided for naming consistency with other lowercase aliases in the framework
 * @ingroup PipeCore
 */
using pipe = Pipe;

} // namespace qb

// ============================================================================================
// Pipe -- TEMPLATE BODIES.  TEMPLATES ONLY BELOW THIS LINE.
//
// Event construction and pushing through the channel: the definitions of `Pipe::push` and
// `Pipe::allocated_push` declared above. They shipped as `core/Pipe.tpp` through 2.6.0 and
// moved here in 3.0 -- `.h` is now the only header extension in qb.
//
// Pipe.tpp had NO includes of its own; it relied entirely on whatever its includer had already
// pulled, which is why it could never have been compiled alone. Standing on its own needs two
// things, and both are handled here rather than at the top of the file:
//   * `<qb/system/event/router.h>` for `router::ensure_disposer`. `router` appears nowhere in
//     Pipe's declarations -- it is a body-only dependency, so the include belongs next to the
//     bodies. router.h pulls nothing from qb/core, so there is no cycle either way.
//   * the `service_event_type` concept, which lived at Actor.h:141-142 and was unreachable
//     from here: Actor.h includes THIS header at its line 50, 91 lines before it declared the
//     concept. It now lives at Event.h:696, which Pipe.h already includes at :41.
// Both are position problems, not file-extension problems -- which is the whole reason a `.tpp`
// was never the fix.
//
// The include and the bodies are appended after the namespace closes so that no line of the
// declarations above moves: Pipe.h carries cited anchors at :118, :126, :127 and :135.
//
// TEMPLATES ONLY, and the reason outlives the extension: this header is reached both by
// libqb-core's single amalgamated TU (via Actor.cpp) and by every consumer TU, and the include
// guard stops double inclusion only WITHIN one TU. Every definition below is a template, which
// gives it vague linkage; one non-template, non-`inline` definition added here is an instant
// duplicate symbol between the archive and any consumer object. `inline` is not the workaround
// -- it leaves N definitions of one entity in the program. Anything non-template belongs in a
// .cpp.
// ============================================================================================
#include <qb/system/event/router.h>

namespace qb {


template <typename T, typename... _Args>
T &
Pipe::push(_Args &&...args) const noexcept {
    router::ensure_disposer<Event, T>();
    constexpr std::size_t BUCKET_SIZE = allocator::getItemSize<T, EventBucket>();
    auto                 &data        = pipe->template allocate_back<T>(std::forward<_Args>(args)...);
    data.id                           = data.template type_to_id<T>();
    data.dest                         = dest;
    data.source                       = source;
    // C++20: use service_event_type concept
    if constexpr (service_event_type<T>) {
        data.forward = source;
        std::swap(data.id, data.service_event_id);
    }

    data.bucket_size = BUCKET_SIZE;
    return data;
}

template <typename T, typename... _Args>
T &
Pipe::allocated_push(std::size_t size, _Args &&...args) const noexcept {
    router::ensure_disposer<Event, T>();
    size += sizeof(T);
    size       = size / sizeof(EventBucket) + static_cast<bool>(size % sizeof(EventBucket));
    auto &data = *(new (reinterpret_cast<T *>(pipe->allocate_back(size))) T(std::forward<_Args>(args)...));

    data.id     = data.template type_to_id<T>();
    data.dest   = dest;
    data.source = source;
    // C++20: use service_event_type concept
    if constexpr (service_event_type<T>) {
        data.forward = source;
        std::swap(data.id, data.service_event_id);
    }

    data.bucket_size = static_cast<uint16_t>(size);
    return data;
}

} // namespace qb
#endif // QB_PROXYPIPE_H
