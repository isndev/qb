#include <gtest/gtest.h>
#include <cube/actor.h>
#include <cube/main.h>

class TestServiceActor : public cube::ServiceActor
{
    bool _ret_init;
public:
    TestServiceActor() = delete;
    explicit TestServiceActor(bool init)
            : cube::ServiceActor(1337), _ret_init(init) {}

    virtual bool onInit() override final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        kill();
        return _ret_init;
    }
};

class TestActor : public cube::Actor
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

class TestRefActor : public cube::Actor
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
    cube::Main main({0});

    main.addActor<TestActor>(0, false);

    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddActor, ShouldReturnValidActorIdAtStart) {
    cube::Main main({0});

    auto id = main.addActor<TestServiceActor>(0, true);
    EXPECT_NE(static_cast<uint32_t>(id), 0u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddActor, ShouldReturnValidServiceActorIdAtStart) {
    cube::Main main({0});

    auto id = main.addActor<TestServiceActor>(0, true);
    EXPECT_EQ(static_cast<uint32_t>(id), 1337u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddActorUsingCoreBuilder, ShouldNotAddActorOnBadCoreIndex) {
    cube::Main main({0});

    auto builder = main.core(1)
            .addActor<TestActor>(true);
    EXPECT_FALSE(static_cast<bool>(builder));
    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddActorUsingCoreBuilder, ShouldRetrieveValidOrderedActorIdList) {
    cube::Main main({0});

    auto builder = main.core(0)
            .addActor<TestServiceActor>(true)
            .addActor<TestActor>(true);
    EXPECT_TRUE(static_cast<bool>(builder));
    EXPECT_EQ(builder.idList().size(), 2u);
    EXPECT_EQ(static_cast<uint32_t>(builder.idList()[0]), 1337u);
    EXPECT_NE(static_cast<uint32_t>(builder.idList()[1]), 0u);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(AddReferencedActor, ShouldReturnNullptrIfActorFailedToInit) {
    cube::Main main({0});

    main.addActor<TestRefActor>(0, false);
    main.start(false);
    EXPECT_TRUE(main.hasError());
}

TEST(AddReferencedActor, ShouldReturnActorPtrOnSucess) {
    cube::Main main({0});

    main.addActor<TestRefActor>(0, true);
    main.start(false);
    EXPECT_FALSE(main.hasError());
}