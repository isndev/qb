#include <gtest/gtest.h>
#include <cube/actor.h>
#include <cube/main.h>

class TestActor : public qb::Actor
{
    bool keep_live = false;
public:
    TestActor() = default;
    TestActor(bool live) : keep_live(live) {}
    virtual bool onInit() override final {
        if (!keep_live)
            kill();
        return true;
    }
};

TEST(Main, StartMonoCoreShouldAbortIfNoActor) {
    qb::Main main({0});

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMultiCoreShouldAbortIfNoActor) {
    const auto max_core = std::thread::hardware_concurrency();
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    const auto fail_core = std::rand() % max_core;

    EXPECT_GT(max_core, 1u);
    qb::Main main(qb::CoreSet::build(max_core));

    for (auto i = 0u; i < max_core; ++i) {
        if (i != fail_core)
            main.addActor<TestActor>(i);
    }

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMonoCoreWithNoError) {
    qb::Main main({0});

    main.addActor<TestActor>(0);
    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StartMultiCoreWithNoError) {
    const auto max_core = std::thread::hardware_concurrency();

    EXPECT_GT(max_core, 1u);
    qb::Main main(qb::CoreSet::build(max_core));

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i);

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StopMonoCoreWithNoError) {
    qb::Main main({0});

    main.addActor<TestActor>(0, true);
    main.start();
    main.stop();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StopMultiCoreWithNoError) {
    const auto max_core = std::thread::hardware_concurrency();

    EXPECT_GT(max_core, 1u);
    qb::Main main(qb::CoreSet::build(max_core));

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i, true);

    main.start();
    main.stop();
    main.join();
    EXPECT_FALSE(main.hasError());
}