/**
 * @file test_connector_integration.cpp
 * @brief Integration tests for coroutine_connector
 *
 * Tests connector functionality, lifecycle, and edge cases.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Connector Basic Functionality
// =============================================================================

class ConnectorBasic : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Connector instantiation
 * @brief Verifies connector can be created and initial state is correct
 */
TEST_F(ConnectorBasic, ConnectorInstantiation) {
    auto connector = make_coroutine_connector<qb::io::transport::tcp>();
    
    EXPECT_FALSE(connector.is_connected());
}

/**
 * @test Multiple connector instances
 * @brief Verifies multiple connectors can coexist
 */
TEST_F(ConnectorBasic, MultipleConnectorInstances) {
    auto connector1 = make_coroutine_connector<qb::io::transport::tcp>();
    auto connector2 = make_coroutine_connector<qb::io::transport::tcp>();
    auto connector3 = make_coroutine_connector<qb::io::transport::tcp>();
    
    EXPECT_FALSE(connector1.is_connected());
    EXPECT_FALSE(connector2.is_connected());
    EXPECT_FALSE(connector3.is_connected());
}

/**
 * @test Connector move construction
 * @brief Verifies connector can be move-constructed
 */
TEST_F(ConnectorBasic, MoveConstruction) {
    auto connector1 = make_coroutine_connector<qb::io::transport::tcp>();
    
    // Move construct
    auto connector2 = std::move(connector1);
    
    EXPECT_FALSE(connector2.is_connected());
}

/**
 * @test Connector move assignment
 * @brief Verifies connector can be move-assigned
 */
TEST_F(ConnectorBasic, MoveAssignment) {
    auto connector1 = make_coroutine_connector<qb::io::transport::tcp>();
    auto connector2 = make_coroutine_connector<qb::io::transport::tcp>();
    
    // Move assign
    connector2 = std::move(connector1);
    
    EXPECT_FALSE(connector2.is_connected());
}

// =============================================================================
// TEST SUITE: Connector Lifecycle
// =============================================================================

class ConnectorLifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Disconnect without connection is safe
 * @brief Verifies disconnect() can be called even when not connected
 */
TEST_F(ConnectorLifecycle, DisconnectWithoutConnection) {
    auto connector = make_coroutine_connector<qb::io::transport::tcp>();
    
    EXPECT_FALSE(connector.is_connected());
    
    // Should not crash or cause issues
    connector.disconnect();
    connector.disconnect();
    
    EXPECT_FALSE(connector.is_connected());
}

/**
 * @test Transport access without connection
 * @brief Verifies transport() returns valid (but closed) transport before connection
 */
TEST_F(ConnectorLifecycle, TransportAccessWithoutConnection) {
    auto connector = make_coroutine_connector<qb::io::transport::tcp>();
    
    // Can get transport reference even before connection
    auto& transport = connector.transport();
    EXPECT_FALSE(transport.is_open());
}

// =============================================================================
// TEST SUITE: Connection Failure Tests
// =============================================================================

class ConnectorFailures : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Invalid URI handling
 * @brief Verifies connector handles malformed URIs
 */
TEST_F(ConnectorFailures, InvalidUri) {
    auto target = qb::io::uri("invalid://uri");
    auto connector = make_coroutine_connector<qb::io::transport::tcp>();
    std::atomic<bool> done{false};
    bool connection_result = true;

    auto connect_task = [&connector, &target, &connection_result, &done]() -> qb::io::async::task<void> {
        connection_result = co_await connector.co_connect(target, 1s);
        done.store(true);
    };

    coro_scheduler().spawn(connect_task());
    
    int iterations = 0;
    while (!done.load() && iterations < 100) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    // Invalid URIs should fail
    EXPECT_FALSE(connection_result);
    EXPECT_FALSE(connector.is_connected());
}

/**
 * @test Empty URI handling
 * @brief Verifies connector handles empty/invalid URIs
 */
TEST_F(ConnectorFailures, EmptyUri) {
    auto target = qb::io::uri("");
    auto connector = make_coroutine_connector<qb::io::transport::tcp>();
    std::atomic<bool> done{false};
    bool connection_result = true;

    auto connect_task = [&connector, &target, &connection_result, &done]() -> qb::io::async::task<void> {
        connection_result = co_await connector.co_connect(target, 1s);
        done.store(true);
    };

    coro_scheduler().spawn(connect_task());
    
    int iterations = 0;
    while (!done.load() && iterations < 100) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_FALSE(connection_result);
    EXPECT_FALSE(connector.is_connected());
}

/**
 * @test Malformed TCP URI
 * @brief Verifies connector handles malformed TCP URIs
 */
TEST_F(ConnectorFailures, MalformedTcpUri) {
    auto target = qb::io::uri("tcp://");
    auto connector = make_coroutine_connector<qb::io::transport::tcp>();
    std::atomic<bool> done{false};
    bool connection_result = true;

    auto connect_task = [&connector, &target, &connection_result, &done]() -> qb::io::async::task<void> {
        connection_result = co_await connector.co_connect(target, 1s);
        done.store(true);
    };

    coro_scheduler().spawn(connect_task());
    
    int iterations = 0;
    while (!done.load() && iterations < 100) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_FALSE(connection_result);
}

// =============================================================================
// TEST SUITE: Parallel Operations
// =============================================================================

class ConnectorParallel : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple parallel connectors can be created
 * @brief Verifies multiple connectors can coexist in parallel
 */
TEST_F(ConnectorParallel, MultipleParallelConnectors) {
    constexpr int NUM_CONNECTORS = 5;
    std::atomic<int> created_count{0};

    auto task = [&created_count]() -> qb::io::async::task<void> {
        for (int i = 0; i < NUM_CONNECTORS; ++i) {
            auto connector = make_coroutine_connector<qb::io::transport::tcp>();
            EXPECT_FALSE(connector.is_connected());
            created_count.fetch_add(1);
        }
        co_return;
    };

    coro_scheduler().spawn(task());
    run_for(100ms);

    EXPECT_EQ(created_count.load(), NUM_CONNECTORS);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
