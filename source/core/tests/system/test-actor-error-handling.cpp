/**
 * @file qb/core/tests/system/test-actor-error-handling.cpp
 * @brief Unit tests for actor error handling and resilience
 *
 * This file contains tests for error handling in the QB Actor Framework.
 * It verifies that actors can properly detect, handle, and recover from
 * various error conditions while maintaining system stability.
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
#include <qb/io.h>
#include <qb/io/async.h>
#include <qb/main.h>
#include <stdexcept>

// Define test events
struct ErrorInducingEvent : public qb::Event {
    enum class ErrorType { None, ThrowException, InvalidOperation, SendToInvalidActor };

    ErrorType error_type;
    explicit ErrorInducingEvent(ErrorType type)
        : error_type(type) {}
};

struct MonitorEvent : public qb::Event {
    qb::ActorId target_id;
    explicit MonitorEvent(qb::ActorId id)
        : target_id(id) {}
};

struct StatusEvent : public qb::Event {
    bool is_alive;
    explicit StatusEvent(bool alive)
        : is_alive(alive) {}
};

// Coordinator to track overall test status
std::atomic<bool> g_error_detected{false};
std::atomic<bool> g_recovery_successful{false};
std::atomic<int>  g_error_actors_terminated{0};

// Error-inducing actor that will deliberately cause errors
class ErrorActor : public qb::Actor {
    bool _should_recover;

public:
    ErrorActor(int id, bool should_recover)
        : _should_recover(should_recover) {}

    bool
    onInit() override {
        registerEvent<ErrorInducingEvent>(*this);
        registerEvent<MonitorEvent>(*this);
        return true;
    }

    void
    on(const ErrorInducingEvent &event) {
        // Handle different types of errors
        switch (event.error_type) {
            case ErrorInducingEvent::ErrorType::ThrowException:
                // Simulating an exception
                // In real code this would cause the actor to crash
                g_error_detected = true;

                // Simulate recovery if needed
                if (_should_recover) {
                    g_recovery_successful = true;
                } else {
                    g_error_actors_terminated++;
                    kill();
                }
                break;

            case ErrorInducingEvent::ErrorType::InvalidOperation:
                // Simulate an invalid operation
                g_error_detected = true;

                // Simulate recovery if needed
                if (_should_recover) {
                    g_recovery_successful = true;
                } else {
                    g_error_actors_terminated++;
                    kill();
                }
                break;

            case ErrorInducingEvent::ErrorType::SendToInvalidActor:
                // Try to send to an invalid actor ID
                // Note: This should fail gracefully in the QB framework
                to(qb::ActorId(999999)).push<StatusEvent>(true);
                g_error_detected = true;

                // Actor should stay alive even after sending to invalid ID
                if (_should_recover) {
                    g_recovery_successful = true;
                } else {
                    g_error_actors_terminated++;
                    kill();
                }
                break;

            case ErrorInducingEvent::ErrorType::None:
            default:
                // No error, just respond with status
                break;
        }
    }

    void
    on(const MonitorEvent &event) {
        // Check if the target actor is alive and respond
        bool is_actor_alive = false;

        // This creates direct communication between actors
        try {
            // Try to send a message to the target
            to(event.target_id).push<StatusEvent>(true);
            is_actor_alive = true;
        } catch (...) {
            is_actor_alive = false;
        }

        // Send back status response
        to(event.getSource()).push<StatusEvent>(is_actor_alive);
    }
};

// Monitor actor to check the status of other actors
class MonitorActor : public qb::Actor {
    std::vector<qb::ActorId> _monitored_actors;
    int                      _num_actors_to_monitor;
    int                      _num_actors_checked;

public:
    explicit MonitorActor(int num_actors)
        : _num_actors_to_monitor(num_actors)
        , _num_actors_checked(0) {}

    bool
    onInit() override {
        registerEvent<StatusEvent>(*this);
        // KillEvent est déjà enregistré par défaut pour tous les acteurs

        // Add timeout handling for cases where actors don't respond
        // Use async callback to broadcast KillEvent to terminate all actors after
        // timeout
        qb::io::async::callback(
            [this]() {
                // Force terminate all actors if monitoring takes too long
                broadcast<qb::KillEvent>();
                // Kill self as well
                kill();
            },
            0.5); // 500ms timeout (0.5 seconds)

        return true;
    }

    void
    addActorToMonitor(qb::ActorId actor_id) {
        _monitored_actors.push_back(actor_id);
    }

    void
    startMonitoring() {
        for (const auto &actor_id : _monitored_actors) {
            to(actor_id).push<MonitorEvent>(id());
        }
    }

    void
    on(const StatusEvent &event) {
        // Track actor status responses
        _num_actors_checked++;

        // If we've checked all actors, terminate
        if (_num_actors_checked >= _num_actors_to_monitor) {
            kill();
        }
    }
};

// Coordinator actor that manages the error test
class CoordinatorActor : public qb::Actor {
    std::vector<qb::ActorId>      _error_actors;
    qb::ActorId                   _monitor_actor_id;
    ErrorInducingEvent::ErrorType _error_type;
    bool                          _should_actors_recover;
    int                           _num_actors;

public:
    CoordinatorActor(int num_actors, ErrorInducingEvent::ErrorType error_type,
                     bool should_recover)
        : _error_type(error_type)
        , _should_actors_recover(should_recover)
        , _num_actors(num_actors) {}

    bool
    onInit() override {
        registerEvent<StatusEvent>(*this);

        // Create error actors
        for (int i = 0; i < _num_actors; i++) {
            auto actor_id = addRefActor<ErrorActor>(i, _should_actors_recover);
            _error_actors.push_back(actor_id->id());
        }

        // Create monitor actor
        auto monitor      = addRefActor<MonitorActor>(_num_actors);
        _monitor_actor_id = monitor->id();

        // Add actors to monitor
        for (const auto &actor_id : _error_actors) {
            monitor->addActorToMonitor(actor_id);
        }

        // Trigger errors in all error actors
        for (const auto &actor_id : _error_actors) {
            to(actor_id).push<ErrorInducingEvent>(_error_type);
        }

        // Start monitoring (this will cause the test to eventually complete)
        monitor->startMonitoring();

        return true;
    }

    void
    on(const StatusEvent &) {
        // This is just a handler to receive potential messages from actors
        // No explicit action needed
    }
};

// Test actor recovery from exceptions
TEST(ErrorHandling, ShouldRecoverFromErrors) {
    // Reset global flags
    g_error_detected          = false;
    g_recovery_successful     = false;
    g_error_actors_terminated = 0;

    // Create main instance
    qb::Main main;

    // Number of actors to test
    const int num_actors = 3;

    // Add coordinator actor (with recovery enabled)
    main.addActor<CoordinatorActor>(0, num_actors,
                                    ErrorInducingEvent::ErrorType::ThrowException,
                                    true // actors should recover
    );

    // Run the engine
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Check that errors were detected
    EXPECT_TRUE(g_error_detected);

    // Check that recovery happened
    EXPECT_TRUE(g_recovery_successful);

    // No actors should have terminated
    EXPECT_EQ(g_error_actors_terminated, 0);
}

// Test actor termination when recovery is disabled
TEST(ErrorHandling, ShouldTerminateOnUnrecoverableErrors) {
    // Reset global flags
    g_error_detected          = false;
    g_recovery_successful     = false;
    g_error_actors_terminated = 0;

    // Create main instance
    qb::Main main;

    // Number of actors to test
    const int num_actors = 3;

    // Add coordinator actor (without recovery)
    main.addActor<CoordinatorActor>(0, num_actors,
                                    ErrorInducingEvent::ErrorType::InvalidOperation,
                                    false // actors should not recover
    );

    // Run the engine
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Check that errors were detected
    EXPECT_TRUE(g_error_detected);

    // Check that recovery did not happen
    EXPECT_FALSE(g_recovery_successful);

    // All actors should have terminated
    EXPECT_EQ(g_error_actors_terminated, num_actors);
}

// Test resilience when sending to invalid actors
TEST(ErrorHandling, ShouldHandleInvalidActorReferences) {
    // Reset global flags
    g_error_detected          = false;
    g_recovery_successful     = false;
    g_error_actors_terminated = 0;

    // Create main instance
    qb::Main main;

    // Number of actors to test
    const int num_actors = 3;

    // Add coordinator actor (with recovery enabled)
    main.addActor<CoordinatorActor>(0, num_actors,
                                    ErrorInducingEvent::ErrorType::SendToInvalidActor,
                                    true // actors should recover
    );

    // Run the engine
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Check that errors were detected
    EXPECT_TRUE(g_error_detected);

    // Check that recovery happened
    EXPECT_TRUE(g_recovery_successful);

    // No actors should have terminated
    EXPECT_EQ(g_error_actors_terminated, 0);
}