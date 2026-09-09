/**
 * @file unit/coroutine/scheduler-abandoned-frames.cpp
 * @brief A scheduler destroyed with a frame still suspended REPORTS it, in every build: the
 *        process-wide tally moves by exactly that frame, and a clean teardown moves it by nothing
 *        (Huly QB-84).
 *
 * `~CoroutineScheduler` deliberately abandons suspended frames rather than destroying them (their
 * watchers still reference them; see the destructor's decision log). Until 3.2 the only trace of
 * it was a debug-only `fprintf`, so a release build accumulated frames in silence. The report now
 * goes through `report_abandoned_coroutine_frames`, which adds to
 * `abandoned_coroutine_frames_total()` -- the number this file reads back in both polarities.
 *
 * WHICH LIFECYCLE REACHES IT. The listener's own teardown (`~listener`, `reset_coro_scheduler()`)
 * runs `destroy_all_suspended()` BEFORE the destructor, so a frame parked on the listener's
 * scheduler is destroyed, not abandoned -- pinned here too, so the report is never mistaken
 * for that path. The abandon path is a `CoroutineScheduler` owned directly (the class is
 * public) and destroyed while a frame is still suspended on it: that is what the cases below
 * build, with `CoroutineScheduler::set_current()` naming it the thread's scheduler.
 *
 * The abandoned frame is NOT leaked by the test: the coroutine parks on an awaiter that registers
 * it as suspended (exactly what a watcher awaiter does) and the test keeps the `task` that owns
 * the frame, so the frame is destroyed by that task after the scheduler is gone -- a path that
 * scrubs no scheduler (`forget_frame_if_current` finds none) and stops no watcher (the awaiter
 * holds none).
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
 * @ingroup Tests
 */

#include <chrono>
#include <coroutine>
#include <cstddef>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::reset_async_context;

namespace scheduler_abandoned_frames_test {

/// Parks the coroutine for ever, registered as SUSPENDED with the current scheduler -- the
/// bookkeeping a watcher awaiter does on suspend -- and holding no watcher, so destroying the
/// frame later touches no loop.
struct park_registered {
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> h) const noexcept {
        CoroutineScheduler::current_ptr()->register_suspended(h);
    }
    void
    await_resume() const noexcept {}
};

task<void>
park_for_ever() {
    co_await park_registered{};
}

task<void>
finish_at_once() {
    co_return;
}

class SchedulerAbandonedFrames : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        if (listener::current.has_coro_scheduler()) {
            run_for(5ms);
            listener::current.reset_coro_scheduler();
        }
        listener::current.clear();
    }
};

} // namespace scheduler_abandoned_frames_test

using namespace scheduler_abandoned_frames_test;

/**
 * @test One frame parked at teardown of a directly-owned scheduler: the tally moves by exactly
 *       one, and the frame is then destroyed by the task that owns it (no leak, no double free).
 */
TEST_F(SchedulerAbandonedFrames, OneSuspendedFrameIsCountedOnce) {
    const std::size_t before = abandoned_coroutine_frames_total();
    {
        task<void> parked = park_for_ever();
        {
            CoroutineScheduler sched; // owned here, not by the listener
            CoroutineScheduler::set_current(&sched);
            parked.handle().resume(); // runs to the park: registered as suspended with `sched`
            EXPECT_EQ(sched.active_count(), 1u) << "the parked frame must be counted as suspended";
        } // ~CoroutineScheduler with one suspended frame: abandoned, reported
        CoroutineScheduler::set_current(nullptr);
        EXPECT_EQ(abandoned_coroutine_frames_total() - before, 1u) << "one abandoned frame must be reported once";
        // `parked` goes out of scope here: its frame is destroyed by the task, after the scheduler.
    }
    EXPECT_EQ(abandoned_coroutine_frames_total() - before, 1u) << "destroying the frame afterwards must not report again";
}

/**
 * @test A clean teardown -- every frame completed -- reports nothing.
 */
TEST_F(SchedulerAbandonedFrames, ACleanTeardownReportsNothing) {
    const std::size_t before = abandoned_coroutine_frames_total();
    {
        task<void> done = finish_at_once();
        {
            CoroutineScheduler sched;
            CoroutineScheduler::set_current(&sched);
            done.handle().resume(); // completes: never suspended
            EXPECT_TRUE(done.handle().done());
        }
        CoroutineScheduler::set_current(nullptr);
    }
    EXPECT_EQ(abandoned_coroutine_frames_total(), before) << "a teardown with nothing suspended must not move the tally";
}

/**
 * @test The tally is cumulative across schedulers: two teardowns, one frame each, add two.
 */
TEST_F(SchedulerAbandonedFrames, TheTallyAccumulatesAcrossTeardowns) {
    const std::size_t before = abandoned_coroutine_frames_total();
    for (int round = 0; round < 2; ++round) {
        task<void> parked = park_for_ever();
        {
            CoroutineScheduler sched;
            CoroutineScheduler::set_current(&sched);
            parked.handle().resume();
        }
        CoroutineScheduler::set_current(nullptr);
    }
    EXPECT_EQ(abandoned_coroutine_frames_total() - before, 2u);
}

/**
 * @test The listener's teardown DESTROYS a parked frame (`destroy_all_suspended` runs before the
 *       destructor), so it reports nothing: the report is for a directly-owned scheduler, never
 *       for the lifecycle the framework drives itself.
 */
TEST_F(SchedulerAbandonedFrames, ListenerTeardownDestroysRatherThanAbandons) {
    const std::size_t before = abandoned_coroutine_frames_total();
    coro_scheduler().spawn(park_for_ever()); // owned by the listener's scheduler
    run_for(1ms);                            // the spawned frame runs to its park
    EXPECT_EQ(coro_scheduler().active_count(), 1u);
    listener::current.reset_coro_scheduler(); // destroy_all_suspended, then ~CoroutineScheduler
    EXPECT_EQ(abandoned_coroutine_frames_total(), before) << "a frame the teardown destroyed is not an abandoned one";
}
