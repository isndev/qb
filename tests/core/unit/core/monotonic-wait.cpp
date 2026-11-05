/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/monotonic-wait.cpp
 * @brief `qb::detail::wait_for_monotonic` (Huly QB-1227): the bounded condition-variable wait behind a core's mailbox
 *        park and TestKit's watchdog, held to `std::condition_variable::wait_for`'s contract on every platform.
 */

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <qb/core/detail/monotonic_wait.h>

using namespace std::chrono_literals;

namespace monotonic_wait_test {

/// What one wait returned, and how long it took on the monotonic clock.
struct Waited {
    bool         result = false;
    qb::duration took{};
};

/// Waits with `timeout` for a flag set from another thread after `notify_after` (never, if negative), or set at once.
[[nodiscard]] inline Waited
wait_with(qb::duration timeout, bool ready_at_start, qb::duration notify_after,
          qb::detail::past_range beyond = qb::detail::past_range::no_end) {
    std::mutex              m;
    std::condition_variable cv;
    bool                    ready = ready_at_start;
    auto const              t0    = qb::mono_now(); // before the notifier starts: its delay is then a lower bound of the wait
    std::thread             notifier;
    if (notify_after >= qb::duration::zero())
        notifier = std::thread([&] {
            std::this_thread::sleep_for(notify_after);
            {
                std::lock_guard g(m);
                ready = true;
            }
            cv.notify_one();
        });
    Waited w;
    {
        std::unique_lock lk(m);
        w.result = qb::detail::wait_for_monotonic(cv, lk, timeout, [&] { return ready; }, beyond);
        w.took   = qb::mono_now() - t0;
    }
    if (notifier.joinable())
        notifier.join();
    return w;
}

} // namespace monotonic_wait_test

TEST(MonotonicWait, APredicateTrueOnEntryReturnsWithoutWaiting) {
    using namespace monotonic_wait_test;
    auto const w = wait_with(10s, true, -1ms);
    EXPECT_TRUE(w.result);
    EXPECT_LT(w.took, 5s) << "the wait blocked before checking a predicate already true";
}

TEST(MonotonicWait, ANonPositiveTimeoutReturnsThePredicateWithoutBlocking) {
    using namespace monotonic_wait_test;
    for (auto const timeout : {qb::duration::zero(), qb::duration{-1s}}) {
        auto const w = wait_with(timeout, false, -1ms);
        EXPECT_FALSE(w.result);
        EXPECT_LT(w.took, 5s);
    }
}

TEST(MonotonicWait, ATimeoutEndsTheWaitNoEarlierThanItsSpan) {
    using namespace monotonic_wait_test;
    auto const w = wait_with(50ms, false, -1ms);
    EXPECT_FALSE(w.result) << "nobody signalled: the wait must end on its timeout, the predicate false";
    EXPECT_GE(w.took, 50ms) << "the wait ended early";
}

TEST(MonotonicWait, ANotifyEndsTheWaitBeforeItsTimeout) {
    using namespace monotonic_wait_test;
    auto const w = wait_with(10s, false, 20ms);
    EXPECT_TRUE(w.result);
    EXPECT_GE(w.took, 20ms);
    EXPECT_LT(w.took, 5s) << "a notify did not end the wait";
}

TEST(MonotonicWait, ADurationMaxTimeoutWaitsUntilNotified) {
    // No end: `qb::duration::max()` must not overflow `now + timeout` into the past (an immediate timeout).
    using namespace monotonic_wait_test;
    auto const w = wait_with(qb::duration::max(), false, 50ms);
    EXPECT_TRUE(w.result) << "a wait of duration::max() ended without its predicate";
    EXPECT_GE(w.took, 50ms);
}

TEST(MonotonicWait, APastRangeTimeoutReturnsAtOnceWhenAsked) {
    // The answer a caller that must not wait without end asks for (a core's park never reaches it: a second at most).
    using namespace monotonic_wait_test;
    auto const w = wait_with(qb::duration::max(), false, -1ms, qb::detail::past_range::returns_at_once);
    EXPECT_FALSE(w.result);
    EXPECT_LT(w.took, 5s) << "a past-range timeout asked to return at once waited";
}
