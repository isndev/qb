/**
 * @file qb/tests/io/shared/io_cadence_window.h
 * @brief A case that asserts what a non-blocking pass does INSIDE the io poll interval, judged on the
 *        cadence's own counter and replayed when the host stalled it past the interval (Huly QB-975).
 *
 * The cadence (Huly QB-191) skips the backend poll on a cold non-blocking pass while
 * `qb::tsc_ticks()` has advanced less than `io_poll_interval_ticks()` since the last poll. A case
 * that asserts "this pass did not poll" holds only if its passes really ran inside the interval, and
 * a loaded host can stall a case past it: a 3 ms sleep that overran a 50 ms interval on a macOS
 * runner made the pass legitimately poll and deliver. So such a case is an attempt that:
 *
 *   - runs on a fresh thread, so on a fresh thread-local listener: the cadence state (hot or cold,
 *     the last poll's stamp) outlives `listener::clear()`, and a case run on the test's own thread
 *     opened its interval wherever the previous case had left it;
 *   - reads the counter around the pass that stamps the interval (`Window`) and judges, after its
 *     last pass, whether every pass ran inside the interval (`ran_inside`), on the reading and the
 *     threshold the listener judges on, with a sixteenth of the interval spare;
 *   - asserts the inside-the-interval outcome only when the window held, and returns whether it
 *     did; `replay_until_conclusive` replays it otherwise, a bounded number of times. A stall spoils
 *     one attempt; a pass that never fits inside the interval -- one that blocks, or waits for the
 *     interval -- spoils every one, and fails the case.
 *
 * The due side waits on the same counter (`wait_until_due`) rather than sleeping a duration whose
 * clock the listener's calibration only approximates (Huly QB-925: judge on the mechanism's clock).
 * Shared by the io-cadence (QB-191) and the timer-gate (QB-190) tests.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#ifndef QB_IO_TESTS_SHARED_IO_CADENCE_WINDOW_H
#define QB_IO_TESTS_SHARED_IO_CADENCE_WINDOW_H

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/system/time.h>

namespace qb::io::test {

/// How many attempts a case makes before it reports that none ran inside its interval. A transient
/// stall of the host spoils one attempt, not twenty in a row.
inline constexpr int kCadenceAttempts = 20;

/// The counter read on each side of the pass that started an interval -- a hot pass or a due one,
/// both of which stamp the poll they make: `before` is no later than the listener's stamp, `after`
/// no earlier.
struct CadenceWindow {
    std::uint64_t before = 0;
    std::uint64_t after  = 0;
};

/// One non-blocking pass that stamps the interval's start, with the counter read around it.
inline CadenceWindow
stamping_pass() {
    CadenceWindow w;
    w.before = qb::tsc_ticks();
    qb::io::async::listener::current.run(EVRUN_NOWAIT);
    w.after = qb::tsc_ticks();
    return w;
}

/// Sets the interval and runs the first pass of this thread's fresh listener, which polls -- hot at
/// construction, and due anyway against a stamp of zero -- so the interval starts there; it finds
/// nothing and leaves the loop cold.
inline CadenceWindow
open_cold_interval(qb::duration const interval) {
    qb::io::async::listener::current.set_io_poll_interval(interval);
    return stamping_pass();
}

/// Whether every pass since `w` ran inside the interval, read after the last of them: the counter's
/// advance since `w.before` bounds what the listener measured from its stamp to its last judgement.
/// A sixteenth of the interval is kept spare, far above the few cycles an unserialised counter read
/// can move against the code around it; a counter read going backwards (a migration) is not inside.
inline bool
ran_inside(CadenceWindow const &w) {
    const std::uint64_t interval = qb::io::async::listener::current.io_poll_interval_ticks();
    const std::uint64_t now      = qb::tsc_ticks();
    return now >= w.before && now - w.before < interval - interval / 16;
}

/// Sleeps until the interval started at `w` has elapsed on the counter, a sixteenth spare: the next
/// non-blocking pass is due.
inline void
wait_until_due(CadenceWindow const &w) {
    const std::uint64_t interval = qb::io::async::listener::current.io_poll_interval_ticks();
    const std::uint64_t past     = interval + interval / 16;
    const auto          give_up  = qb::mono_now() + std::chrono::seconds{10};
    for (;;) {
        const std::uint64_t now = qb::tsc_ticks();
        if (now >= w.after && now - w.after >= past)
            return;
        if (qb::mono_now() > give_up) {
            ADD_FAILURE() << "the counter did not cover the interval in ten seconds";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

/// Runs `attempt` on a fresh thread, so on a fresh thread-local listener, and returns its result.
template <typename Attempt>
bool
on_fresh_thread(Attempt &attempt) {
    bool        result = false;
    std::thread t([&attempt, &result] {
        result = attempt();
        qb::io::async::listener::current.clear();
    });
    t.join();
    return result;
}

/// Replays `attempt` -- which returns whether its passes ran inside the interval, i.e. whether its
/// verdict counts -- on a fresh thread each time, until one did or one failed, at most
/// `kCadenceAttempts` times; then reports that none did.
template <typename Attempt>
void
replay_until_conclusive(Attempt attempt) {
    for (int i = 0; i < kCadenceAttempts; ++i)
        if (on_fresh_thread(attempt) || ::testing::Test::HasFailure())
            return;
    ADD_FAILURE() << "none of " << kCadenceAttempts
                  << " attempts ran its passes inside the interval, on the cadence's own counter: a stall of the host does not "
                     "cover that many, a pass that never fits inside the interval does";
}

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_IO_CADENCE_WINDOW_H
