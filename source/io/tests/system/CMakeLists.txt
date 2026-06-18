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
        test-buffered-io.cpp
        test-file-operations.cpp
        test-stream-operations.cpp
        test-async-io.cpp
        test-async-io-bases.cpp
        test-cpu.cpp
        test-tcp-socket.cpp
        test-udp-socket.cpp
        test-uri-json.cpp
        test-session-text.cpp
        test-session-json.cpp
        test-io-plan.cpp
        test-quic.cpp
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
            test-ssl-socket.cpp
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
        add_dependencies(${PROJECT_NAME}-gtest-test-ssl-socket generate_ssl_certs)
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

# -----------------------------------------------------------------------------
# Per-backend libev event-loop test matrix  (OPT-IN — disabled by default)
# -----------------------------------------------------------------------------
# Re-runs the I/O-heavy, backend-sensitive tests under additional libev backends by
# forcing QB_EV_BACKEND (resolved by qb::io::async::listener). This is OFF by
# default on purpose: the normal suite above already runs every test once on the
# auto-selected backend (the one that actually ships: kqueue on macOS, epoll on
# Linux, wepoll on Windows), and the cross-backend PERFORMANCE comparison lives in
# the `qb-io-benchmark-ev-backends` benchmark. So the dev pipeline shouldn't pay to
# re-run the suite on extra backends every build — but the capability is here when
# you need it (CI backend matrix, or debugging a backend-specific bug).
#
# Enable by setting QB_IO_EV_TEST_BACKENDS to a ';'-separated list, e.g.:
#   cmake -DQB_IO_EV_TEST_BACKENDS="select;poll"                       # macOS/Linux alternatives
#   cmake -DQB_IO_EV_TEST_BACKENDS="select;poll;iouring;linuxaio"      # native Linux host
#
# Valid values (pick the ones available on your host):
#   select   - everywhere    (capped at FD_SETSIZE, ~1024)
#   poll     - Linux, macOS  (not Windows)
#   epoll    - Linux (default there); Windows = wepoll
#   kqueue   - macOS / BSD   (default on macOS)
#   iouring  - Linux 5.13+   (native host, or docker --security-opt seccomp=unconfined)
#   linuxaio - Linux
# (No need to list the default backend — it is already covered by the normal run.)
if(QB_IO_EV_TEST_BACKENDS)
    # SSL-cert-free, loop-heavy tests (avoid cert working-dir coupling in the matrix).
    set(QB_IO_BACKEND_SENSITIVE_TESTS
            test-io
            test-async-io-bases
            test-connection-timeout
            test-session-json
    )

    foreach (_qb_t ${QB_IO_BACKEND_SENSITIVE_TESTS})
        set(_qb_tgt ${PROJECT_NAME}-gtest-${_qb_t})
        if (TARGET ${_qb_tgt})
            # Localhost network tests: the base (auto) run and every backend variant
            # exercise the SAME binary, often on the same ports. A shared RESOURCE_LOCK
            # stops ctest from scheduling two variants of one test concurrently under
            # `--parallel` (which caused port/accept flakes). Distinct tests still run
            # in parallel.
            set(_qb_lock "evmx_${_qb_t}")
            set_tests_properties(${_qb_tgt} PROPERTIES RESOURCE_LOCK "${_qb_lock}")
            foreach (_qb_be ${QB_IO_EV_TEST_BACKENDS})
                add_test(NAME ${_qb_tgt}-ev-${_qb_be} COMMAND ${_qb_tgt})
                set_tests_properties(${_qb_tgt}-ev-${_qb_be} PROPERTIES
                        ENVIRONMENT "QB_EV_BACKEND=${_qb_be}"
                        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_qb_tgt}>"
                        RESOURCE_LOCK "${_qb_lock}"
                        LABELS "qb-tests;ev-backend;ev-backend-${_qb_be}")
            endforeach ()
        endif ()
    endforeach ()

    qb_status_message("Per-backend ev test matrix ENABLED: [${QB_IO_EV_TEST_BACKENDS}]")
endif()
