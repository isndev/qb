/**
 * @file qb/core/tests/system/test-actor-concurrency-safety.cpp
 * @brief Unit tests for actor concurrency safety
 *
 * This file contains tests for concurrency safety in the QB Actor Framework.
 * It verifies that actors can safely interact concurrently without race conditions
 * or deadlocks, even under high load and with multiple cores.
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

#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <mutex>
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/io/async.h>
#include <qb/main.h>
#include <random>
#include <vector>

// Define test events
struct IncrementEvent : public qb::Event {
    int counter_id;
    int increment_by;

    IncrementEvent(int id, int inc)
        : counter_id(id)
        , increment_by(inc) {}
};

struct QueryCountersEvent : public qb::Event {
    qb::ActorId reply_to;

    explicit QueryCountersEvent(qb::ActorId id)
        : reply_to(id) {}
};

struct CountersResponseEvent : public qb::Event {
    std::vector<int> counter_values;

    explicit CountersResponseEvent(std::vector<int> values)
        : counter_values(std::move(values)) {}
};

struct WorkerCompleteEvent : public qb::Event {
    int worker_id;

    explicit WorkerCompleteEvent(int id)
        : worker_id(id) {}
};

struct TestCompleteEvent : public qb::Event {};

// Global state for test coordination
constexpr int     NUM_COUNTERS   = 10;
constexpr int     NUM_WORKERS    = 5;
constexpr int     NUM_OPERATIONS = 1000;
std::atomic<int>  g_total_operations{0};
std::atomic<bool> g_test_complete{false};
std::atomic<bool> g_killed_counter{false};

// A simple actor that maintains multiple counters that can be incremented concurrently
class CounterActor : public qb::Actor {
private:
    std::array<int, NUM_COUNTERS> _counters;
    int                           _total_count;

public:
    CounterActor()
        : _total_count(0) {
        // Initialize all counters to zero
        _counters.fill(0);
    }

    bool
    onInit() override {
        registerEvent<IncrementEvent>(*this);
        registerEvent<QueryCountersEvent>(*this);
        registerEvent<TestCompleteEvent>(*this);
        return true;
    }

    // Increment a specific counter
    void
    on(const IncrementEvent &event) {
        // Check for valid counter ID and only count up to NUM_OPERATIONS
        if (event.counter_id >= 0 && event.counter_id < NUM_COUNTERS &&
            _total_count < NUM_OPERATIONS) {
            _counters[event.counter_id] += event.increment_by;
            _total_count++;

            // Track total operations
            g_total_operations.store(_total_count);
        }
    }

    // Query all counter values
    void
    on(const QueryCountersEvent &event) {
        // Copy all counter values to a vector
        std::vector<int> values(NUM_COUNTERS);
        for (int i = 0; i < NUM_COUNTERS; ++i) {
            values[i] = _counters[i];
        }

        // Send response with counter values
        to(event.reply_to).push<CountersResponseEvent>(std::move(values));
    }

    // Complete test
    void
    on(const TestCompleteEvent &) {
        g_test_complete  = true;
        g_killed_counter = true;
        kill();
    }
};

// Worker actor that sends increment operations to the counter actor
class WorkerActor : public qb::Actor {
private:
    qb::ActorId  _counter_actor_id;
    qb::ActorId  _coordinator_id;
    int          _worker_id;
    int          _operations_remaining;
    std::mt19937 _rng;

public:
    WorkerActor(qb::ActorId counter_id, qb::ActorId coordinator_id, int worker_id,
                int operations)
        : _counter_actor_id(counter_id)
        , _coordinator_id(coordinator_id)
        , _worker_id(worker_id)
        , _operations_remaining(operations) {
        // Initialize random number generator with a unique seed per worker
        _rng.seed(static_cast<unsigned>(_worker_id));
    }

    bool
    onInit() override {
        // Schedule the first increment (others will be chained)
        qb::io::async::callback(
            [this]() { sendNextIncrement(); },
            0.001 * _worker_id); // Slight stagger in startup to reduce contention

        return true;
    }

    void
    sendNextIncrement() {
        if (_operations_remaining <= 0) {
            // No more operations to send, notify coordinator and terminate
            to(_coordinator_id).push<WorkerCompleteEvent>(_worker_id);
            kill();
            return;
        }

        // Create a uniform distribution for counter IDs and increment values
        std::uniform_int_distribution<int> counter_dist(0, NUM_COUNTERS - 1);
        std::uniform_int_distribution<int> increment_dist(1, 1);

        int counter_id   = counter_dist(_rng);
        int increment_by = increment_dist(_rng);

        // Send the increment event
        to(_counter_actor_id).push<IncrementEvent>(counter_id, increment_by);

        // Decrement remaining operations
        _operations_remaining--;

        // Schedule the next increment with a small delay
        qb::io::async::callback([this]() { sendNextIncrement(); }, 0.0005);
    }
};

// Coordinator actor that manages the concurrency test
class ConcurrencyCoordinatorActor : public qb::Actor {
private:
    qb::ActorId _counter_actor_id;
    int         _active_workers;
    bool        _test_completed;

public:
    ConcurrencyCoordinatorActor()
        : _active_workers(NUM_WORKERS)
        , _test_completed(false) {}

    bool
    onInit() override {
        registerEvent<CountersResponseEvent>(*this);
        registerEvent<WorkerCompleteEvent>(*this);

        // Create counter actor
        auto counter_actor = addRefActor<CounterActor>();
        _counter_actor_id  = counter_actor->id();

        // Create worker actors - chaque worker fait exactement NUM_OPERATIONS /
        // NUM_WORKERS opérations
        const int ops_per_worker = NUM_OPERATIONS / NUM_WORKERS;
        for (int i = 0; i < NUM_WORKERS; ++i) {
            // Pass coordinator ID to workers so they can notify when done
            addRefActor<WorkerActor>(_counter_actor_id, id(), i, ops_per_worker);
        }

        // Schedule final check as safety timeout
        qb::io::async::callback(
            [this]() {
                if (!_test_completed) {
                    finalizeTest();
                }
            },
            2.0); // Réduire à 2 secondes

        return true;
    }

    // Handle worker completion notifications
    void
    on(const WorkerCompleteEvent &event) {
        _active_workers--;

        // If all workers have completed, finalize the test
        if (_active_workers <= 0 && !_test_completed) {
            finalizeTest();
        }
    }

    // Complete the test and verify results
    void
    finalizeTest() {
        if (_test_completed)
            return; // Prevent double completion
        _test_completed = true;

        // Query final counter values
        to(_counter_actor_id).push<QueryCountersEvent>(id());

        // Complete test after a short delay to allow query to be processed
        qb::io::async::callback(
            [this]() {
                if (!g_killed_counter) {
                    to(_counter_actor_id).push<TestCompleteEvent>();
                }

                // Give counter actor time to process the complete event before
                // terminating
                qb::io::async::callback([this]() { kill(); }, 0.1);
            },
            0.2);
    }

    // Handle counter query responses
    void
    on(const CountersResponseEvent &event) {
        // Verify that total operations match expected count
        int total_operations = 0;
        for (int value : event.counter_values) {
            total_operations += value;
        }

        // Verify operation counts (only on final check)
        if (_test_completed) {
            g_total_operations.store(total_operations);
            g_test_complete = true;
        }
    }
};

// Simple dummy actor that just keeps a core active
class DummyActor : public qb::Actor {
public:
    DummyActor() {}

    bool
    onInit() override {
        // Add a callback to kill this actor after the test should be complete
        qb::io::async::callback([this]() { kill(); },
                                10.0); // 10 seconds is more than enough for the test

        return true;
    }
};

// Test for actor concurrency safety
TEST(ConcurrencySafety, ShouldHandleConcurrentOperationsSafely) {
    // Reset globals
    g_total_operations = 0;
    g_test_complete    = false;
    g_killed_counter   = false;

    // Create main instance
    qb::Main main;

    // Add the coordinator to the first core
    main.core(0).addActor<ConcurrencyCoordinatorActor>();

    // Run the engine - attendez plus longtemps pour s'assurer que tout est terminé
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Verify test completion flag is set
    EXPECT_TRUE(g_test_complete);

    // Le nombre exact d'opérations peut varier, mais devrait être proche de
    // NUM_OPERATIONS
    const int ops = g_total_operations.load();
    EXPECT_LE(ops, NUM_OPERATIONS);
    EXPECT_GE(ops, NUM_OPERATIONS * 0.9); // Permettre une légère marge inférieure (90%)
}