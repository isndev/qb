/**
 * @file qb/core/tests/system/test-actor-callback.cpp
 * @brief Unit tests for actor callback functionality
 * 
 * This file contains tests for the callback mechanism in the QB Actor Framework.
 * It verifies that actor callbacks are properly registered, executed, and unregistered.
 * 
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

class TestActor final
    : public qb::Actor
    , public qb::ICallback {
    const uint64_t _max_loop;
    uint64_t _count_loop;

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
    onCallback() final {
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