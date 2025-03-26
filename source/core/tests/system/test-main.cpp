/**
 * @file qb/core/tests/system/test-main.cpp
 * @brief Unit tests for Main class functionality
 * 
 * This file contains tests for the Main class in the QB Actor Framework,
 * which is responsible for initializing and managing the actor system.
 * It tests various scenarios including starting/stopping actors in mono-core
 * and multi-core environments, error handling, and signal handling.
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
#include <qb/main.h>
#include <qb/actor.h>
#include <csignal>

class TestActor : public qb::Actor {
    bool keep_live = false;
    bool throw_except = false;

public:
    TestActor() = default;
    explicit TestActor(bool live, bool except = false)
        : keep_live(live)
        , throw_except(except) {}
    bool
    onInit() final {
        if (throw_except)
            throw std::runtime_error("Test Exception Error");

        if (!keep_live)
            kill();
        registerEvent<qb::SignalEvent>(*this);
        return true;
    }

    void on(qb::SignalEvent const &event) {
        if (event.signum == SIGINT)
            kill();
        if (event.signum == SIGABRT)
            kill();
    }
};

TEST(Main, StartMonoCoreShouldAbortIfNoActor) {
    qb::Main main;

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMultiCoreShouldAbortIfNoActor) {
    const auto max_core = std::thread::hardware_concurrency();
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    const auto fail_core = std::rand() % max_core;

    EXPECT_GT(max_core, 1u);
    qb::Main main;

    for (auto i = 0u; i < max_core; ++i) {
        main.addActor<TestActor>(i);
    }

    main.core(fail_core).clear();
    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMonoCoreShouldAbortIfCoreHasThrownException) {
    qb::Main main;

    main.addActor<TestActor>(0, true, true);

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError());
}

TEST(Main, StartMonoCoreWithNoError) {
    qb::Main main;

    main.addActor<TestActor>(0);
    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StartMultiCoreWithNoError) {
    const auto max_core = std::thread::hardware_concurrency();

    EXPECT_GT(max_core, 1u);
    qb::Main main;

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i);

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StopMonoCoreWithNoError) {
    qb::Main main;

    main.addActor<TestActor>(0, true);
    main.start();
    main.stop();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StopMultiCoreWithNoError) {
    const auto max_core = std::thread::hardware_concurrency();

    EXPECT_GT(max_core, 1u);
    qb::Main main;

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i, true);

    main.start();
    main.stop();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Main, StopMultiCoreWithCustomSignal) {
    const auto max_core = std::thread::hardware_concurrency();

    EXPECT_GT(max_core, 1u);
    qb::Main main;

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i, true);

    qb::Main::registerSignal(SIGABRT);
    main.start();
    std::raise(SIGABRT);
    main.join();
    EXPECT_FALSE(main.hasError());
}