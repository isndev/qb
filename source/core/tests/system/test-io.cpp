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
#include <qb/system/timestamp.h>
#include <qb/string.h>

struct TestEvent : public qb::Event {};

class TestActor : public qb::Actor
{
public:
    TestActor() {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        LOG_DEBUG("TestActor had been constructed");
    }

    ~TestActor() {
        LOG_CRIT("TestActor id dead");
    }

    virtual bool onInit() override final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        LOG_VERB("TestActor had been initialized at" << qb::Timestamp::nano());
        registerEvent<TestEvent>(*this);
        push<TestEvent>(id());
        qb::io::cout() << "Test Actor(" << id() << "): Hello master !" << std::endl;
        return true;
    }

    void on(TestEvent const &) {
        LOG_INFO("TestActor received TestEvent at" << qb::Timestamp::nano());
        kill();
        LOG_WARN("TestActor will be killed at" << qb::Timestamp::nano());
    }
};

TEST(IO, STRING_TEST) {
    const char c_str[] = "0123456789012345678901234567890123456789";
    std::string std_str(c_str);

    qb::string qb_str1(c_str);
    qb::string qb_str2(std_str);
    qb::string<40> qb_str3(c_str);
    qb::string<40> qb_str4(qb_str1);

    EXPECT_EQ(qb_str1.size(), 30u);
    EXPECT_EQ(qb_str2.size(), 30u);
    EXPECT_EQ(qb_str3.size(), 40u);
    EXPECT_TRUE(qb_str1 == qb_str2);
    EXPECT_FALSE(qb_str1 == std_str);
    EXPECT_FALSE(qb_str1 == c_str);
    EXPECT_FALSE(qb_str3 == qb_str2);
    EXPECT_TRUE(qb_str3 == std_str);

    std::sort(qb_str4.begin(), qb_str4.end());
    std::string tmp;
    for (auto c : qb_str4) {
        tmp += c;
    }
    EXPECT_TRUE(tmp == "000111222333444555666777888999");
    tmp.clear();
    for (auto it = qb_str4.crbegin(); it != qb_str4.crend(); ++it) {
        tmp += *it;
    }
    EXPECT_TRUE(tmp == "999888777666555444333222111000");
    std::cout << std::endl;
    qb_str1 = qb_str2 = qb_str3 = qb_str4 = "end";
    EXPECT_TRUE(qb_str1.size() == qb_str2.size());
    EXPECT_TRUE(qb_str2.size() == qb_str3.size());
    EXPECT_TRUE(qb_str3.size() == qb_str4.size());
    EXPECT_TRUE(qb_str1 == qb_str2);
    EXPECT_TRUE(qb_str2 == qb_str3);
    EXPECT_TRUE(qb_str3 == qb_str4);
}

TEST(IO, BasicTestMonoCore) {
    qb::io::log::init("./test-mono-io", 128);
    qb::io::log::setLevel(qb::io::log::Level::DEBUG);
    qb::Main main({0});

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
    qb::Main main(qb::CoreSet::build(max_core));

    for (auto i = 0u; i < max_core; ++i)
        main.addActor<TestActor>(i);

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}