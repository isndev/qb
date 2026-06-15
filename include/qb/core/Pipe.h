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
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * // Pre-size the pipe buffer for a 1 MB payload to avoid internal reallocation.
 * auto large_vec = std::make_shared<std::vector<char>>(1024 * 1024);
 * // ... fill large_vec ...
 *
 * qb::Pipe pipe = getPipe(processor_id);
 * std::size_t hint = sizeof(LargeDataEvent) + large_vec->size();
 * auto& ev = pipe.allocated_push<LargeDataEvent>(hint, large_vec);
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
     *         is consumed by the destination actor. Do **not** store the reference
     *         past the current scope — its lifetime is managed by the framework.
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
     * @param  size    Total byte size to pre-allocate for this event in the pipe buffer.
     *                 Typically `sizeof(_Event) + dynamic_payload_bytes`. Providing an
     *                 accurate value avoids internal reallocation when the event carries
     *                 a large dynamic payload (e.g. a `std::shared_ptr<std::vector<T>>`).
     * @param  args    Arguments forwarded to the `_Event` constructor.
     * @return Mutable reference to the newly constructed event in the pre-allocated slot.
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
     * std::size_t hint = sizeof(BlobEvent) + blob->size();
     * auto& ev = pipe.allocated_push<BlobEvent>(hint, blob);
     * @endcode
     *
     * @note If `size` is smaller than `sizeof(_Event)` the framework silently uses
     *       `sizeof(_Event)` as the minimum allocation.
     *
     * @warning **Maximum event size.** An event's total in-pipe footprint must not
     *          exceed the per-core mailbox ring capacity, which is
     *          `(std::numeric_limits<uint16_t>::max() / QB_LOCKFREE_EVENT_BUCKET_BYTES)`
     *          buckets — i.e. **~1023 buckets (≈64 KiB with the default 64-byte,
     *          cache-line bucket)**. A larger event cannot be enqueued into another
     *          core's mailbox and will never be delivered cross-core (it stays
     *          stuck in the source pipe). The `bucket_size` header field is also a
     *          `uint16_t`, so an event spanning ≥ 65536 buckets wraps that field
     *          (a value of exactly 65536 truncates to 0 and stalls the receiver).
     *          For large payloads, put the data on the heap (e.g. a
     *          `std::shared_ptr<std::vector<T>>` member) and keep the event itself
     *          small — do **not** size `allocated_push` to the payload bytes.
     */
    template <typename _Event, typename... _Args>
    [[nodiscard]] _Event &allocated_push(std::size_t size,
                                         _Args &&...args) const noexcept;

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
#endif // QB_PROXYPIPE_H
