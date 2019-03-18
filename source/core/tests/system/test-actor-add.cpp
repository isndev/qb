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

struct Tag {};

class TestServiceActor : public qb::ServiceActor<Tag>
{
    bool _ret_init;
public:
    TestServiceActor() = delete;
    explicit TestServiceActor(bool init)
            : _ret_init(init) {}

    virtual bool onInit() override final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        kill();
        return _ret_init;
    }
};

class TestActor : public qb::Actor
{
    bool _ret_init;
public:
    TestActor() = delete;
    explicit TestActor(bool init)
      : _ret_init(init) {}

    virtual bool onInit() override final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        kill();
        return _ret_init;
    }
};

class TestRefActor : public qb::Actor
{
    bool _ret_init;
public:
    TestRefActor() = delete;
    explicit TestRefActor(bool init)
            : _ret_init(init) {}

    virtual bool onInit() override final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        auto actor = addRefActor<TestActor>(_ret_init);

        kill();
        return actor != nullptr;
    }
};

TEST(AddActor, EngineShouldAbortIfActorFailedToInitAtStart) {
    qb::Main main({0});

    main.addActor<TestActor>(0, false);

    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddActor, ShouldReturnValidActorIdAtStart) {
    qb::Main main({0});

    auto id = main.addActor<TestServiceActor>(0, true);
    EXPECT_NE(static_cast<uint32_t>(id), 0u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddActor, ShouldReturnValidServiceActorIdAtStart) {
    qb::Main main({0});

    auto id = main.addActor<TestServiceActor>(0, true);
    EXPECT_EQ(static_cast<uint32_t>(id), 1u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddActorUsingCoreBuilder, ShouldNotAddActorOnBadCoreIndex) {
    qb::Main main({0});

    auto builder = main.core(1)
            .addActor<TestActor>(true);
    EXPECT_FALSE(static_cast<bool>(builder));
    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddActorUsingCoreBuilder, ShouldRetrieveValidOrderedActorIdList) {
    qb::Main main({0});

    auto builder = main.core(0)
            .addActor<TestServiceActor>(true)
            .addActor<TestActor>(true);
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
    qb::Main main({0});

    main.addActor<TestRefActor>(0, false);
    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddReferencedActor, ShouldReturnActorPtrOnSucess) {
    qb::Main main({0});

    main.addActor<TestRefActor>(0, true);
    main.start(false);
    EXPECT_FALSE(main.hasError());
}

class TestKillSenderActor : public qb::Actor
{
public:
    TestKillSenderActor() = default;

    virtual bool onInit() override final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        push<qb::KillEvent>(id());
        push<qb::KillEvent>(qb::BroadcastId(1));
        return true;
    }
};

class TestKillActor : public qb::Actor
{
public:
    TestKillActor() = default;

    virtual bool onInit() override final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        return true;
    }
};

TEST(KillActor, UsingEvent) {
    qb::Main main({0, 1});

    main.addActor<TestKillSenderActor>(0);
    auto builder = main.core(1);
    for (auto i = 0u; i < 1024; ++i)
        builder.addActor<TestKillActor>();
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}