/**
 * @file qb/core/CoreStats.h
 * @brief Cumulative activity counters of one `VirtualCore`, read through `qb::Actor::getCoreStats()`.
 *
 * Defines `qb::CoreStats`, a plain struct of `std::uint64_t` counters a core accumulates from
 * its first loop pass to its last. The counters are the core's own -- written by its thread,
 * never shared, never atomic -- so the only reader is an actor running on that core, through
 * `qb::Actor::getCoreStats()`, which copies them out. A view across cores is built by asking each
 * core for its copy, the way any cross-core question is asked.
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
 * @ingroup Engine
 */

#ifndef QB_CORE_STATS_H
#define QB_CORE_STATS_H

#include <array>
#include <cstdint>
#include "DeadLetter.h"

namespace qb {

/*!
 * @struct CoreStats
 * @ingroup Engine
 * @brief What one `VirtualCore` has done since it started: events in and out, losses, io, passes.
 *
 * @details
 * A snapshot, returned by value from `qb::Actor::getCoreStats()` and taken on the calling actor's
 * own core. Every field is a monotonic count since that core started; two snapshots taken some
 * time apart give a rate. Nothing is reset: these are the counts the core keeps for its own idle
 * policy, accumulated instead of cleared every pass, and the snapshot is assembled only when asked.
 *
 * Same-core traffic shows on the receiving side only: an event an actor pushes to an actor of
 * its own core never leaves the core, so it is counted in `events_received` and not in
 * `events_sent`.
 */
struct CoreStats {
    /// Passes of the core's event loop -- the same count `qb::LoopEvent::iteration` reports.
    std::uint64_t loop_passes = 0;
    /// Events the core took in, from other cores' mailboxes and from its own same-core queue,
    /// whether an actor handled them or not.
    std::uint64_t events_received = 0;
    /// Size of `events_received`, in 64-byte event buckets.
    std::uint64_t buckets_received = 0;
    /// Events the core published into ANOTHER core's mailbox: a `push` once its outbound queue
    /// is flushed, a `send`, `reply` or `forward` that went straight into the peer's ring.
    std::uint64_t events_sent = 0;
    /// Size of `events_sent`, in 64-byte event buckets.
    std::uint64_t buckets_sent = 0;
    /// Attempts to publish an event that found the destination core's mailbox full -- the
    /// backpressure signal. A retried event counts once per failed attempt; the event itself is
    /// sent later (`events_sent`) or, for a `qb::EventQOS0`, dropped (`events_dropped`).
    std::uint64_t sends_blocked = 0;
    /// `qb::EventQOS0` events discarded because the destination's mailbox was full, as their
    /// contract allows. No other event is ever discarded on backpressure.
    std::uint64_t events_dropped = 0;
    /// Events that reached no actor -- dead letters, whatever the reason; the sum of
    /// `dead_letters_by_reason`. Each one also reaches the core's `qb::DeadLetterHandler`, if any.
    std::uint64_t dead_letters = 0;
    /// `dead_letters` split by `qb::DeadLetterReason`, indexed by its value.
    std::array<std::uint64_t, DeadLetterReasons> dead_letters_by_reason{};
    /// qb-io callbacks the core's loop ran: watcher callbacks, deferred callbacks and coroutine
    /// resumes, including those an idle core's park delivered
    /// (`qb::io::async::listener::total_events_processed()`).
    std::uint64_t io_events = 0;
};

} // namespace qb
#endif // QB_CORE_STATS_H
