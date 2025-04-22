/**
 * @file qb/io/tests/system/test-event-combined.cpp
 * @brief Unit tests for combined asynchronous event handling
 *
 * This file contains tests for handling multiple asynchronous events simultaneously
 * in the QB framework, testing the coordination between different event types
 * including timers, signals, and file events.
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
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <qb/io/async/event/all.h>
#include <qb/io/async/listener.h>
#include <qb/io/system/file.h>
#include <thread>
#ifdef _WIN32
using pid_t = int;
#else
#include <unistd.h> // For getpid()
#endif
#include <vector>

// Define platform detection macros
#if defined(__APPLE__)
#define QB_PLATFORM_MACOS 1
#else
#define QB_PLATFORM_MACOS 0
#endif

// Actor class that handles multiple event types
struct EventHandler {
    std::atomic<int> timer_events{0};
    std::atomic<int> signal_events{0};
    std::atomic<int> file_events{0};
    std::atomic<int> io_events{0};
    int              fd_test = 0;

    bool
    is_alive() {
        return true;
    }

    // Handler for SIGINT
    void
    on(qb::io::async::event::signal<SIGINT> const &event) {
        EXPECT_EQ(SIGINT, event.signum);
        ++signal_events;
        std::cout << "Received signal event" << std::endl;
    }

    // Handler for Timer events
    void
    on(qb::io::async::event::timer const &) {
        ++timer_events;
        std::cout << "Received timer event #" << timer_events << std::endl;
    }

    // Handler for File events
    void
    on(qb::io::async::event::file const &event) {
#ifdef _WIN32
        EXPECT_GE(event.attr.st_size, 0);
#else
        EXPECT_GE(event.attr.st_size, 0);
#endif
        ++file_events;
        std::cout << "Received file event, file size: " << event.attr.st_size
                  << std::endl;
    }

    // Handler for IO events
    void
    on(qb::io::async::event::io &event) {
        EXPECT_EQ(fd_test, event.fd);
        EXPECT_TRUE(event._revents & EV_READ);
        event.stop();
        ++io_events;
        std::cout << "Received IO event" << std::endl;
    }
};

// Helper function to reset libev and start fresh
void
reinitialize_libev() {
    std::cout << "Re-initializing libev..." << std::endl;
    qb::io::async::init();
}

// Get current process ID safely
pid_t
get_process_id() {
    return getpid();
}

// Safe way to send SIGINT to the current process
void
send_signal_to_self() {
#if !QB_PLATFORM_MACOS
    // Use numeric signal value 2 for SIGINT on Linux with direct PID
    std::string cmd = "kill -2 " + std::to_string(get_process_id());
    int ret = system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "Failed to send signal via system command, ret=" << ret << std::endl;
    }
#else
    std::raise(SIGINT);
#endif
}

#if !QB_PLATFORM_MACOS
// Test focusing only on file events - not suitable for macOS
TEST(KernelEventsCombined, FileEvent) {
    reinitialize_libev();
    qb::io::async::listener handler;
    EventHandler            actor;

    // Create test file first to ensure it exists
    std::cout << "Creating test file..." << std::endl;
    EXPECT_EQ(system("echo 'file event test' > test-file.txt"), 0);
    EXPECT_EQ(system("ls -la test-file.txt"), 0);

    // Register only the file event
    std::cout << "Registering file event..." << std::endl;
    auto &event =
        handler.registerEvent<qb::io::async::event::file>(actor, "./test-file.txt", 0);
    event.start();

    // Run event loop for a short time
    auto start_time = std::chrono::steady_clock::now();
    auto duration   = std::chrono::milliseconds(500);

    // Run for a bit to see if file event is detected
    while (std::chrono::steady_clock::now() - start_time < duration) {
        handler.run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Modify the file to trigger an event
    std::cout << "Modifying file to trigger event..." << std::endl;
    EXPECT_EQ(system("echo 'modified content' >> test-file.txt"), 0);

    // Run for a bit more to detect the modification
    start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < duration) {
        handler.run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Print event count
        std::cout << "File events: " << actor.file_events << std::endl;
    }

    // Verify file events were detected
    EXPECT_GT(actor.file_events, 0) << "Expected at least one file event";

    // Cleanup
    EXPECT_EQ(system("rm -f test-file.txt"), 0);

    // Make sure to stop all events
    event.stop();
}
#endif

// Test for basic timer and signal functionality - works on all platforms
TEST(KernelEventsCombined, BasicTimerAndSignal) {
    reinitialize_libev();
    qb::io::async::listener handler;
    EventHandler            actor;

    // Register timer and signal events
    auto &sig_event = handler.registerEvent<qb::io::async::event::signal<SIGINT>>(actor);
    auto &timer_event =
        handler.registerEvent<qb::io::async::event::timer>(actor, 0.1, 0.1);

    sig_event.start();
    timer_event.start();

    // We'll use a direct approach to raise the signal
    auto start_time    = std::chrono::steady_clock::now();
    auto duration      = std::chrono::milliseconds(700);
    bool signal_raised = false;

    while (std::chrono::steady_clock::now() - start_time < duration) {
        if (actor.timer_events == 3 && !signal_raised) {
            // Raise the signal after a few timer events
            std::cout << "Raising SIGINT..." << std::endl;
            std::raise(SIGINT);
            signal_raised = true;
        }

        // Run the event loop to process events
        handler.run(EVRUN_ONCE);

        // Add some debug output
        std::cout << "Event counts - Timer: " << actor.timer_events
                  << ", Signal: " << actor.signal_events << std::endl;

        // Break early if we've received the signal and multiple timer events
        if (actor.signal_events > 0 && actor.timer_events >= 5) {
            std::cout << "Required events received, breaking early" << std::endl;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify events were received
    EXPECT_GE(actor.timer_events, 2) << "Should have received multiple timer events";
    EXPECT_GE(actor.signal_events, 1) << "Should have received at least one signal";

    // Stop all events to clean up properly
    sig_event.stop();
    timer_event.stop();
}

// Simple Timer-only test to avoid assertion failures
TEST(KernelEventsCombined, TimerOnly) {
    reinitialize_libev();
    qb::io::async::listener handler;
    EventHandler            actor;

    // Reset counters
    actor.timer_events  = 0;
    actor.signal_events = 0;
    actor.file_events   = 0;
    actor.io_events     = 0;

    // Register timer events with different intervals
    auto &event = handler.registerEvent<qb::io::async::event::timer>(actor, 0.05, 0.05);
    event.start();

    // Run event loop for a short time
    auto start_time = std::chrono::steady_clock::now();
    auto duration   = std::chrono::milliseconds(400);

    while (std::chrono::steady_clock::now() - start_time < duration) {
        handler.run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Break early if we've received several timer events
        if (actor.timer_events >= 5) {
            break;
        }
    }

    // Verify events were processed
    EXPECT_GE(actor.timer_events, 2) << "Should have received multiple timer events";

    std::cout << "TimerOnly test complete: " << actor.timer_events << " timer events"
              << std::endl;

    // Stop the event to clean up
    event.stop();
}

// Test IO events - should be last test to avoid libev assertion failures
TEST(KernelEventsCombined, IOEvents) {
    reinitialize_libev();
    qb::io::async::listener handler;
    EventHandler            actor;

    // Reset counters since this might run after other tests
    actor.timer_events  = 0;
    actor.signal_events = 0;
    actor.file_events   = 0;
    actor.io_events     = 0;

    // Create temporary file
    std::cout << "Creating IO test file..." << std::endl;
    EXPECT_EQ(system("echo 'io test data' > test-io.file"), 0);

    qb::io::sys::file f("test-io.file");
    actor.fd_test = f.native_handle();

    // Register IO event
    auto &event = handler.registerEvent<qb::io::async::event::io>(
        actor, f.native_handle(), EV_READ);
    event.start();

    // Run event loop for a short time
    auto start_time = std::chrono::steady_clock::now();
    auto duration   = std::chrono::milliseconds(300);

    while (std::chrono::steady_clock::now() - start_time < duration) {
        handler.run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Break if we got the IO event to avoid running too long
        if (actor.io_events > 0) {
            break;
        }
    }

    // Verify IO events were processed
    EXPECT_GT(actor.io_events, 0) << "Should have received at least one IO event";

    // Cleanup - but continue even if this fails
    int cleanup_ret = system("rm -f test-io.file");
    if (cleanup_ret != 0) {
        std::cerr << "Warning: Cleanup command failed with return code " << cleanup_ret << std::endl;
    }

    // Already stopped by the handler
}