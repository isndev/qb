/**
 * @file qb/core/tests/system/test-type.cpp
 * @brief Unit tests for type characteristics in the QB framework
 *
 * This file contains tests that verify the size and characteristics of
 * various types in the QB framework. It ensures that critical types like
 * CoreId, ActorId, and EventId maintain their expected sizes, and that
 * event classes have the expected trivial destruction properties.
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
#include <qb/string.h>
#include <qb/system/timestamp.h>

struct TriviallyDestructibleEvent : public qb::Event {
    bool         b{};
    std::byte    by{};
    char         c{};
    double       d{};
    float        f{};
    char         e[10];
    std::size_t  s{};
    qb::string<> str{};
};

struct NonTriviallyDestructibleEvent : public TriviallyDestructibleEvent {
    std::vector<int> vec{};
};

TEST(TYPE, AllCheck) {
    std::cout << "-------- Constants --------" << std::endl;
    std::cout << "QB_LOCKFREE_CACHELINE_BYTES(" << QB_LOCKFREE_CACHELINE_BYTES << ")"
              << std::endl;
    std::cout << "QB_LOCKFREE_EVENT_BUCKET_BYTES(" << QB_LOCKFREE_EVENT_BUCKET_BYTES
              << ")" << std::endl;
    std::cout << "QB_MAX_EVENT_SIZE(" << std::numeric_limits<uint16_t>::max() << ")"
              << std::endl;
    std::cout << "--------   Types   --------" << std::endl;
    std::cout << "sizeof<std::size_t>(" << sizeof(std::size_t) << ")" << std::endl;
    std::cout << "sizeof<std::allocator>(" << sizeof(std::allocator<EventBucket>) << ")"
              << std::endl;
    std::cout << "sizeof<CoreId>(" << sizeof(qb::CoreId) << ")" << std::endl;
    EXPECT_EQ(sizeof(qb::CoreId), 2);
    std::cout << "sizeof<ActorId>(" << sizeof(qb::ActorId) << ")" << std::endl;
    EXPECT_EQ(sizeof(qb::ActorId), 4);
    std::cout << "sizeof<EventId>(" << sizeof(qb::EventId) << ")" << std::endl;
    EXPECT_EQ(sizeof(qb::EventId), 2);
    std::cout << "--------  Classes  --------" << std::endl;
    std::cout << "sizeof<Main>(" << sizeof(qb::Main) << ")" << std::endl;
    std::cout << "sizeof<VirtualCore>(" << sizeof(qb::VirtualCore) << ")" << std::endl;
    std::cout << "sizeof<VirtualPipe>(" << sizeof(qb::VirtualPipe) << ")" << std::endl;
    std::cout << "sizeof<Pipe>(" << sizeof(qb::Pipe) << ")" << std::endl;
    std::cout << "sizeof<Actor>(" << sizeof(qb::Actor) << ")" << std::endl;
    std::cout << "sizeof<EventQOS0>(" << sizeof(qb::EventQOS0) << ")" << std::endl;
    std::cout << "is_trivially_destructible<EventQOS0>("
              << std::is_trivially_destructible_v<qb::EventQOS0> << ")" << std::endl;
    EXPECT_EQ(std::is_trivially_destructible_v<qb::EventQOS0>, 1);
    std::cout << "sizeof<EventQOS1>(" << sizeof(qb::EventQOS1) << ")" << std::endl;
    std::cout << "is_trivially_destructible<EventQOS1>("
              << std::is_trivially_destructible_v<qb::EventQOS1> << ")" << std::endl;
    EXPECT_EQ(std::is_trivially_destructible_v<qb::EventQOS1>, 1);
    std::cout << "sizeof<EventQOS2>(" << sizeof(qb::EventQOS2) << ")" << std::endl;
    std::cout << "is_trivially_destructible<EventQOS2>("
              << std::is_trivially_destructible_v<qb::EventQOS2> << ")" << std::endl;
    EXPECT_EQ(std::is_trivially_destructible_v<qb::EventQOS0>, 1);
    std::cout << "is_trivially_destructible<TriviallyDestructibleEvent>("
              << std::is_trivially_destructible_v<TriviallyDestructibleEvent> << ")"
              << std::endl;
    EXPECT_EQ(std::is_trivially_destructible_v<TriviallyDestructibleEvent>, 1);
    std::cout << "is_trivially_destructible<NonTriviallyDestructibleEvent>("
              << std::is_trivially_destructible_v<NonTriviallyDestructibleEvent> << ")"
              << std::endl;
    EXPECT_EQ(std::is_trivially_destructible_v<NonTriviallyDestructibleEvent>, 0);
    std::cout << "sizeof<ServiceEvent>(" << sizeof(qb::ServiceEvent) << ")" << std::endl;
    //    EXPECT_EQ(sizeof(qb::ServiceEvent), 22);
}