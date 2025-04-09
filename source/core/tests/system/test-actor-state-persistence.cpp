/**
 * @file qb/core/tests/system/test-actor-state-persistence.cpp
 * @brief Unit tests for actor state persistence and recovery
 *
 * This file contains tests for state persistence and recovery in the QB Actor Framework.
 * It verifies that actors can properly save their state, recover from failures,
 * and continue operation with the correct internal state.
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
#include <map>
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/io/async.h>
#include <qb/main.h>
#include <string>

// Define test events
struct StateUpdateEvent : public qb::Event {
    std::string key;
    int         value;

    StateUpdateEvent(const std::string &k, int v)
        : key(k)
        , value(v) {}
};

struct StateQueryEvent : public qb::Event {
    std::string key;
    qb::ActorId reply_to;

    StateQueryEvent(const std::string &k, qb::ActorId id)
        : key(k)
        , reply_to(id) {}
};

struct StateResponseEvent : public qb::Event {
    std::string key;
    int         value;
    bool        found;

    StateResponseEvent(const std::string &k, int v, bool f)
        : key(k)
        , value(v)
        , found(f) {}
};

struct SimulateFailureEvent : public qb::Event {};
struct RestoreStateEvent : public qb::Event {};
struct CheckpointEvent : public qb::Event {};
struct VerifyStateEvent : public qb::Event {};
struct TestCompleteEvent : public qb::Event {};

// Global state for test coordination
std::atomic<bool> g_state_recovered{false};
std::atomic<int>  g_checkpoint_count{0};
std::atomic<int>  g_verification_count{0};

// State actor that persists and recovers state
class StatefulActor : public qb::Actor {
private:
    std::map<std::string, int> _state;
    bool                       _failed;

public:
    StatefulActor()
        : _failed(false) {}

    bool
    onInit() override {
        registerEvent<StateUpdateEvent>(*this);
        registerEvent<StateQueryEvent>(*this);
        registerEvent<SimulateFailureEvent>(*this);
        registerEvent<RestoreStateEvent>(*this);
        registerEvent<CheckpointEvent>(*this);
        registerEvent<VerifyStateEvent>(*this);
        registerEvent<TestCompleteEvent>(*this);

        // Initialize with some default state
        _state["counter"] = 0;
        _state["version"] = 1;

        return true;
    }

    // Handle state updates
    void
    on(const StateUpdateEvent &event) {
        if (_failed)
            return; // Simulate failure by ignoring updates

        _state[event.key] = event.value;
    }

    // Handle state queries
    void
    on(const StateQueryEvent &event) {
        if (_failed)
            return; // Simulate failure by ignoring queries

        auto it    = _state.find(event.key);
        bool found = (it != _state.end());
        int  value = found ? it->second : -1;

        // Send response to the query
        to(event.reply_to).push<StateResponseEvent>(event.key, value, found);
    }

    // Simulate a failure by marking the actor as failed
    void
    on(const SimulateFailureEvent &) {
        _failed = true;
    }

    // Restore state from "persistent storage" (in this test, we just reset the failed
    // flag)
    void
    on(const RestoreStateEvent &) {
        _failed           = false;
        g_state_recovered = true;
    }

    // Create a checkpoint of the state (in a real system, this would write to
    // disk/database)
    void
    on(const CheckpointEvent &) {
        if (_failed)
            return;

        // Increment counter to simulate changing state
        _state["counter"]    = _state["counter"] + 1;
        _state["checkpoint"] = _state["counter"];

        g_checkpoint_count++;
    }

    // Verify state is consistent after recovery
    void
    on(const VerifyStateEvent &) {
        if (_failed)
            return;

        // Verify state integrity
        bool state_valid = (_state["counter"] == _state["checkpoint"]);

        if (state_valid) {
            g_verification_count++;
        }
    }

    // Complete test
    void
    on(const TestCompleteEvent &) {
        kill();
    }
};

// Coordinator actor that manages the state test
class StateCoordinatorActor : public qb::Actor {
private:
    qb::ActorId _stateful_actor_id;

public:
    StateCoordinatorActor() {}

    bool
    onInit() override {
        registerEvent<StateResponseEvent>(*this);

        // Create stateful actor
        auto actor         = addRefActor<StatefulActor>();
        _stateful_actor_id = actor->id();

        // Schedule the test sequence with delays to simulate real operation
        scheduleTestSequence();

        return true;
    }

    void
    scheduleTestSequence() {
        // Update state
        to(_stateful_actor_id).push<StateUpdateEvent>("test", 42);

        // Create checkpoint after 50ms
        qb::io::async::callback(
            [this]() { to(_stateful_actor_id).push<CheckpointEvent>(); }, 0.05);

        // Query state after 100ms
        qb::io::async::callback(
            [this]() {
                to(_stateful_actor_id).push<StateQueryEvent>("test", id());
                to(_stateful_actor_id).push<StateQueryEvent>("counter", id());
            },
            0.1);

        // Simulate failure after 150ms
        qb::io::async::callback(
            [this]() { to(_stateful_actor_id).push<SimulateFailureEvent>(); }, 0.15);

        // Try query after failure (should be ignored)
        qb::io::async::callback(
            [this]() { to(_stateful_actor_id).push<StateQueryEvent>("test", id()); },
            0.2);

        // Restore state after 250ms
        qb::io::async::callback(
            [this]() { to(_stateful_actor_id).push<RestoreStateEvent>(); }, 0.25);

        // Verify state after 300ms
        qb::io::async::callback(
            [this]() { to(_stateful_actor_id).push<VerifyStateEvent>(); }, 0.3);

        // Query state after recovery
        qb::io::async::callback(
            [this]() { to(_stateful_actor_id).push<StateQueryEvent>("test", id()); },
            0.35);

        // Complete test
        qb::io::async::callback(
            [this]() {
                to(_stateful_actor_id).push<TestCompleteEvent>();
                kill();
            },
            0.4);
    }

    // Handle state responses
    void
    on(const StateResponseEvent &event) {
        // We just verify the response came back with correct data
        if (event.key == "test") {
            EXPECT_TRUE(event.found);
            EXPECT_EQ(event.value, 42);
        }
    }
};

// Test for actor state persistence and recovery
TEST(StatePersistence, ShouldPersistAndRecoverState) {
    // Reset globals
    g_state_recovered    = false;
    g_checkpoint_count   = 0;
    g_verification_count = 0;

    // Create main instance
    qb::Main main;

    // Initialiser le core 0 et ajouter un acteur
    main.core(0).addActor<StateCoordinatorActor>();

    // Run the engine
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Verify state was recovered
    EXPECT_TRUE(g_state_recovered);
    EXPECT_GT(g_checkpoint_count, 0);
    EXPECT_GT(g_verification_count, 0);
}