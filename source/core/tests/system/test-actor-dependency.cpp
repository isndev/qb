/**
 * @file qb/core/tests/system/test-actor-dependency.cpp
 * @brief Unit tests for actor dependency resolution
 *
 * This file contains tests for the actor dependency resolution mechanisms in the
 * QB Actor Framework. It verifies that actors can properly discover and communicate
 * with other actors through different dependency resolution approaches.
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

constexpr uint32_t MAX_ACTOR = 2048;

class TestActor : public qb::Actor {
public:
    TestActor() = default;
    bool
    onInit() final {
        return true;
    }
};

class TestActorDependency : public qb::Actor {
    qb::ActorIdList const _ids;

public:
    explicit TestActorDependency(qb::ActorIdList const &ids = {})
        : _ids(ids) {
        if (_ids.empty()) {
            registerEvent<qb::RequireEvent>(*this);
            require<TestActor>();
        } else {
            for (auto id : _ids)
                push<qb::KillEvent>(id);
            kill();
        }
    }

    uint32_t counter = 0;
    void
    on(qb::RequireEvent const &event) {
        if (is<TestActor>(event)) {
            ++counter;
            send<qb::KillEvent>(event.getSource());
        }
        if (counter == MAX_ACTOR)
            kill();
    }
};

TEST(ActorDependency, GetActorIdDependencyFromAddActorAtStart) {
    qb::Main main;

    qb::ActorIdList list;
    for (auto i = 0u; i < MAX_ACTOR; ++i) {
        list.push_back(main.addActor<TestActor>(0));
    }
    main.addActor<TestActorDependency>(1, list);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorDependency, GetActorIdDependencyFromCoreBuilderAtStart) {
    qb::Main main;

    auto builder = main.core(0).builder();
    for (auto i = 0u; i < MAX_ACTOR; ++i) {
        builder.addActor<TestActor>();
    }
    main.addActor<TestActorDependency>(1, builder.idList());

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorDependency, GetActorIdDependencyFromRequireEvent) {
    qb::Main main;

    auto builder = main.core(0).builder();
    for (auto i = 0u; i < MAX_ACTOR; ++i) {
        builder.addActor<TestActor>();
    }
    main.addActor<TestActorDependency>(1);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}