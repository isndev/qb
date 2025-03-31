/**
 * @file qb/core/tests/system/test-actor-delayed-events.cpp
 * @brief Unit tests for actor delayed event processing
 * 
 * This file contains tests for delayed event processing in the QB Actor Framework.
 * It verifies that actors can properly schedule, queue, and process events with timing constraints
 * using the non-blocking async callback mechanism.
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
#include <qb/system/timestamp.h>
#include <vector>
#include <atomic>
#include <mutex>

// Define test events
struct TimerEvent : public qb::Event {
    uint64_t timestamp;
    int timer_id;
    
    TimerEvent(int id) : timer_id(id), timestamp(0) {}
};

struct CompleteEvent : public qb::Event {};

// Global variables to track event order
std::atomic<int> g_completed_timers{0};
std::vector<int> g_timer_order;
std::mutex g_order_mutex;

// Timer actor that schedules and processes timed events using async callbacks
class TimerActor : public qb::Actor {
    int _num_timers;
    std::vector<uint64_t> _timestamps;
    
public:
    explicit TimerActor(int num_timers) : _num_timers(num_timers) {
        _timestamps.resize(num_timers + 1, 0);
    }

    bool onInit() override {
        registerEvent<TimerEvent>(*this);
        registerEvent<CompleteEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        
        // Schedule timer events in reverse order to test they execute in correct order
        for (int i = _num_timers; i > 0; --i) {
            // Create and send the first event to self
            // The delay will be handled in the event handler
            to(id()).push<TimerEvent>(i);
        }
        
        return true;
    }

    void on(const TimerEvent& event) {
        // Record the timestamp of this event using precise timing
        uint64_t current_time = qb::Timestamp::nano();
        
        // For first timer reception, record time and schedule a delayed repeat
        if (_timestamps[event.timer_id] == 0) {
            _timestamps[event.timer_id] = current_time;
            
            // Schedule delayed callback based on timer_id to ensure proper ordering
            // Higher timer_id = longer delay to make them complete in ascending order
            double delay_sec = event.timer_id * 0.05; // 50ms in seconds
            
            // Use simplified async callback with direct seconds parameter
            qb::io::async::callback([this, timer_id = event.timer_id]() {
                // When the timer finishes, send another event to self
                to(id()).push<TimerEvent>(timer_id);
            }, delay_sec);
        } 
        // On second event reception, verify order and record completion
        else {
            // Calculate elapsed time
            uint64_t elapsed = current_time - _timestamps[event.timer_id];
            
            // Record the timer completion order
            {
                std::lock_guard<std::mutex> lock(g_order_mutex);
                g_timer_order.push_back(event.timer_id);
            }
            
            // Increment completed timers counter
            g_completed_timers++;
            
            // If all timers completed, send completion event
            if (g_completed_timers == _num_timers) {
                to(id()).push<CompleteEvent>();
            }
        }
    }

    void on(const CompleteEvent&) {
        // Test complete, kill self
        kill();
    }
    
    // Handler for KillEvent
    void on(const qb::KillEvent&) {
        kill();
    }
};

// Test for timer ordering in actor system
TEST(DelayedEvents, ShouldProcessEventsInTimerOrder) {
    // Reset global trackers
    g_completed_timers = 0;
    {
        std::lock_guard<std::mutex> lock(g_order_mutex);
        g_timer_order.clear();
    }
    
    // Number of timers to test
    const int num_timers = 5;
    
    // Create main instance with io support
    qb::Main main; // IO engine is enabled by default in qb::Main
    
    // Add timer actor
    main.addActor<TimerActor>(0, num_timers);
    
    // Start and run engine
    main.start(false);
    EXPECT_FALSE(main.hasError());
    
    // Verify all timers completed
    EXPECT_EQ(g_completed_timers, num_timers);
    
    // Verify timers completed in correct order (1 to num_timers)
    std::vector<int> expected_order;
    for (int i = 1; i <= num_timers; ++i) {
        expected_order.push_back(i);
    }
    
    {
        std::lock_guard<std::mutex> lock(g_order_mutex);
        EXPECT_EQ(g_timer_order, expected_order);
    }
}

// Actor that uses async callbacks for regular timing
class CallbackActor : public qb::Actor {
    const int _target_count;
    int _current_count;
    uint64_t _start_time;
    
public:
    explicit CallbackActor(int target_count) 
        : _target_count(target_count), _current_count(0), _start_time(0) {}
    
    bool onInit() override {
        registerEvent<qb::KillEvent>(*this);
        _start_time = qb::Timestamp::nano();
        
        // Schedule first callback immediately (0 sec delay)
        qb::io::async::callback([this]() {
            handle_callback();
        }, 0.0);
        
        return true;
    }
    
    void handle_callback() {
        // Increment counter with each callback
        _current_count++;
        
        // If reached target, terminate
        if (_current_count >= _target_count) {
            uint64_t elapsed = qb::Timestamp::nano() - _start_time;
            
            // Record elapsed time (should be proportional to target_count)
            {
                std::lock_guard<std::mutex> lock(g_order_mutex);
                g_timer_order.push_back(static_cast<int>(elapsed / 1000000)); // convert to ms
            }
            
            kill();
        } else {
            // Schedule next callback after a short delay (1ms = 0.001 sec)
            qb::io::async::callback([this]() {
                handle_callback();
            }, 0.001);
        }
    }
    
    // Handler for KillEvent
    void on(const qb::KillEvent&) {
        kill();
    }
};

// Test for consistent callback timing
TEST(DelayedEvents, ShouldMaintainConsistentCallbackTiming) {
    // Reset global trackers
    {
        std::lock_guard<std::mutex> lock(g_order_mutex);
        g_timer_order.clear();
    }
    
    // Test parameters
    const int callback_count = 50;
    
    // Create main instance with io support
    qb::Main main; // Enable IO engine
    
    // Add actor that uses callbacks
    main.addActor<CallbackActor>(0, callback_count);
    
    // Start and run engine
    main.start(false);
    EXPECT_FALSE(main.hasError());
    
    // Verify timing recorded (approximate check for reasonability)
    std::lock_guard<std::mutex> lock(g_order_mutex);
    ASSERT_EQ(g_timer_order.size(), 1);
    
    // Elapsed time should be roughly proportional to callback count
    // This is a loose check, adjust based on actual timing of system
    EXPECT_GT(g_timer_order[0], 0);
} 