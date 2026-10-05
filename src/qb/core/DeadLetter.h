/**
 * @file qb/core/DeadLetter.h
 * @brief Dead letters: the events that reached no actor, why, and the per-core hook that hears of them.
 *
 * Defines `qb::DeadLetterReason`, `qb::DeadLetter` -- the metadata of one undeliverable event, never
 * the event itself -- and `qb::DeadLetterHandler`, the callable a `qb::CoreInitializer` installs
 * before `qb::Main::start()` (`setDeadLetterHandler`). Every dead letter is also counted in the
 * core's `qb::CoreStats`, hook or not.
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

#ifndef QB_DEAD_LETTER_H
#define QB_DEAD_LETTER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include "Event.h"

namespace qb {

/*!
 * @enum DeadLetterReason
 * @ingroup Engine
 * @brief Why an event reached no actor.
 */
enum class DeadLetterReason : std::uint8_t {
    /// No live actor has the destination id on its core: it never existed, or it was removed. The
    /// stale-id and kill/message-race signal. (An event reaching an actor that handles its type but
    /// was killed earlier in the same pass, before its removal, is skipped by the dispatch unreported.)
    not_found,
    /// The destination actor is alive but handles no event of this type: a wiring bug, a
    /// `registerEvent<E>(*this)` that was never made.
    unhandled,
    /// The destination was in its asynchronous `onInit()` and its stash of early events was full;
    /// the activation is failed with it.
    stash_overflow,
    /// The event was held for an actor whose asynchronous `onInit()` then failed, threw, timed out
    /// or was killed: it is discarded unreplayed.
    init_failed,
    /// The event is wider than the cross-core mailbox ring -- the trailing bytes of a
    /// `qb::Pipe::allocated_push` -- so no amount of draining could ever deliver it.
    oversize,
    /// The event was still queued for a core that had already stopped when this core shut down.
    peer_stopped,
};

/// Number of `qb::DeadLetterReason` values: the size of `qb::CoreStats::dead_letters_by_reason`.
inline constexpr std::size_t DeadLetterReasons = 6;

/*!
 * @brief The name of a `qb::DeadLetterReason`, for log lines.
 * @ingroup Engine
 */
[[nodiscard]] constexpr std::string_view
dead_letter_reason_name(DeadLetterReason const reason) noexcept {
    switch (reason) {
        case DeadLetterReason::not_found:
            return "not_found";
        case DeadLetterReason::unhandled:
            return "unhandled";
        case DeadLetterReason::stash_overflow:
            return "stash_overflow";
        case DeadLetterReason::init_failed:
            return "init_failed";
        case DeadLetterReason::oversize:
            return "oversize";
        case DeadLetterReason::peer_stopped:
            return "peer_stopped";
    }
    return "unknown";
}

/*!
 * @struct DeadLetter
 * @ingroup Engine
 * @brief What a dead-letter handler is told about an event that reached no actor: its metadata,
 *        never the event -- which has been, or is about to be, destroyed.
 */
struct DeadLetter {
    EventId          event_id;    ///< the event's type id: `qb::event_type_name(event_id)` names it
    ActorId          source;      ///< who sent it
    ActorId          destination; ///< whom it was for
    CoreId           core;        ///< the core that gave up on it, the one running the handler
    DeadLetterReason reason;      ///< why it reached no actor
};

/*!
 * @typedef DeadLetterHandler
 * @ingroup Engine
 * @brief A per-core dead-letter hook, installed with `qb::CoreInitializer::setDeadLetterHandler()`.
 * @details Called on the core's own thread, in the middle of its loop pass, once per dead letter:
 *          keep it short and non-blocking. It sees metadata only, and there is nothing to re-route
 *          -- the event is gone. One callable shared by several cores is called concurrently from
 *          their threads, so what it captures must be safe for that. An exception escaping it is
 *          contained and logged; the core carries on.
 */
using DeadLetterHandler = std::function<void(DeadLetter const &)>;

} // namespace qb
#endif // QB_DEAD_LETTER_H
