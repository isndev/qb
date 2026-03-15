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
# -----------------------------------------------------------------------------

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
