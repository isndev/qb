/**
 * @file qb/core/DeathWatch.h
 * @brief Death watch: learn when another actor is really gone, whatever ended it.
 *
 * Defines `qb::DownReason` and `qb::DownEvent`, the event a watcher receives once the actor it
 * watches (`qb::Actor::watch()`) has been destroyed -- after its destructor ran, on any core, for
 * any cause: a `kill()`, an `onInit()` that failed or threw, the engine's shutdown. Watching an id
 * that holds no actor answers at once.
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
 * @ingroup Actor
 */

#ifndef QB_DEATH_WATCH_H
#define QB_DEATH_WATCH_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "Event.h"

namespace qb {

/*!
 * @enum DownReason
 * @ingroup Actor
 * @brief Why a watched actor is gone.
 */
enum class DownReason : std::uint8_t {
    /// It was killed (`kill()`, a `KillEvent`, the engine's shutdown) and its destructor ran.
    killed,
    /// Its `onInit()` returned false, or did not complete before its activation deadline.
    init_failed,
    /// Its `onInit()` threw.
    init_threw,
    /// Its core had stopped when the watch reached it, so the actor cannot be there; or the core
    /// ended on an exception that escaped a handler, and its actors with it.
    core_stopped,
    /// No actor held the id when the watch reached its core: it never existed, or it was gone.
    unknown,
};

/// Number of `qb::DownReason` values.
inline constexpr std::size_t DownReasons = 5;

/*!
 * @brief The name of a `qb::DownReason`, for log lines.
 * @ingroup Actor
 */
[[nodiscard]] constexpr std::string_view
down_reason_name(DownReason const reason) noexcept {
    switch (reason) {
        case DownReason::killed:
            return "killed";
        case DownReason::init_failed:
            return "init_failed";
        case DownReason::init_threw:
            return "init_threw";
        case DownReason::core_stopped:
            return "core_stopped";
        case DownReason::unknown:
            return "unknown";
    }
    return "?";
}

/*!
 * @struct DownEvent
 * @ingroup Actor
 * @brief What a watcher receives, once, when an actor it watches (`qb::Actor::watch()`) is gone.
 * @details Delivered once the watched actor's destructor has run -- or at once, when no actor holds
 *          the id or its core has stopped; `getSource()` is the watched id too. Register it like any
 *          event -- `registerEvent<qb::DownEvent>(*this)` -- or it reaches the watcher as an
 *          `unhandled` dead letter.
 */
struct DownEvent : Event {
    ActorId    watched{};                   ///< the actor that is gone
    DownReason reason = DownReason::killed; ///< why

    DownEvent() = default;
    DownEvent(ActorId const gone, DownReason const why) noexcept
        : watched(gone)
        , reason(why) {}
};

namespace detail {

// The death watch's control events (Huly QB-51), each resolved by a core itself, never by an actor.
// A request and its withdrawal are addressed to the watched actor's CORE, not to the actor: a
// unicast to an actor still in its `onInit()` is stashed, and dropped with the stash if that fails.
// Every watch carries a ticket, a number its watcher's core gives it, and its answer carries it
// back: ids are reused, and the ticket is what tells an answer about an id's previous holder from
// the answer to a watch of its next one.

/// A watch on its way to the watched actor's core: destination `BroadcastId(that core)`, source the
/// watcher.
struct WatchRequest : Event {
    ActorId       target{};   ///< the actor to watch
    std::uint64_t ticket = 0; ///< the watch's number, which its answer carries back

    WatchRequest() = default;
    WatchRequest(ActorId const watched, std::uint64_t const number) noexcept
        : target(watched)
        , ticket(number) {}
};
/// The withdrawal of a watch, same addressing.
struct UnwatchRequest : Event {
    ActorId target{}; ///< the actor no longer watched

    UnwatchRequest() = default;
    explicit UnwatchRequest(ActorId const watched) noexcept
        : target(watched) {}
};
/// The answer to a watch, on its way to the watcher: destination the watcher, source the watched
/// id. The watcher's core turns it into the `qb::DownEvent` it delivers -- if the watch it answers
/// is still open there, neither withdrawn by `unwatch()` nor answered already.
struct WatchDown : Event {
    DownReason    reason = DownReason::killed;
    std::uint64_t ticket = 0; ///< the ticket of the watch it answers

    WatchDown() = default;
    WatchDown(DownReason const why, std::uint64_t const number) noexcept
        : reason(why)
        , ticket(number) {}
};
/// A core that has stopped, to every core still running -- sent only once a watch has crossed
/// cores: each answers (`core_stopped`) the watches it still holds open on that core.
/// Destination `BroadcastId(core)`, source `BroadcastId(stopped core)`.
struct CoreStopping : Event {};

} // namespace detail

} // namespace qb
#endif // QB_DEATH_WATCH_H
