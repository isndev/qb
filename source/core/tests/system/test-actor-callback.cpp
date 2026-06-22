/**
 * @file qb/core/tests/system/test-actor-callback.cpp
 * @brief Unit tests for actor callback functionality
 *
 * This file contains tests for the callback mechanism in the QB Actor Framework.
 * It verifies that actor callbacks are properly registered, executed, and unregistered.
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

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <atomic>
#include <cstdint>

class TestActor final
    : public qb::Actor
    , public qb::ICallback {
    const uint64_t _max_loop;
    uint64_t       _count_loop;

public:
    TestActor() = delete;
    explicit TestActor(uint64_t const max_loop)
        : _max_loop(max_loop)
        , _count_loop(0) {
        if (_max_loop)
            registerCallback(*this);
        else
            kill();
    }

    ~TestActor() final {
        if (_max_loop == 1000) {
            EXPECT_EQ(_count_loop, _max_loop);
        }
    }

    void
    on(qb::LoopEvent const &) final {
        if (_max_loop == 10000)
            unregisterCallback();
        if (++_count_loop >= _max_loop)
            kill();
    }
};

TEST(CallbackActor, ShouldNotCallOnCallbackIfNotRegistred) {
    qb::Main main;

    main.addActor<TestActor>(0, 0);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(CallbackActor, ShouldCallOnCallbackIfRegistred) {
    qb::Main main;

    main.addActor<TestActor>(0, 1000);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(CallbackActor, ShouldNotCallOnCallbackAnymoreIfUnregistred) {
    qb::Main main;

    main.addActor<TestActor>(0, 1000);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// LoopEvent payload: `now` equals Actor::time() for the pass (and is non-zero),
// and `iteration` is strictly monotonic across ticks.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_now_matches_time{true};
std::atomic<bool> g_now_nonzero{true};
std::atomic<bool> g_iteration_monotonic{true};
std::atomic<int>  g_loop_ticks{0};
} // namespace

class LoopEventActor final
    : public qb::Actor
    , public qb::ICallback {
    std::uint64_t _prev_iter = 0;
    bool          _first     = true;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &loop) final {
        if (loop.now != time())          // same cached timestamp as Actor::time()
            g_now_matches_time.store(false);
        if (loop.now == 0)
            g_now_nonzero.store(false);
        if (!_first && loop.iteration <= _prev_iter)
            g_iteration_monotonic.store(false);
        _first     = false;
        _prev_iter = loop.iteration;
        if (g_loop_ticks.fetch_add(1) + 1 >= 25) {
            unregisterCallback();
            kill();
        }
    }
};

TEST(CallbackActor, LoopEventCarriesLoopContext) {
    g_now_matches_time.store(true);
    g_now_nonzero.store(true);
    g_iteration_monotonic.store(true);
    g_loop_ticks.store(0);

    qb::Main main;
    main.addActor<LoopEventActor>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_GE(g_loop_ticks.load(), 25);
    EXPECT_TRUE(g_now_matches_time.load());     // LoopEvent.now == time()
    EXPECT_TRUE(g_now_nonzero.load());          // a real wall-clock timestamp
    EXPECT_TRUE(g_iteration_monotonic.load());  // iteration strictly increases per pass
}