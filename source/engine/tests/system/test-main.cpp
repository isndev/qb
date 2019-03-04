#include <gtest/gtest.h>
#include <cube/actor.h>
#include <cube/main.h>

class TestActor : public cube::Actor
{
public:
    TestActor() = default;
    virtual bool onInit() override final {
      kill();
      return true;
    }
};

TEST(Main, StartMonoCoreShouldAbortIfNoActor) {
    cube::Main main({0});

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMultiCoreShouldAbortIfNoActor) {
    const auto max_core = std::thread::hardware_concurrency();
    std::srand(std::time(nullptr));
    const auto fail_core = std::rand() % max_core;

    EXPECT_GT(max_core, 1u);
    cube::Main main(cube::CoreSet::build(max_core));

    for (auto i = 0u; i < max_core; ++i) {
        if (i != fail_core)
            main.addActor<TestActor>(i);
    }

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMonoCoreWithNoError) {
    cube::Main main({0});

    main.addActor<TestActor>(0);
    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StartMultiCoreWithNoError) {
    const auto max_core = std::thread::hardware_concurrency();

    EXPECT_GT(max_core, 1u);
    cube::Main main(cube::CoreSet::build(max_core));

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i);

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}