/**
 * @file qb/core/detail/monotonic_wait.h
 * @brief A bounded condition-variable wait whose time never reads the time of day on Apple.
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

#ifndef QB_CORE_DETAIL_MONOTONIC_WAIT_H
#define QB_CORE_DETAIL_MONOTONIC_WAIT_H

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>
#include <qb/system/time.h>
#if defined(__APPLE__)
#include <cerrno>
#include <ctime>
#include <pthread.h>
#endif

namespace qb::detail {

/// The longest one park of a core lasts, whatever its latency (Huly QB-1227). `qb::Main::stop()` wakes no parked core --
/// it only advances counters, to stay safe in a signal handler -- so a core sees a stop at the end of its park: a longer
/// one would hold the stop for its whole length, and `qb::duration::max()` would overflow the wait's end.
inline constexpr qb::duration kLongestPark = std::chrono::seconds{1};

/// What `wait_for_monotonic` does with a timeout past the clock's range, where `now + timeout` would overflow.
enum class past_range {
    no_end,         ///< an untimed wait: `qb::duration::max()` means no end (TestKit's watchdog)
    returns_at_once ///< `pred()` at once, as libc++'s and libstdc++'s `wait_for` end up doing
};

/**
 * @brief `cv.wait_for(lk, timeout, pred)`, timed on the monotonic clock on Apple, never overflowing.
 * @return `pred()` once the wait ends, as `std::condition_variable::wait_for` returns it.
 * @details libc++ on Apple has no `pthread_cond_clockwait`: its `wait_for` -- and its `wait_until` of a `steady_clock`
 *          instant alike -- turns the wait into an absolute deadline of `system_clock::now()`, which libpthread turns
 *          back into a relative wait with its own reading of the time of day. A real step of the time of day between
 *          those two reads (well under a microsecond), or a process whose `system_clock::now()` is interposed (a time
 *          mock), shifted the wait by the difference (Huly QB-1227: a core parked on its mailbox, TestKit's watchdog).
 *          There the wait is `pthread_cond_timedwait_relative_np`, re-armed from `qb::mono_now()` after every wake, with
 *          no reading of the time of day at all. Elsewhere it is the standard library's `wait_until` of an end at most a
 *          day after a fresh `qb::mono_now()`, re-armed the same way: `wait_for` read the clock again, and where libstdc++
 *          has no `pthread_cond_clockwait` it moves a `steady_clock` end to `system_clock`, whose headroom is smaller -- a
 *          timeout the range check let through could overflow there. libstdc++ on glibc 2.30 or newer waits on
 *          `CLOCK_MONOTONIC`; musl, the BSDs' libc++, older glibc and Android below API 30 keep a `CLOCK_REALTIME`
 *          deadline; MSVC's STL is to be confirmed.
 *
 *          A timeout past the clock's range (`qb::duration::max()`) overflowed `now + timeout` in both standard
 *          libraries, and the wait returned at once. `beyond` says what happens instead, on every platform: an untimed
 *          wait, or `pred()` at once without the overflow. A core's park never asks for one: it lasts `kLongestPark`
 *          at most, so that it sees `qb::Main::stop()`, which wakes no parked core.
 */
template <typename Predicate>
[[nodiscard]] bool
wait_for_monotonic(std::condition_variable &cv, std::unique_lock<std::mutex> &lk, qb::duration const timeout, Predicate pred,
                   past_range const beyond = past_range::no_end) {
    if (timeout <= qb::duration::zero())
        return pred(); // as `wait_for` does; `now + timeout` never runs below the clock's range either
    auto const now = qb::mono_now();
    if (timeout >= qb::mono_time::max() - now) {
        if (beyond == past_range::returns_at_once)
            return pred();
        cv.wait(lk, pred);
        return true;
    }
    // One wait is capped at a day, re-armed from a fresh reading until the end: a relative `timespec` the kernel takes on
    // Apple, and elsewhere an end no standard library overflows, whichever clock it converts it to.
    constexpr std::int64_t kSliceNs = std::int64_t{86'400} * 1'000'000'000;
    auto const             until    = now + timeout;
    while (!pred()) {
        auto const at   = qb::mono_now();
        auto const left = std::chrono::duration_cast<std::chrono::nanoseconds>(until - at).count();
        if (left <= 0)
            return pred();
        auto const slice = left < kSliceNs ? left : kSliceNs;
#if defined(__APPLE__)
        timespec const rel{static_cast<std::time_t>(slice / 1'000'000'000), static_cast<long>(slice % 1'000'000'000)};
        int const      rc = ::pthread_cond_timedwait_relative_np(cv.native_handle(), lk.mutex()->native_handle(), &rel);
        if (rc != 0 && rc != ETIMEDOUT)
            std::terminate(); // what libc++'s wait_for does on the same error, a throw out of a noexcept park
#else
        (void) cv.wait_until(lk, at + std::chrono::nanoseconds{slice}); // a spurious wake or the slice's end: the loop decides
#endif
    }
    return true;
}

} // namespace qb::detail

#endif // QB_CORE_DETAIL_MONOTONIC_WAIT_H
