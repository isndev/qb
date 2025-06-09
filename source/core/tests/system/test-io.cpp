/**
 * @file qb/core/tests/system/test-io.cpp
 * @brief Unit tests for I/O functionality in the QB framework
 *
 * This file contains tests for I/O functionality in the QB framework, including
 * string operations, logging, and actor-based I/O in both mono-core and multi-core
 * environments.
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
#include <qb/string.h>
#include <qb/system/timestamp.h>

struct TestEvent : public qb::Event {};

class TestActor final : public qb::Actor {
public:
    TestActor() {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        LOG_DEBUG("TestActor had been constructed");
    }

    ~TestActor() final {
        LOG_CRIT("TestActor id dead");
    }

    bool
    onInit() final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        LOG_VERB("TestActor had been initialized at" << qb::Timestamp::nano());
        registerEvent<TestEvent>(*this);
        push<TestEvent>(id());
        qb::io::cout() << "Test Actor(" << id() << "): Hello master !" << std::endl;
        return true;
    }

    void
    on(TestEvent const &) {
        LOG_INFO("TestActor received TestEvent at" << qb::Timestamp::nano());
        kill();
        LOG_WARN("TestActor will be killed at" << qb::Timestamp::nano());
    }
};

TEST(IO, BasicTestMonoCore) {
    qb::io::log::init("./test-mono-io", 128);
    qb::io::log::setLevel(qb::io::log::Level::DEBUG);
    qb::Main main;

    LOG_INFO("Broadcast id=" << qb::BroadcastId(0));
    main.addActor<TestActor>(0);

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(IO, BasicTestMultiCore) {
    nanolog::initialize(nanolog::NonGuaranteedLogger(1), "./test.io", 128);
    nanolog::set_log_level(nanolog::LogLevel::VERBOSE);

    EXPECT_FALSE(nanolog::is_logged(nanolog::LogLevel::DEBUG));
    EXPECT_TRUE(nanolog::is_logged(nanolog::LogLevel::VERBOSE));

    const auto max_core = std::thread::hardware_concurrency();

    EXPECT_GT(max_core, 1u);
    qb::Main main;

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i);

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}