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
# qb IO System Tests - CMake Configuration
#
# This file configures the system tests for the qb IO component.
# It defines test targets for core IO functionality and optional components
# that depend on available libraries (ZLIB, OpenSSL).
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Test targets definition
# -----------------------------------------------------------------------------

# Core IO functionality tests
set(CORE_TESTS
    test-event.cpp
    test-io.cpp
    test-session-text.cpp
    test-event-combined.cpp
    test-connection-timeout.cpp
)

# Create core test targets
foreach(TEST_SOURCE ${CORE_TESTS})
    # Extract test name from filename (removing extension)
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    
    # Create test target with a descriptive name
    cxx_gtest(
        ${PROJECT_NAME}-gtest-${TEST_NAME}  # Target name
        "${PROJECT_NAME}"                   # Library dependency
        ${TEST_SOURCE}                      # Source file
    )
    
    # Set properties for the test target (optional)
    set_target_properties(${PROJECT_NAME}-gtest-${TEST_NAME} PROPERTIES
        FOLDER "Tests/IO/System"
    )
endforeach()

# -----------------------------------------------------------------------------
# Optional feature tests
# -----------------------------------------------------------------------------

# Optional crypto tests (requires OpenSSL)
if (OPENSSL_FOUND)
    # List of crypto tests
    set(CRYPTO_TESTS
        test-crypto.cpp
        test-crypto-advanced.cpp
        test-crypto-asymmetric.cpp
    )
    
    # Generate targets for each crypto test
    foreach(TEST_SOURCE ${CRYPTO_TESTS})
        # Extract test name from filename
        get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
        
        cxx_gtest(
            ${PROJECT_NAME}-gtest-${TEST_NAME}
            "${PROJECT_NAME}"
            ${TEST_SOURCE}
        )
        
        set_target_properties(${PROJECT_NAME}-gtest-${TEST_NAME} PROPERTIES
            FOLDER "Tests/IO/System"
        )
    endforeach()
    
    message(STATUS "[IO Tests] Building OpenSSL crypto tests")
endif ()

# Optional compression tests (requires ZLIB)
if (ZLIB_FOUND)
    # Liste des tests de compression
    set(COMPRESSION_TESTS
        test-compression.cpp
        test-compression-levels.cpp
    )
    
    # Générer les cibles pour chaque test de compression
    foreach(TEST_SOURCE ${COMPRESSION_TESTS})
        # Extract test name from filename
        get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
        
        cxx_gtest(
            ${PROJECT_NAME}-gtest-${TEST_NAME}
            "${PROJECT_NAME}"
            ${TEST_SOURCE}
        )
        
        set_target_properties(${PROJECT_NAME}-gtest-${TEST_NAME} PROPERTIES
            FOLDER "Tests/IO/System"
        )
    endforeach()
    
    message(STATUS "[IO Tests] Building ZLIB compression tests")
endif ()

# -----------------------------------------------------------------------------
# Test resources setup
# -----------------------------------------------------------------------------

# Copy SSL resources for tests if OpenSSL is available
if (OPENSSL_FOUND)
    message(STATUS "[IO Tests] Copying SSL test resources")
    file(COPY ${QB_SSL_RESOURCES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif ()