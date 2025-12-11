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
        test-event-combined.cpp
        test-connection-timeout.cpp
        test-file-operations.cpp
        test-stream-operations.cpp
        test-async-io.cpp
        test-session-text.cpp
        test-session-json.cpp
)

# Create core test targets
foreach (TEST_SOURCE ${CORE_TESTS})
    # Extract test name from filename (removing extension)
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

    # Create test target with a descriptive name
    qb_add_test(
        NAME ${PROJECT_NAME}-gtest-${TEST_NAME}
        SOURCES ${TEST_SOURCE}
        DEPENDS ${PROJECT_NAME}
    )

    # Set properties for the test target (optional)
    set_target_properties(${PROJECT_NAME}-gtest-${TEST_NAME} PROPERTIES
            FOLDER "Tests/IO/System"
    )
endforeach ()

# -----------------------------------------------------------------------------
# Optional feature tests
# -----------------------------------------------------------------------------

# Optional crypto tests (requires OpenSSL)
if(QB_HAS_SSL)
    # List of crypto tests
    set(CRYPTO_TESTS
            test-crypto.cpp
            test-crypto-advanced.cpp
            test-crypto-asymmetric.cpp
            test-crypto-jwt.cpp
    )

    # Generate targets for each crypto test
    foreach (TEST_SOURCE ${CRYPTO_TESTS})
        # Extract test name from filename
        get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

        qb_add_test(
            NAME ${PROJECT_NAME}-gtest-${TEST_NAME}
            SOURCES ${TEST_SOURCE}
            DEPENDS ${PROJECT_NAME}
        )

        set_target_properties(${PROJECT_NAME}-gtest-${TEST_NAME} PROPERTIES
                FOLDER "Tests/IO/System"
        )
    endforeach ()

    # -----------------------------------------------------------------------------
    # Generate SSL certificates for tests
    # -----------------------------------------------------------------------------
    # Define output paths for certificates
    set(SSL_CERT_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cert.pem")
    set(SSL_KEY_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/key.pem")

    # Function to check if OpenSSL is available
    find_program(OPENSSL_EXECUTABLE openssl)

    if (OPENSSL_EXECUTABLE)
        # Check OpenSSL version to determine command options
        execute_process(
                COMMAND ${OPENSSL_EXECUTABLE} version -v
                OUTPUT_VARIABLE OPENSSL_VERSION_OUTPUT
                ERROR_VARIABLE OPENSSL_VERSION_ERROR
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" OPENSSL_VERSION_MATCH "${OPENSSL_VERSION_OUTPUT}")
        set(OPENSSL_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(OPENSSL_VERSION_MINOR ${CMAKE_MATCH_2})
        set(OPENSSL_VERSION_PATCH ${CMAKE_MATCH_3})

        # For OpenSSL >= 1.1.1, use -addext option
        if ((OPENSSL_VERSION_MAJOR GREATER 1) OR
        (OPENSSL_VERSION_MAJOR EQUAL 1 AND OPENSSL_VERSION_MINOR GREATER 1) OR
        (OPENSSL_VERSION_MAJOR EQUAL 1 AND OPENSSL_VERSION_MINOR EQUAL 1 AND OPENSSL_VERSION_PATCH GREATER_EQUAL 1))
            set(OPENSSL_CERT_COMMAND
                    ${OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048 -keyout ${SSL_KEY_OUTPUT}
                    -out ${SSL_CERT_OUTPUT} -days 365 -nodes
                    -subj "/CN=localhost/O=QB Tests/C=US"
                    -addext "subjectAltName = DNS:localhost"
            )
        else ()
            # Older OpenSSL versions don't support -addext
            set(OPENSSL_CERT_COMMAND
                    ${OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048 -keyout ${SSL_KEY_OUTPUT}
                    -out ${SSL_CERT_OUTPUT} -days 365 -nodes
                    -subj "/CN=localhost/O=QB Tests/C=US"
            )
        endif ()

        # Define command to generate SSL certificates
        add_custom_command(
                OUTPUT ${SSL_CERT_OUTPUT} ${SSL_KEY_OUTPUT}
                COMMAND ${OPENSSL_CERT_COMMAND}
                COMMENT "Generating self-signed SSL certificate for tests"
                VERBATIM
        )

        # Define custom target for certificate generation
        add_custom_target(generate_ssl_certs DEPENDS ${SSL_CERT_OUTPUT} ${SSL_KEY_OUTPUT})

        # Create a directory structure for the certificates
        # Note: Test binaries are in ${CMAKE_BINARY_DIR}/bin/tests (set by qb_add_test)
        set(TEST_CERT_DIR "${CMAKE_BINARY_DIR}/bin/tests")
        file(MAKE_DIRECTORY ${TEST_CERT_DIR})

        # Copy SSL certificates to where test binaries are located
        add_custom_command(
                TARGET generate_ssl_certs
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                ${SSL_CERT_OUTPUT} ${SSL_KEY_OUTPUT}
                ${TEST_CERT_DIR}/
                COMMENT "Copying SSL certificates to test binary directory"
        )

        # Make ssl-dependent tests depend on certificate generation
        add_dependencies(${PROJECT_NAME}-gtest-test-async-io generate_ssl_certs)
        add_dependencies(${PROJECT_NAME}-gtest-test-session-text generate_ssl_certs)

        # Tests using SSL will look for certificates in their runtime directory
        qb_status_message("SSL certificate generation enabled (OpenSSL ${OPENSSL_VERSION_OUTPUT})")
    else ()
        qb_status_message("OpenSSL executable not found, skipping certificate generation")
    endif ()

    qb_status_message("Building OpenSSL crypto tests")
endif ()

# Optional compression tests (requires ZLIB)
if(QB_HAS_COMPRESSION)
    # Liste des tests de compression
    set(COMPRESSION_TESTS
            test-compression.cpp
            test-compression-levels.cpp
    )

    # Générer les cibles pour chaque test de compression
    foreach (TEST_SOURCE ${COMPRESSION_TESTS})
        # Extract test name from filename
        get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

        qb_add_test(
            NAME ${PROJECT_NAME}-gtest-${TEST_NAME}
            SOURCES ${TEST_SOURCE}
            DEPENDS ${PROJECT_NAME}
        )

        set_target_properties(${PROJECT_NAME}-gtest-${TEST_NAME} PROPERTIES
                FOLDER "Tests/IO/System"
        )
    endforeach ()

    qb_status_message("Building ZLIB compression tests")
endif ()

# -----------------------------------------------------------------------------
# Test resources setup
# -----------------------------------------------------------------------------

# SSL resources are automatically copied by qb_setup_test_resources()
# which is called in qb/CMakeLists.txt. Tests will find them in their working directory.
# Note: Generated SSL certificates (generate_ssl_certs target) are handled separately above.