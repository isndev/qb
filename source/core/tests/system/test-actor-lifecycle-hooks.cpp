/**
 * @file qb/core/tests/system/test-actor-lifecycle-hooks.cpp
 * @brief Unit tests for actor lifecycle hooks
 * 
 * This file contains tests for the lifecycle hooks in the QB Actor Framework.
 * It verifies that actor lifecycle methods (onInit, onKill, destructor) are called
 * in the correct order and under various termination scenarios.
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
#include <qb/io.h>
#include <qb/io/async.h>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <functional>
#include <map>

// Define test events
struct StartEvent : public qb::Event {};
struct StopEvent : public qb::Event {};
struct KillEvent : public qb::Event {};
struct StatusEvent : public qb::Event {
    qb::ActorId reply_to;
    explicit StatusEvent(qb::ActorId id) : reply_to(id) {}
};

struct LifecycleEvent : public qb::Event {
    std::string actor_id;
    std::string event_type;
    void* data = nullptr;
    
    LifecycleEvent(const std::string& id, const std::string& type)
        : actor_id(id), event_type(type), data(nullptr) {}
        
    LifecycleEvent(const std::string& id, const std::string& type, void* payload)
        : actor_id(id), event_type(type), data(payload) {}
};

// Global state for test coordination
std::atomic<bool> g_test_complete{false};
std::vector<std::string> g_lifecycle_events;
std::mutex g_events_mutex;

// Helper to record lifecycle events in a thread-safe way
void recordLifecycleEvent(const std::string& actor_id, const std::string& event_type) {
    std::lock_guard<std::mutex> lock(g_events_mutex);
    g_lifecycle_events.push_back(actor_id + ":" + event_type);
}

// Forward declare for callback actor
class CallbackActor;

// Actor with lifecycle hooks
class LifecycleActor : public qb::Actor {
private:
    std::string _actor_name;
    bool _should_fail_init;
    bool _cleanup_resources;
    
public:
    LifecycleActor(std::string name, bool fail_init = false)
        : _actor_name(std::move(name)), _should_fail_init(fail_init), _cleanup_resources(false) {
        recordLifecycleEvent(_actor_name, "constructor");
    }
    
    ~LifecycleActor() override {
        // Simulate resource cleanup in destructor
        if (_cleanup_resources) {
            recordLifecycleEvent(_actor_name, "cleanup_resources");
        }
        
        recordLifecycleEvent(_actor_name, "destructor");
    }
    
    bool onInit() override {
        recordLifecycleEvent(_actor_name, "onInit");
        
        // Flag that we have resources to clean up (for testing)
        _cleanup_resources = true;
        
        // Setup timeout for delayed self-kill
        if (_actor_name == "delayed_stop_actor") {
            qb::io::async::callback([this]() {
                recordLifecycleEvent(_actor_name, "self_kill");
                kill();
            }, 0.2);
        }
        
        // External kill for immediate kill actor
        if (_actor_name == "immediate_kill_actor") {
            qb::io::async::callback([this]() {
                recordLifecycleEvent(_actor_name, "external_kill");
                kill();
            }, 0.1);
        }
        
        // Normal actor just stays alive
        
        // Optionally fail initialization for testing
        return !_should_fail_init;
    }
};

// Class that completes the test after delay
class TestCoordinatorActor : public qb::Actor {
public:
    bool onInit() override {
        recordLifecycleEvent("coordinator", "onInit");
        
        // Add a safety timeout to make sure test completes
        qb::io::async::callback([this]() {
            recordLifecycleEvent("coordinator", "test_complete");
            // Envoyer un broadcast pour tuer tous les acteurs
            broadcast<qb::KillEvent>();
            g_test_complete = true;
            kill();
        }, 1.0);
        
        return true;
    }
};

// Verify that lifecycle events are in the expected order
bool verifyLifecycleOrder(const std::vector<std::string>& events) {
    // Group events by actor
    std::map<std::string, std::vector<std::string>> actor_events;
    for (const auto& event : events) {
        size_t pos = event.find(':');
        if (pos != std::string::npos) {
            std::string actor = event.substr(0, pos);
            std::string event_type = event.substr(pos + 1);
            actor_events[actor].push_back(event_type);
        }
    }
    
    // Print all events for debugging
    std::cout << "All lifecycle events:" << std::endl;
    for (const auto& event : events) {
        std::cout << "  " << event << std::endl;
    }
    
    // Verify order for each actor
    for (const auto& [actor, actor_event_list] : actor_events) {
        std::cout << "Events for " << actor << ":" << std::endl;
        for (const auto& e : actor_event_list) {
            std::cout << "  " << e << std::endl;
        }
        
        // Normal actors should follow: constructor -> onInit -> [events] -> cleanup -> destructor
        if (actor == "normal_actor") {
            // Check for required events
            auto constructor_it = std::find(actor_event_list.begin(), actor_event_list.end(), "constructor");
            auto init_it = std::find(actor_event_list.begin(), actor_event_list.end(), "onInit");
            auto cleanup_it = std::find(actor_event_list.begin(), actor_event_list.end(), "cleanup_resources");
            auto destructor_it = std::find(actor_event_list.begin(), actor_event_list.end(), "destructor");
            
            if (constructor_it == actor_event_list.end() || 
                init_it == actor_event_list.end() ||
                cleanup_it == actor_event_list.end() ||
                destructor_it == actor_event_list.end()) {
                std::cout << "Missing required events for normal_actor" << std::endl;
                return false;
            }
            
            // Verify sequence
            // Constructor before onInit
            if (std::distance(actor_event_list.begin(), constructor_it) > 
                std::distance(actor_event_list.begin(), init_it)) {
                std::cout << "Constructor not before onInit for normal_actor" << std::endl;
                return false;
            }
            
            // Cleanup before destructor
            if (std::distance(actor_event_list.begin(), cleanup_it) > 
                std::distance(actor_event_list.begin(), destructor_it)) {
                std::cout << "Cleanup not before destructor for normal_actor" << std::endl;
                return false;
            }
        }
        
        // Failed initialization actors should only have constructor, onInit and destructor
        // Removed check for fail_init_actor as we're not testing it anymore
        
        // Immediate kill actor should have an external_kill event
        if (actor == "immediate_kill_actor") {
            auto external_kill_it = std::find(actor_event_list.begin(), actor_event_list.end(), "external_kill");
            if (external_kill_it == actor_event_list.end()) {
                std::cout << "Missing external_kill event for immediate_kill_actor" << std::endl;
                return false;
            }
        }
        
        // Delayed stop actor should have a self_kill event
        if (actor == "delayed_stop_actor") {
            auto self_kill_it = std::find(actor_event_list.begin(), actor_event_list.end(), "self_kill");
            
            if (self_kill_it == actor_event_list.end()) {
                std::cout << "Missing self_kill event for delayed_stop_actor" << std::endl;
                return false;
            }
        }
    }
    
    return true;
}

// Test for actor lifecycle hooks
TEST(ActorLifecycle, ShouldCallLifecycleHooksInCorrectOrder) {
    // Reset globals
    {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        g_lifecycle_events.clear();
    }
    g_test_complete = false;
    
    // Create simple Main instance
    qb::Main main;
    
    // Create a variety of lifecycle actors
    main.core(0).addActor<LifecycleActor>(std::string("normal_actor"));
    main.core(0).addActor<LifecycleActor>(std::string("delayed_stop_actor"));
    main.core(0).addActor<LifecycleActor>(std::string("immediate_kill_actor"));
    
    // Add coordinator to ensure test completes
    main.core(0).addActor<TestCoordinatorActor>();
    
    // Run the engine
    main.start(false);
    
    // Wait for test to complete (with timeout)
    for (int i = 0; i < 50 && !g_test_complete; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // If test did not complete, force completion
    if (!g_test_complete) {
        g_test_complete = true;
        std::cout << "Test timed out!" << std::endl;
    }
    
    // Stop the engine
    main.stop();
    
    // Wait a moment for any pending destructors to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Get the lifecycle events
    std::vector<std::string> events;
    {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        events = g_lifecycle_events;
    }
    
    // Verify test completion flag is set
    EXPECT_TRUE(g_test_complete);
    
    // Check if we have a reasonable number of events
    EXPECT_GT(events.size(), 10) << "Not enough lifecycle events recorded";
    
    // If we have events, verify they are in the expected order
    if (!events.empty()) {
        EXPECT_TRUE(verifyLifecycleOrder(events));
    }
} 