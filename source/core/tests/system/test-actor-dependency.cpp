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
    qb::Main::CoreBuilder::ActorIdList const _ids;
public:
    explicit TestActorDependency(qb::Main::CoreBuilder::ActorIdList const &ids = {})
            : _ids(std::move(ids)) {}

    virtual bool onInit() override final {
        if (!_ids.size()) {
            registerEvent<qb::RequireEvent>(*this);
            require<TestActor>();
        } else {
            for (auto id : _ids)
                push<qb::KillEvent>(id);
            kill();
        }
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

TEST(ActorDependency, GetActorIdDependencyFromAddActorAtStart) {
    qb::Main main({0, 1});

    qb::Main::CoreBuilder::ActorIdList list;
    for (auto i = 0u; i < MAX_ACTOR; ++i) {
        list.push_back(main.addActor<TestActor>(0));
    }
    main.addActor<TestActorDependency>(1, list);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorDependency, GetActorIdDependencyFromCoreBuilderAtStart) {
    qb::Main main({0, 1});

    auto builder = main.core(0);
    for (auto i = 0u; i < MAX_ACTOR; ++i) {
        builder.addActor<TestActor>();
    }
    main.addActor<TestActorDependency>(1, builder.idList());

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

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