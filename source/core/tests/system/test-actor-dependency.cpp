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

constexpr uint32_t MAX_ACTOR = 2048;

class TestActor : public qb::Actor
{
public:
    TestActor() = default;
    virtual bool onInit() override final {
        return true;
    }
};

class TestActorDependency
        : public qb::Actor
{
public:
    TestActorDependency() = default;

    virtual bool onInit() override final {
        registerEvent<qb::RequireEvent>(*this);
        require<TestActor>();
        return true;
    }

    uint32_t counter = 0;
    void on(qb::RequireEvent const &event) {
        if (is<TestActor>(event)) {
            ++counter;
            send<qb::KillEvent>(event.getSource());
        }
        if (counter == MAX_ACTOR)
            kill();
    }
};

class TestActorReverse : public qb::Actor {
    uint32_t counter = 0;
public:
    virtual bool onInit() override final {
        // overload ping
        registerEvent<qb::PingEvent>(*this);
        return true;
    }

    void on(qb::PingEvent const &event) {
        if (is<TestActorReverse>(event.type)) {
            send<qb::RequireEvent>(event.getSource(), event.type, qb::ActorStatus::Alive);
            ++counter;
        }
        if (counter == MAX_ACTOR)
            kill();
    }
};

class TestActorReverseDependency : public qb::Actor {
public:
    virtual bool onInit() override final {
        registerEvent<qb::RequireEvent>(*this);
        require<TestActorReverse>();
        return true;
    }

    void on(qb::RequireEvent const &) {
        kill();
    }
};

TEST(ActorDependency, GetActorIdDependencyFromRequireEvent) {
    qb::Main main({0, 1});

    auto builder = main.core(0);
    for (auto i = 0u; i < MAX_ACTOR; ++i) {
        builder.addActor<TestActor>();
    }
    main.addActor<TestActorDependency>(1);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorDependency, GetActorIdReverseDependencyFromRequireEvent) {
    qb::Main main({0, 1});

    auto builder = main.core(0);
    for (auto i = 0u; i < MAX_ACTOR; ++i) {
        builder.addActor<TestActorReverseDependency>();
    }
    main.addActor<TestActorReverse>(1);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}