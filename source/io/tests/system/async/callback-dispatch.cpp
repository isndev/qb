/**
 * @file system/async/callback-dispatch.cpp
 * @brief `qb::io::async::callback()` — immediate vs deferred dispatch on the event loop.
 *
 * `async::callback(func[, timeout])` (qb/io/async/io.h) is the everyday way to defer a callable onto
 * the qb-io loop. It has exactly two behaviours, both proven here against a real (socket-free) libev
 * loop, so this is a SYSTEM test (it needs `qb::io::async::init()` and pumps the loop, but opens no
 * descriptor):
 *
 *   - a non-positive timeout (`<= 0`) runs the callable SYNCHRONOUSLY, in-call, before `callback()`
 *     returns — no loop iteration is required;
 *   - a positive timeout schedules a self-deleting `Timeout<F>` that fires exactly once when the loop
 *     reaches the deadline, and not before.
 *
 * Restructured from the dissolved system/test-async-io.cpp (CallbackImmediateExecution,
 * CallbackScheduledExecution). The hand-rolled `for(i){run(EVRUN_ONCE);sleep_for()}` poll is replaced
 * by the shared deadline-bounded `qb::io::test::pump_until`, the deferred case additionally asserts the
 * callback does NOT fire before the loop runs (the inline/deferred distinction is the whole contract),
 * and a fires-exactly-once guard is added. No file-local main() (shared gtest_main).
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

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

class CallbackDispatchTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

} // namespace

// =============================================================================
// Immediate dispatch (non-positive timeout) — runs inline, no loop needed
// =============================================================================

TEST_F(CallbackDispatchTest, ZeroTimeoutRunsInline) {
    bool executed = false;
    // Zero timeout: the callable runs before callback() returns — observable
    // without ever calling async::run().
    async::callback([&executed]() { executed = true; }, qb::duration::zero());
    EXPECT_TRUE(executed) << "callback(zero) did not run inline";
}

TEST_F(CallbackDispatchTest, NoArgOverloadRunsInline) {
    bool executed = false;
    // The single-argument overload is unconditionally synchronous.
    async::callback([&executed]() { executed = true; });
    EXPECT_TRUE(executed) << "callback(func) did not run inline";
}

TEST_F(CallbackDispatchTest, NegativeTimeoutRunsInline) {
    bool executed = false;
    async::callback([&executed]() { executed = true; }, -5s);
    EXPECT_TRUE(executed) << "callback(negative) did not run inline";
}

// =============================================================================
// Deferred dispatch (positive timeout) — fires once via the loop, not before
// =============================================================================

TEST_F(CallbackDispatchTest, ScheduledCallbackFiresViaLoopAndNotBefore) {
    std::atomic<int> fired{0};
    async::callback([&fired]() { fired.fetch_add(1); }, 50ms);

    // The deferred callback must NOT have run synchronously inside callback().
    EXPECT_EQ(fired.load(), 0) << "a deferred callback fired before the loop ran";

    EXPECT_TRUE(pump_until([&] { return fired.load() == 1; })) << "deferred callback never fired";

    // And it must fire exactly once — pump further to prove no re-fire.
    EXPECT_FALSE(pump_until([&] { return fired.load() > 1; }, 100ms)) << "deferred callback fired more than once";
    EXPECT_EQ(fired.load(), 1);
}
