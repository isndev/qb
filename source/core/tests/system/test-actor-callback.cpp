/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

struct UnregisterCallbackEvent : public qb::Event {};

class TestActor
        : public qb::Actor
        , public qb::ICallback
{
    const uint64_t _max_loop;
    uint64_t _count_loop;
public:
    TestActor() = delete;
    explicit TestActor(uint64_t const max_loop)
      : _max_loop(max_loop), _count_loop(0) {}

    ~TestActor() {
        if (_max_loop == 1000) {
            EXPECT_EQ(_count_loop, _max_loop);
        }
    }

    virtual bool onInit() override final {
        registerEvent<UnregisterCallbackEvent>(*this);
        if (_max_loop)
            registerCallback(*this);
        else
            kill();
        return true;
    }

    virtual void onCallback() override final {
        if (_max_loop == 10000)
            push<UnregisterCallbackEvent>(id());
        if (++_count_loop >= _max_loop)
            kill();
    }

    void on(UnregisterCallbackEvent &) {
        unregisterCallback(*this);
        push<qb::KillEvent>(id());
    }
};

TEST(CallbackActor, ShouldNotCallOnCallbackIfNotRegistred) {
    qb::Main main({0});

    main.addActor<TestActor>(0, 0);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(CallbackActor, ShouldCallOnCallbackIfRegistred) {
    qb::Main main({0});

    main.addActor<TestActor>(0, 1000);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(CallbackActor, ShouldNotCallOnCallbackAnymoreIfUnregistred) {
    qb::Main main({0});

    main.addActor<TestActor>(0, 1000);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}