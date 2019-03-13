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

class TestActor : public qb::Actor
{
    bool keep_live = false;
    bool throw_except;
public:
    TestActor() = default;
    TestActor(bool live, bool except = false) : keep_live(live), throw_except(except) {}
    virtual bool onInit() override final {
        if (throw_except)
            throw std::runtime_error("Test Exception Error");

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

TEST(Main, StartMonoCoreShouldAbortIfNoExistingCore) {
    qb::Main main({255});

    main.addActor<TestActor>(255, true);

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMonoCoreShouldAbortIfCoreHasThrownException) {
    qb::Main main({0});

    main.addActor<TestActor>(0, true, true);

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