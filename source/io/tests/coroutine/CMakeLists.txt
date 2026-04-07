#
# qb - C++ Actor Framework
# Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# -----------------------------------------------------------------------------
# qb IO Coroutine Tests - CMake Configuration
#
# This file configures the test build system for the qb IO coroutine component.
# It includes comprehensive tests for C++23 coroutine integration with libev.
#
# Debug options (configure with -DQB_DEBUG_CORO_LIFECYCLE=ON):
#   QB_DEBUG_CORO_LIFECYCLE  - [sched] and [listener] traces (destructor, suspend, clear, reset)
# -----------------------------------------------------------------------------
option(QB_DEBUG_CORO_LIFECYCLE "Enable coroutine scheduler/listener lifecycle debug traces" OFF)

# Basic coroutine tests - core functionality
qb_add_test(
    NAME qb-io-gtest-coroutine-basic
    SOURCES test_basic.cpp
    DEPENDS qb-io gtest_main
)

# Exception handling tests
qb_add_test(
    NAME qb-io-gtest-coroutine-exceptions
    SOURCES test_exceptions.cpp
    DEPENDS qb-io gtest_main
)

# Comprehensive coroutine tests (TDD approach)
qb_add_test(
    NAME qb-io-gtest-coroutine-comprehensive
    SOURCES test_coroutine_comprehensive.cpp
    DEPENDS qb-io gtest_main
)

# Timer awaiter tests
qb_add_test(
    NAME qb-io-gtest-coroutine-timers
    SOURCES test_coroutine_timers.cpp
    DEPENDS qb-io gtest_main
)

# Edge cases and stress tests
qb_add_test(
    NAME qb-io-gtest-coroutine-edge-cases
    SOURCES test_coroutine_edge_cases.cpp
    DEPENDS qb-io gtest_main
)

# Safety and isolation tests
qb_add_test(
    NAME qb-io-gtest-coroutine-safety
    SOURCES test_coroutine_safety.cpp
    DEPENDS qb-io gtest_main
)

# Coroutine combinators and patterns (when_all, when_any, etc.)
qb_add_test(
    NAME qb-io-gtest-coroutine-combinators
    SOURCES test_coroutine_combinators.cpp
    DEPENDS qb-io gtest_main
)
if(QB_DEBUG_CORO_LIFECYCLE)
    target_compile_definitions(qb-io-gtest-coroutine-combinators PRIVATE QB_DEBUG_CORO_LIFECYCLE=1)
endif()

# Concurrency patterns (producer-consumer, pipeline, etc.)
qb_add_test(
    NAME qb-io-gtest-coroutine-patterns
    SOURCES test_coroutine_patterns.cpp
    DEPENDS qb-io gtest_main
)

# Scheduler and lifecycle tests
qb_add_test(
    NAME qb-io-gtest-coroutine-scheduler
    SOURCES test_coroutine_scheduler.cpp
    DEPENDS qb-io gtest_main
)

# Custom awaiter tests
qb_add_test(
    NAME qb-io-gtest-coroutine-awaiters
    SOURCES test_coroutine_awaiters.cpp
    DEPENDS qb-io gtest_main
)

# Performance benchmark tests
qb_add_test(
    NAME qb-io-gtest-coroutine-benchmark
    SOURCES test_coroutine_benchmark.cpp
    DEPENDS qb-io gtest_main
)

# Cancellation tests
qb_add_test(
    NAME qb-io-gtest-coroutine-cancellation
    SOURCES test_coroutine_cancellation.cpp
    DEPENDS qb-io gtest_main
)
if(QB_DEBUG_CORO_LIFECYCLE)
    target_compile_definitions(qb-io-gtest-coroutine-cancellation PRIVATE QB_DEBUG_CORO_LIFECYCLE=1)
endif()

# Channel tests
qb_add_test(
    NAME qb-io-gtest-coroutine-channel
    SOURCES test_coroutine_channel.cpp
    DEPENDS qb-io gtest_main
)

# Sync primitives tests
qb_add_test(
    NAME qb-io-gtest-coroutine-sync
    SOURCES test_coroutine_sync.cpp
    DEPENDS qb-io gtest_main
)

# Retry tests
qb_add_test(
    NAME qb-io-gtest-coroutine-retry
    SOURCES test_coroutine_retry.cpp
    DEPENDS qb-io gtest_main
)

# Scope tests
qb_add_test(
    NAME qb-io-gtest-coroutine-scope
    SOURCES test_coroutine_scope.cpp
    DEPENDS qb-io gtest_main
)
# Optional: -DQB_DEBUG_SCOPE=1 for scope/scheduler flow traces
# -DQB_DEBUG_SCOPE=1 for scope/scheduler flow traces when debugging
if(QB_DEBUG_SCOPE)
    target_compile_definitions(qb-io-gtest-coroutine-scope PRIVATE QB_DEBUG_SCOPE=1)
endif()

# Generator tests
qb_add_test(
    NAME qb-io-gtest-coroutine-generator
    SOURCES test_coroutine_generator.cpp
    DEPENDS qb-io gtest_main
)

# Stream tests
qb_add_test(
    NAME qb-io-gtest-coroutine-stream
    SOURCES test_coroutine_stream.cpp
    DEPENDS qb-io gtest_main
)

# shared_task<T> tests
qb_add_test(
    NAME qb-io-gtest-coroutine-shared-task
    SOURCES test_coroutine_shared_task.cpp
    DEPENDS qb-io gtest_main
)

# Regression tests for all identified and fixed coroutine bugs
qb_add_test(
    NAME qb-io-gtest-coroutine-regression
    SOURCES test_coroutine_regression.cpp
    DEPENDS qb-io gtest_main
)

# -----------------------------------------------------------------------------
# DISABLED: coroutine client/stream/connector (moved to coroutine/old/, redesign pending)
# -----------------------------------------------------------------------------
# qb_add_test(
#     NAME qb-io-gtest-coroutine-client
#     SOURCES test_coroutine_client.cpp
#     DEPENDS qb-io gtest_main
# )
# qb_add_test(
#     NAME qb-io-gtest-coroutine-stream-advanced
#     SOURCES test_coro_stream_advanced.cpp
#     DEPENDS qb-io gtest_main
# )
# qb_add_test(
#     NAME qb-io-gtest-connector-integration
#     SOURCES test_connector_integration.cpp
#     DEPENDS qb-io gtest_main
# )
# qb_add_test(
#     NAME qb-io-gtest-client-io-operations
#     SOURCES test_client_io_operations.cpp
#     DEPENDS qb-io gtest_main
# )
# qb_add_test(
#     NAME qb-io-gtest-coro-client-integration
#     SOURCES test_coro_client_integration.cpp
#     DEPENDS qb-io gtest_main
# )