/**
 * @file qb/io/reconnect.h
 * @brief The reconnect contract's vocabulary, shared by every qb client that keeps a connection.
 *
 * One header, names and pure functions only -- no runtime code, nothing to link. A client of qb-io
 * or of a qbm module that keeps a connection open (qbm-redis, qbm-pgsql, the qbm-http clients) reports
 * where it stands and what it lost with these words, so a reader of one client reads every other:
 *
 * - `connection_state`: idle, connecting, handshaking, ready, reconnecting, down -- what `state()`
 *   answers. A client is usable (`is_connected()`) only in `ready`: the protocol handshake, the
 *   user's handshake hook and the session replay have run on that connection before any user byte.
 * - `down_reason`: why a client reached `down`, its only terminal state -- what `down_reason()`
 *   answers. Shaped like the death watch's `qb::DownReason` (a byte-sized enum, its count, its name).
 * - `gap_reason` and `gap_info`: why, and how much, a stream lost at one point -- what a gap marker
 *   carries. A reconnect is a gap (the server published while no connection was there), and so is
 *   an overflow of the client's own queue.
 * - `reconnect_policy`: the backoff of a reconnection run -- how many attempts, how long between
 *   them. Not `qb::io::async::retry_policy` (`coroutine/retry.h`), which retries a coroutine body
 *   on an exception, nor `qb::retry_policy` (`core/patterns/resilience.h`), which retries an `ask`.
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
 * @ingroup IO
 */

#ifndef QB_IO_RECONNECT_H
#define QB_IO_RECONNECT_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <string_view>
#include <utility>
#include <qb/system/time.h>

namespace qb::io {

/*!
 * @enum connection_state
 * @ingroup IO
 * @brief Where a connection-keeping client stands: what its `state()` answers.
 * @details The life of a client is `idle` -> `connecting` -> `handshaking` -> `ready`, and from
 *          `ready` either back through `reconnecting` -> `handshaking` -> `ready` when a reconnection
 *          run is armed and the connection was lost, or to `down`. What becomes of work submitted
 *          outside `ready` is each client's documented policy -- qbm-redis holds it until the client is
 *          ready, up to its command timeout; qbm-pgsql refuses it -- and either way it never waits for ever.
 *          A new `connect()` leaves `idle` or `down`; nothing else does.
 */
enum class connection_state : std::uint8_t {
    /// Constructed, never asked to connect. Work submitted now fails: nothing would send it.
    idle,
    /// A connection is being opened -- the transport, before any handshake.
    connecting,
    /// The transport is up and the handshake runs on it: the protocol's own (credentials, database,
    /// protocol version), the user's hook, then the replay of the session (a consumer's
    /// subscriptions). Only the handshake writes.
    handshaking,
    /// The handshake succeeded: work flows. The one state in which `is_connected()` is true.
    ready,
    /// A ready connection was lost and a reconnection run is armed: the run waits out its backoff and
    /// tries again, each attempt passing through `handshaking`.
    reconnecting,
    /// Terminal: `down_reason()` says why. Work a client held has failed; a stream has delivered what it held,
    /// then ended. Only an explicit `connect()` leaves it.
    down,
};

/// Number of `qb::io::connection_state` values.
inline constexpr std::size_t connection_states = 6;

/*!
 * @brief The name of a `qb::io::connection_state`, for log lines and metrics labels.
 * @ingroup IO
 */
[[nodiscard]] constexpr std::string_view
connection_state_name(connection_state const state) noexcept {
    switch (state) {
        case connection_state::idle:
            return "idle";
        case connection_state::connecting:
            return "connecting";
        case connection_state::handshaking:
            return "handshaking";
        case connection_state::ready:
            return "ready";
        case connection_state::reconnecting:
            return "reconnecting";
        case connection_state::down:
            return "down";
    }
    return "?";
}

/*!
 * @enum down_reason
 * @ingroup IO
 * @brief Why a client reached `connection_state::down`: what its `down_reason()` answers.
 * @details A loss is recoverable only when a reconnection run is armed; `user` never is. Shaped like
 *          the death watch's `qb::DownReason`. The same words name the cause of a lost connection
 *          before a run takes over (`user`, `peer`, `protocol`): a client's `on_disconnected` hook
 *          receives one of those three.
 */
enum class down_reason : std::uint8_t {
    /// `disconnect()` was called. Terminal whatever the reconnection policy: an explicit disconnect
    /// starts no run.
    user,
    /// The peer closed or reset the connection, could not be reached, or stopped answering (the
    /// client's command watchdog dropped it), and no reconnection run was armed.
    peer,
    /// The peer sent bytes the client could not accept (a frame its parser refused, a message over the
    /// size limit), and no reconnection run was armed.
    protocol,
    /// A reconnection run spent its `reconnect_policy::max_attempts` without a ready connection.
    retries_exhausted,
    /// The server refused the handshake -- bad credentials, a missing permission, a database out of
    /// range -- or the user's handshake hook returned `false`. Retrying cannot heal it, so no run
    /// retries it; a transport failure DURING a handshake is not a refusal, only one more attempt.
    handshake_refused,
};

/// Number of `qb::io::down_reason` values.
inline constexpr std::size_t down_reasons = 5;

/*!
 * @brief The name of a `qb::io::down_reason`, for log lines and metrics labels.
 * @ingroup IO
 */
[[nodiscard]] constexpr std::string_view
down_reason_name(down_reason const reason) noexcept {
    switch (reason) {
        case down_reason::user:
            return "user";
        case down_reason::peer:
            return "peer";
        case down_reason::protocol:
            return "protocol";
        case down_reason::retries_exhausted:
            return "retries_exhausted";
        case down_reason::handshake_refused:
            return "handshake_refused";
    }
    return "?";
}

/*!
 * @enum gap_reason
 * @ingroup IO
 * @brief Why a stream (a subscription, a notification channel) has a hole at one point.
 * @details A stream that continues across a reconnection, or past a full queue, says so in band: a
 *          gap marker sits where the hole is, before the first item that follows it, so a reader
 *          knows to resynchronise from its source of truth. Markers are always on.
 */
enum class gap_reason : std::uint8_t {
    /// The connection was replaced: what the server published while no connection was there is lost,
    /// and no client can count it.
    reconnect,
    /// The client's own queue was full: the items that did not fit were dropped here.
    overflow,
};

/// Number of `qb::io::gap_reason` values.
inline constexpr std::size_t gap_reasons = 2;

/*!
 * @brief The name of a `qb::io::gap_reason`, for log lines and metrics labels.
 * @ingroup IO
 */
[[nodiscard]] constexpr std::string_view
gap_reason_name(gap_reason const reason) noexcept {
    switch (reason) {
        case gap_reason::reconnect:
            return "reconnect";
        case gap_reason::overflow:
            return "overflow";
    }
    return "?";
}

/*!
 * @struct gap_info
 * @ingroup IO
 * @brief What a stream lost at one point: the content of a gap marker.
 * @details Consecutive losses before the next delivered item are coalesced into one marker: the
 *          reason is `reconnect` when a reconnection is among them, and `lost` counts every item the
 *          client itself dropped.
 */
struct gap_info {
    gap_reason    reason = gap_reason::reconnect; ///< why the stream has a hole here
    std::uint64_t lost   = 0;                     ///< items the client dropped (overflow); a reconnect adds an unknown number on top
};

/*!
 * @struct reconnect_policy
 * @ingroup IO
 * @brief The backoff of a client's reconnection run: exponential, capped, jittered, bounded.
 * @details Attempt 1 is immediate; attempt 2 waits `initial_delay`; every further attempt multiplies
 *          the wait by `multiplier`, up to `max_delay`; each wait is jittered by up to a quarter either
 *          way when `jitter` is on; `max_attempts` bounds the run (-1 = unlimited) and the run that
 *          spends it ends `down` with `down_reason::retries_exhausted`; `on_retry(attempt, next_delay)`
 *          is told, before each wait, how many attempts failed and how long the wait will be. Pure data
 *          plus one pure function (`next_delay`), so a policy is testable on its own. The modules alias
 *          it (`qb::redis::RetryPolicy`, `qb::http::RetryPolicy`).
 */
struct reconnect_policy {
    int          max_attempts = -1;                             ///< attempts per run, -1 = unlimited
    qb::duration initial_delay{std::chrono::milliseconds(100)}; ///< the wait before attempt 2
    qb::duration max_delay{std::chrono::seconds(30)};           ///< the wait never grows past this
    double       multiplier = 2.0;                              ///< growth factor between consecutive waits
    bool         jitter     = true;                             ///< +-25 % on every wait
    /// The deadline of each attempt's connect. Zero (the default) leaves it to the client: the
    /// timeout its own `connect()` takes (qbm-redis: 3 s) or the one its setter recorded (the qbm-http
    /// clients' `set_connect_timeout()`).
    qb::duration                                              connect_timeout{};
    std::function<void(int attempt, qb::duration next_delay)> on_retry; ///< called before each wait

    /** @brief Set the maximum number of attempts per run (-1 = unlimited) */
    reconnect_policy &
    with_max_attempts(int n) noexcept {
        max_attempts = n;
        return *this;
    }
    /** @brief Set the wait before the second attempt */
    reconnect_policy &
    with_initial_delay(qb::duration d) noexcept {
        initial_delay = d;
        return *this;
    }
    /** @brief Set the cap the wait never grows past */
    reconnect_policy &
    with_max_delay(qb::duration d) noexcept {
        max_delay = d;
        return *this;
    }
    /** @brief Set the exponential backoff multiplier (below 1 reads as 1: the wait never shrinks) */
    reconnect_policy &
    with_multiplier(double m) noexcept {
        multiplier = m;
        return *this;
    }
    /** @brief Enable or disable the random jitter on the waits */
    reconnect_policy &
    with_jitter(bool j) noexcept {
        jitter = j;
        return *this;
    }
    /** @brief Set the deadline of each attempt's connect (zero = the client's own) */
    reconnect_policy &
    with_connect_timeout(qb::duration t) noexcept {
        connect_timeout = t;
        return *this;
    }
    /** @brief Set the callback invoked before each wait (attempts failed so far, next_delay) */
    reconnect_policy &
    with_on_retry(std::function<void(int, qb::duration)> cb) {
        on_retry = std::move(cb);
        return *this;
    }

    /**
     * @brief The wait after `attempt` failed attempts, before attempt `attempt + 1`.
     * @details Zero for `attempt < 1` (the first attempt is immediate); `initial_delay` after one
     *          failure; then `initial_delay * multiplier^(attempt - 1)`, each step capped at
     *          `max_delay`, in integer milliseconds. With `jitter` on and an `rng` given, a uniform
     *          +-25 % of the wait is added, and the result never drops below 1 ms once the wait was
     *          positive: a policy whose waits are zero (an `initial_delay` of zero) stays at zero, on
     *          purpose, since that is what was asked for.
     */
    [[nodiscard]] qb::duration
    next_delay(int attempt, std::mt19937 *rng = nullptr) const {
        using rep = std::chrono::milliseconds::rep;
        if (attempt < 1) {
            return qb::duration::zero();
        }
        const rep    max_ms   = std::max<rep>(0, std::chrono::duration_cast<std::chrono::milliseconds>(max_delay).count());
        rep          delay_ms = std::clamp<rep>(std::chrono::duration_cast<std::chrono::milliseconds>(initial_delay).count(), 0, max_ms);
        const double growth   = std::max(1.0, multiplier);
        for (int i = 1; i < attempt && delay_ms < max_ms; ++i) {
            const double next = static_cast<double>(delay_ms) * growth;
            delay_ms          = next >= static_cast<double>(max_ms) ? max_ms : static_cast<rep>(next);
        }
        if (jitter && rng != nullptr && delay_ms > 0) {
            const rep                          quarter = delay_ms / 4;
            std::uniform_int_distribution<rep> dist{-quarter, quarter};
            delay_ms = std::max<rep>(1, delay_ms + dist(*rng));
        }
        return qb::duration{std::chrono::milliseconds{delay_ms}};
    }

    /** @brief Whether a run that has made `attempts` attempts may make another one. */
    [[nodiscard]] constexpr bool
    allows_attempt(int attempts) const noexcept {
        return max_attempts < 0 || attempts < max_attempts;
    }
};

} // namespace qb::io

#endif // QB_IO_RECONNECT_H
