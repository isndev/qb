/**
 * @file qb/core/tests/system/test-actor-add.cpp
 * @brief Unit tests for actor creation and management
 *
 * This file contains tests for actor creation, initialization, and lifecycle management
 * in the QB Actor Framework. It tests adding actors to cores, service actor
 * registration, referenced actors, and actor termination via kill events.
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

struct Tag {};

class TestServiceActor : public qb::ServiceActor<Tag> {
    bool _ret_init;

public:
    TestServiceActor() = delete;
    explicit TestServiceActor(bool init)
        : _ret_init(init) {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        EXPECT_EQ(nullptr, getService<TestServiceActor>());
        kill();
    }

    bool
    onInit() final {
        EXPECT_EQ(this, getService<TestServiceActor>());
        return _ret_init;
    }
};

struct CheckServiceActor : public qb::Actor {
    CheckServiceActor() {
        EXPECT_NE(nullptr, getService<TestServiceActor>());
    }

    bool
    onInit() final {
        EXPECT_NE(nullptr, getService<TestServiceActor>());
        kill();
        return true;
    }
};

class TestActor : public qb::Actor {
    bool _ret_init;

public:
    TestActor() = delete;
    explicit TestActor(bool init)
        : _ret_init(init) {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        kill();
    }

    bool
    onInit() final {
        return _ret_init;
    }
};

class TestRefActor : public qb::Actor {
    bool _ret_init;

public:
    TestRefActor() = delete;
    explicit TestRefActor(bool init)
        : _ret_init(init) {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
    }

    bool
    onInit() final {
        auto actor = addRefActor<TestActor>(_ret_init);

        kill();
        return actor != nullptr;
    }
};

TEST(AddActor, EngineShouldAbortIfActorFailedToInitAtStart) {
    qb::Main main;

    main.addActor<TestActor>(0, false);

    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddActor, ShouldReturnValidActorIdAtStart) {
    qb::Main main;

    auto id = main.addActor<TestServiceActor>(0, true);
    main.addActor<CheckServiceActor>(0);
    EXPECT_NE(static_cast<uint32_t>(id), 0u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddActor, ShouldReturnValidServiceActorIdAtStart) {
    qb::Main main;

    auto id = main.addActor<TestServiceActor>(0, true);
    EXPECT_EQ(static_cast<uint32_t>(id), 1u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddActorUsingCoreBuilder, ShouldNotAddActorOnBadCoreIndex) {
    qb::Main main;

    EXPECT_THROW(main.core(256).addActor<TestActor>(true), std::range_error);
}

TEST(AddActorUsingCoreBuilder, ShouldNotAddActorWhenEngineIsRunning) {
    qb::Main main;

    main.core(0).addActor<TestActor>(true);
    main.start();
    EXPECT_THROW(main.core(0).addActor<TestActor>(true), std::runtime_error);
}

TEST(AddActorUsingCoreBuilder, ShouldRetrieveValidOrderedActorIdList) {
    qb::Main main;

    auto builder =
        main.core(0).builder().addActor<TestServiceActor>(true).addActor<TestActor>(
            true);
    EXPECT_TRUE(static_cast<bool>(builder));
    EXPECT_EQ(builder.idList().size(), 2u);
    EXPECT_EQ(static_cast<uint32_t>(builder.idList()[0]), 1u);
    EXPECT_NE(static_cast<uint32_t>(builder.idList()[1]), 0u);
    builder.addActor<TestServiceActor>(true);
    EXPECT_FALSE(static_cast<bool>(builder));
    EXPECT_EQ(builder.idList().size(), 3u);
    EXPECT_EQ(static_cast<uint32_t>(builder.idList()[2]), 0u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddReferencedActor, ShouldReturnNullptrIfActorFailedToInit) {
    qb::Main main;

    main.addActor<TestRefActor>(0, false);
    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddReferencedActor, ShouldReturnActorPtrOnSucess) {
    qb::Main main;

    main.addActor<TestRefActor>(0, true);
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

class TestKillSenderActor : public qb::Actor {
public:
    TestKillSenderActor() = default;

    bool
    onInit() final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        push<qb::KillEvent>(id());
        push<qb::KillEvent>(qb::BroadcastId(1));
        return true;
    }
};

class TestKillActor : public qb::Actor {
public:
    TestKillActor() = default;

    bool
    onInit() final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        return true;
    }
};

TEST(KillActor, UsingEvent) {
    qb::Main main;

    main.addActor<TestKillSenderActor>(0);
    auto builder = main.core(1).builder();
    for (auto i = 0u; i < 1024; ++i)
        builder.addActor<TestKillActor>();
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}