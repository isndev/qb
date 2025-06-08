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
# qb Core System Tests - CMake Configuration
#
# This file configures the system tests for the qb Core component.
# It defines test targets for various aspects of the actor system including
# type system, main components, IO integration, and actor functionality.
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Test targets by category
# -----------------------------------------------------------------------------

# Core type system tests
set(TYPE_SYSTEM_TESTS
        test-type.cpp
)

# Main system component tests
set(MAIN_SYSTEM_TESTS
        test-main.cpp
        test-io.cpp
)

# Actor system functionality tests
set(ACTOR_SYSTEM_TESTS
        test-actor-add.cpp
        test-actor-callback.cpp
        test-actor-dependency.cpp
        test-actor-event.cpp
        test-actor-service-event.cpp
        test-actor-broadcast.cpp
        test-actor-delayed-events.cpp
        test-actor-error-handling.cpp
        test-actor-state-persistence.cpp
        test-actor-concurrency-safety.cpp
        test-actor-resource-management.cpp
        test-actor-lifecycle-hooks.cpp
)

# -----------------------------------------------------------------------------
# Create test targets
# -----------------------------------------------------------------------------

# Helper function to create test targets with consistent naming and properties
function(create_core_test TEST_SOURCE TEST_CATEGORY)
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    set(TARGET_NAME ${PROJECT_NAME}-gtest-system-${TEST_NAME})

    cxx_gtest(
            ${TARGET_NAME}
            "${PROJECT_NAME}"
            ${TEST_SOURCE}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
            FOLDER "Tests/Core/System/${TEST_CATEGORY}"
    )
endfunction()

# Create type system test targets
foreach (TEST_SOURCE ${TYPE_SYSTEM_TESTS})
    create_core_test(${TEST_SOURCE} "TypeSystem")
endforeach ()

# Create main system test targets
foreach (TEST_SOURCE ${MAIN_SYSTEM_TESTS})
    create_core_test(${TEST_SOURCE} "Main")
endforeach ()

# Create actor system test targets
foreach (TEST_SOURCE ${ACTOR_SYSTEM_TESTS})
    create_core_test(${TEST_SOURCE} "Actor")
endforeach ()

# Additional test configuration if needed
message(STATUS "[Core Tests] Configured ${CMAKE_CURRENT_SOURCE_DIR} with ${CMAKE_CURRENT_LIST_FILE}")