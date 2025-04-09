/**
 * @file qb/core/tests/system/test-actor-resource-management.cpp
 * @brief Unit tests for actor resource management and cleanup
 *
 * This file contains tests for resource management in the QB Actor Framework.
 * It verifies that actors correctly allocate and release resources, and that
 * the framework properly cleans up all resources when actors are terminated.
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
#include <memory>
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/io/async.h>
#include <qb/main.h>
#include <vector>

// Define test events
struct AllocateResourceEvent : public qb::Event {
    int size;
    explicit AllocateResourceEvent(int s)
        : size(s) {}
};

struct ReleaseResourceEvent : public qb::Event {
    int resource_id;
    explicit ReleaseResourceEvent(int id)
        : resource_id(id) {}
};

struct ResourceStatusEvent : public qb::Event {
    qb::ActorId reply_to;
    explicit ResourceStatusEvent(qb::ActorId id)
        : reply_to(id) {}
};

struct ResourceReportEvent : public qb::Event {
    int    allocated_count;
    size_t memory_usage;

    ResourceReportEvent(int count, size_t memory)
        : allocated_count(count)
        , memory_usage(memory) {}
};

struct GracefulShutdownEvent : public qb::Event {};
struct ForceShutdownEvent : public qb::Event {};

// Track resource allocation/deallocation
std::atomic<int>    g_resources_allocated{0};
std::atomic<int>    g_resources_freed{0};
std::atomic<size_t> g_memory_allocated{0};
std::atomic<size_t> g_memory_freed{0};
std::atomic<bool>   g_test_complete{false};

// Resource class with tracking of construction/destruction
class ManagedResource {
private:
    int               _id;
    std::vector<char> _data;

public:
    explicit ManagedResource(int id, size_t size)
        : _id(id)
        , _data(size) {
        // Track allocation
        g_resources_allocated++;
        g_memory_allocated += size;
    }

    ~ManagedResource() {
        // Track deallocation
        g_resources_freed++;
        g_memory_freed += _data.size();
    }

    int
    id() const {
        return _id;
    }
    size_t
    size() const {
        return _data.size();
    }
};

// Actor that manages resources
class ResourceActor : public qb::Actor {
private:
    std::vector<std::unique_ptr<ManagedResource>> _resources;
    bool                                          _shutdown_pending;

public:
    ResourceActor()
        : _shutdown_pending(false) {}

    bool
    onInit() override {
        registerEvent<AllocateResourceEvent>(*this);
        registerEvent<ReleaseResourceEvent>(*this);
        registerEvent<ResourceStatusEvent>(*this);
        registerEvent<GracefulShutdownEvent>(*this);
        registerEvent<ForceShutdownEvent>(*this);
        return true;
    }

    // Allocate a new resource
    void
    on(const AllocateResourceEvent &event) {
        if (_shutdown_pending)
            return;

        int resource_id = static_cast<int>(_resources.size());
        _resources.push_back(std::make_unique<ManagedResource>(resource_id, event.size));
    }

    // Release a specific resource
    void
    on(const ReleaseResourceEvent &event) {
        if (event.resource_id >= 0 &&
            static_cast<size_t>(event.resource_id) < _resources.size()) {
            if (_resources[event.resource_id]) {
                _resources[event.resource_id].reset();
            }
        }
    }

    // Report resource status
    void
    on(const ResourceStatusEvent &event) {
        int    count  = 0;
        size_t memory = 0;

        for (const auto &res : _resources) {
            if (res) {
                count++;
                memory += res->size();
            }
        }

        to(event.reply_to).push<ResourceReportEvent>(count, memory);
    }

    // Perform graceful shutdown, releasing all resources first
    void
    on(const GracefulShutdownEvent &) {
        _shutdown_pending = true;

        // Clear all resources one by one
        for (auto &res : _resources) {
            res.reset();
        }
        _resources.clear();

        // Wait to ensure all destructors have been called
        qb::io::async::callback([this]() { kill(); }, 0.1);
    }

    // Force shutdown without explicit cleanup
    void
    on(const ForceShutdownEvent &) {
        kill();
    }

    ~ResourceActor() override {
        // The vector and resources will be automatically cleaned up here
    }
};

// Coordinator actor
class ResourceCoordinatorActor : public qb::Actor {
private:
    std::vector<qb::ActorId> _resource_actors;
    int                      _num_allocations;
    int                      _phase;

public:
    explicit ResourceCoordinatorActor(int num_allocations = 100)
        : _num_allocations(num_allocations)
        , _phase(0) {}

    bool
    onInit() override {
        registerEvent<ResourceReportEvent>(*this);

        // Create several resource actors
        for (int i = 0; i < 3; i++) {
            auto actor = addRefActor<ResourceActor>();
            _resource_actors.push_back(actor->id());
        }

        // Start test sequence
        schedulePhase1();

        return true;
    }

    // Phase 1: Allocate resources
    void
    schedulePhase1() {
        const int bytes_per_resource = 1024; // 1KB

        for (int i = 0; i < _num_allocations; i++) {
            qb::ActorId target = _resource_actors[i % _resource_actors.size()];

            // Allocate with varying sizes
            int size = bytes_per_resource * ((i % 10) + 1);
            to(target).push<AllocateResourceEvent>(size);
        }

        // Move to phase 2 after allocation
        qb::io::async::callback(
            [this]() {
                _phase = 1;
                schedulePhase2();
            },
            0.2);
    }

    // Phase 2: Release some resources manually
    void
    schedulePhase2() {
        // Release every third resource
        for (int i = 0; i < _num_allocations; i += 3) {
            qb::ActorId target = _resource_actors[i % _resource_actors.size()];
            to(target).push<ReleaseResourceEvent>(i / 3);
        }

        // Check status after release
        qb::io::async::callback(
            [this]() {
                for (auto actor_id : _resource_actors) {
                    to(actor_id).push<ResourceStatusEvent>(id());
                }

                // Move to phase 3 after status check
                qb::io::async::callback(
                    [this]() {
                        _phase = 2;
                        schedulePhase3();
                    },
                    0.2);
            },
            0.2);
    }

    // Phase 3: Perform graceful shutdown on one actor
    void
    schedulePhase3() {
        if (!_resource_actors.empty()) {
            // Perform graceful shutdown on the first actor
            to(_resource_actors[0]).push<GracefulShutdownEvent>();

            // Remove it from our list
            _resource_actors.erase(_resource_actors.begin());
        }

        // Move to phase 4 after graceful shutdown
        qb::io::async::callback(
            [this]() {
                _phase = 3;
                schedulePhase4();
            },
            0.3);
    }

    // Phase 4: Force shutdown on remaining actors
    void
    schedulePhase4() {
        for (auto actor_id : _resource_actors) {
            to(actor_id).push<ForceShutdownEvent>();
        }
        _resource_actors.clear();

        // Complete test
        qb::io::async::callback(
            [this]() {
                g_test_complete = true;
                kill();
            },
            0.3);
    }

    // Handle resource status reports
    void
    on(const ResourceReportEvent &event) {
        // Just log the statuses (nothing to do here for the test)
    }
};

// Test for proper resource management
TEST(ResourceManagement, ShouldReleaseAllResourcesOnActorDestruction) {
    // Reset globals
    g_resources_allocated = 0;
    g_resources_freed     = 0;
    g_memory_allocated    = 0;
    g_memory_freed        = 0;
    g_test_complete       = false;

    // Number of allocations to test
    const int num_allocations = 100;

    // Create main instance
    qb::Main main;

    // Add coordinator actor
    main.core(0).addActor<ResourceCoordinatorActor>(num_allocations);

    // Run the engine
    main.start(false);
    EXPECT_FALSE(main.hasError());

    // Verify test completion
    EXPECT_TRUE(g_test_complete);

    // Verify all resources were properly freed
    EXPECT_EQ(g_resources_allocated.load(), g_resources_freed.load());
    EXPECT_EQ(g_memory_allocated.load(), g_memory_freed.load());
}

// Test for resource cleanup after actor failure
TEST(ResourceManagement, ShouldReleaseResourcesEvenAfterActorFailure) {
    // Implementation would be similar to above but with
    // simulated failures rather than graceful shutdown
}