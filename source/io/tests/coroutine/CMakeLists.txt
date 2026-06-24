#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
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
# It includes comprehensive tests for C++20 coroutine integration with libev.
#
# Debug options (configure with -DQB_DEBUG_CORO_LIFECYCLE=ON):
#   QB_DEBUG_CORO_LIFECYCLE  - [sched] and [listener] traces (destructor, suspend, clear, reset)
# -----------------------------------------------------------------------------
option(QB_DEBUG_CORO_LIFECYCLE "Enable coroutine scheduler/listener lifecycle debug traces" OFF)

# -----------------------------------------------------------------------------
# Test targets definition
# -----------------------------------------------------------------------------

set(COROUTINE_TESTS
        test-coroutine-basic.cpp
        test-coroutine-exceptions.cpp
        test-coroutine-comprehensive.cpp
        test-coroutine-timers.cpp
        test-coroutine-edge-cases.cpp
        test-coroutine-safety.cpp
        test-coroutine-combinators.cpp
        test-coroutine-patterns.cpp
        test-coroutine-scheduler.cpp
        test-coroutine-awaiters.cpp
        test-coroutine-cancellation.cpp
        test-coroutine-channel.cpp
        test-coroutine-channel-payload.cpp
        test-coroutine-sync.cpp
        test-coroutine-retry.cpp
        test-coroutine-scope.cpp
        test-coroutine-generator.cpp
        test-coroutine-stream.cpp
        test-coroutine-stream-advanced.cpp
        test-coroutine-shared-task.cpp
        test-coroutine-regression.cpp
        test-coroutine-tcp-connector.cpp
        test-coroutine-tcp-client-io.cpp
        test-coroutine-tcp-client-integration.cpp
)

# test-coroutine-tcp-connector drives raw POSIX socket syscalls (sys/socket.h,
# fcntl, ::close); POSIX-only, so skip it on Windows (qb-io itself is portable).
if (WIN32)
    list(REMOVE_ITEM COROUTINE_TESTS test-coroutine-tcp-connector.cpp)
endif ()

# -----------------------------------------------------------------------------
# Create test targets
# -----------------------------------------------------------------------------

function(create_io_coroutine_test TEST_SOURCE)
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

    if (NOT TEST_NAME MATCHES "^test-coroutine-.+")
        message(FATAL_ERROR
                "Coroutine test sources must be named test-coroutine-*.cpp: ${TEST_SOURCE}")
    endif ()

    string(REGEX REPLACE "^test-" "" TEST_TARGET_SUFFIX "${TEST_NAME}")
    set(TARGET_NAME ${PROJECT_NAME}-gtest-${TEST_TARGET_SUFFIX})

    if (TARGET ${TARGET_NAME})
        message(FATAL_ERROR "Duplicate coroutine test target: ${TARGET_NAME}")
    endif ()

    qb_add_test(
            NAME ${TARGET_NAME}
            SOURCES ${TEST_SOURCE}
            DEPENDS ${PROJECT_NAME}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
            FOLDER "Tests/IO/Coroutine"
    )

    if (QB_DEBUG_CORO_LIFECYCLE AND
    (TEST_TARGET_SUFFIX STREQUAL "coroutine-combinators" OR
            TEST_TARGET_SUFFIX STREQUAL "coroutine-cancellation"))
        target_compile_definitions(${TARGET_NAME} PRIVATE QB_DEBUG_CORO_LIFECYCLE=1)
    endif ()

    if (QB_DEBUG_SCOPE AND TEST_TARGET_SUFFIX STREQUAL "coroutine-scope")
        target_compile_definitions(${TARGET_NAME} PRIVATE QB_DEBUG_SCOPE=1)
    endif ()
endfunction()

foreach (TEST_SOURCE ${COROUTINE_TESTS})
    create_io_coroutine_test(${TEST_SOURCE})
endforeach ()
