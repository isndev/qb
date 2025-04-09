/**
 * @file qb/core/tests/system/test-actor-broadcast.cpp
 * @brief Unit tests for actor broadcast communication
 *
 * This file contains tests for the broadcast communication mechanism in the QB Actor
 * Framework. It verifies that broadcast events are properly distributed to multiple
 * actors.
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

#include <atomic>
#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

// Define test events
struct BroadcastTestEvent : public qb::Event {
    int value;
    explicit BroadcastTestEvent(int val)
        : value(val) {}
};

struct EndTestEvent : public qb::Event {};

// Global counter to track received broadcasts across actors
std::atomic<int> g_received_count{0};
std::atomic<int> g_value_sum{0};

// Receiver actor that processes broadcast messages
class ReceiverActor : public qb::Actor {
public:
    bool
    onInit() override {
        // Register for broadcast events
        registerEvent<BroadcastTestEvent>(*this);
        registerEvent<EndTestEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    // Handler for broadcast events
    void
    on(const BroadcastTestEvent &event) {
        // Increment global counters
        g_received_count++;
        g_value_sum += event.value;
    }

    // Handler for end test signal
    void
    on(const EndTestEvent &) {
        // End the test by killing itself
        kill();
    }

    // Handler for KillEvent
    void
    on(const qb::KillEvent &) {
        kill();
    }
};

// Broadcaster actor that sends broadcasts
class BroadcasterActor : public qb::Actor {
    int _num_broadcasts;

public:
    BroadcasterActor(int num_receivers, int num_broadcasts)
        : _num_broadcasts(num_broadcasts) {}

    bool
    onInit() override {
        registerEvent<qb::KillEvent>(*this);

        // Send broadcast messages
        for (int i = 1; i <= _num_broadcasts; ++i) {
            broadcast<BroadcastTestEvent>(i);
        }

        // Send end test signal to all receivers
        broadcast<EndTestEvent>();

        // Kill self after broadcasting
        kill();

        return true;
    }
};

TEST(BroadcastActor, ShouldReceiveBroadcastsByAllReceivers) {
    // Reset global counters
    g_received_count = 0;
    g_value_sum      = 0;

    // Test parameters
    const int num_receivers  = 5;
    const int num_broadcasts = 10;

    // Create main instance
    qb::Main main;

    // Add receiver actors
    for (int i = 0; i < num_receivers; ++i) {
        main.addActor<ReceiverActor>(0);
    }

    // Add broadcaster actor
    main.addActor<BroadcasterActor>(0, num_receivers, num_broadcasts);

    // Start and run engine
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Verify that all receivers got all broadcasts
    EXPECT_EQ(g_received_count, num_receivers * num_broadcasts);

    // Verify the sum of values received (sum of 1..num_broadcasts * num_receivers)
    int expected_sum = num_receivers * (num_broadcasts * (num_broadcasts + 1)) / 2;
    EXPECT_EQ(g_value_sum, expected_sum);
}

TEST(BroadcastActor, ShouldHandleZeroBroadcasts) {
    // Reset global counters
    g_received_count = 0;
    g_value_sum      = 0;

    // Test with no broadcasts
    const int num_receivers  = 3;
    const int num_broadcasts = 0;

    // Create main instance
    qb::Main main;

    // Add receiver actors
    for (int i = 0; i < num_receivers; ++i) {
        main.addActor<ReceiverActor>(0);
    }

    // Add broadcaster with zero broadcasts
    main.addActor<BroadcasterActor>(0, num_receivers, num_broadcasts);

    // Start and run engine
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Verify no broadcasts were received
    EXPECT_EQ(g_received_count, 0);
    EXPECT_EQ(g_value_sum, 0);
}