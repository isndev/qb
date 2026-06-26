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
# qb-io SYSTEM tests — in-process event loop bound to kernel objects (fds, signals,
# loopback socket pair, local TLS/QUIC). NO external daemon.
# REQUIRES ssl|compression compile-gate on QB_HAS_*; REQUIRES network labels loopback
# tests; WINDOWS_EXCLUDE skips POSIX-only raw-syscall harnesses on Windows.
#
# NOTE: files registered flat at the bottom (SPLIT_PENDING) are large monoliths being
# decomposed per docs/tests-audit/_spec-qb-io.md §2; registered as-is (temp tier) so the
# suite stays green until the split wave replaces them. Their target names are
# qb-io-test-<tier>-<name> already, so the cert/ev-matrix wiring below survives the split.
# -----------------------------------------------------------------------------

# --- renamed (already in their final tiered home) ---------------------------
qb_add_test(MODULE qb-io TIER system NAME async-bases-framing SOURCES async-bases/async-bases-framing.cpp DEPENDS ${PROJECT_NAME} WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME tcp-socket-loopback SOURCES tcp/tcp-socket-loopback.cpp         DEPENDS ${PROJECT_NAME} REQUIRES network WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME kernel-events       SOURCES async/kernel-events.cpp             DEPENDS ${PROJECT_NAME} LABELS signal)
qb_add_test(MODULE qb-io TIER system NAME event-combined      SOURCES async/event-combined.cpp            DEPENDS ${PROJECT_NAME} LABELS signal slow WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME coro-timer-sleep    SOURCES coroutine/coro-timer-sleep.cpp      DEPENDS ${PROJECT_NAME} LABELS coroutine)

# --- coroutine runtime split outputs (channel/stream async + scheduler/retry/cancellation runners) ---
qb_add_test(MODULE qb-io TIER system NAME channel-async           SOURCES coroutine/channel-async.cpp           DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME channel-lifetime        SOURCES coroutine/channel-lifetime.cpp        DEPENDS ${PROJECT_NAME} LABELS coroutine asan)
qb_add_test(MODULE qb-io TIER system NAME stream-async-sources    SOURCES coroutine/stream-async-sources.cpp    DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME tcp-connector-loopback  SOURCES coroutine/tcp-connector-loopback.cpp  DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME cancellation-awaiters   SOURCES async/cancellation-awaiters.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME deadline-combinator     SOURCES async/deadline-combinator.cpp         DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME retry-runner            SOURCES async/retry-runner.cpp                DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME scheduler-runtime       SOURCES async/scheduler-runtime.cpp           DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME scheduler-stress        SOURCES async/scheduler-stress.cpp            DEPENDS ${PROJECT_NAME} LABELS coroutine slow)
qb_add_test(MODULE qb-io TIER system NAME coroutine-capture-safety SOURCES async/coroutine-capture-safety.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME coroutine-memory-pressure SOURCES async/coroutine-memory-pressure.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine slow)
qb_add_test(MODULE qb-io TIER system NAME tcp-connector-state-machine SOURCES tcp/tcp-connector-state-machine.cpp DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)

# --- SPLIT_PENDING: pure-logic monoliths (true tier=unit; decomposed by the split wave) ---
qb_add_test(MODULE qb-io TIER unit NAME uri-json          SOURCES test-uri-json.cpp          DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME cpu               SOURCES test-cpu.cpp               DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME buffered-io       SOURCES test-buffered-io.cpp       DEPENDS ${PROJECT_NAME} WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER unit NAME file-operations   SOURCES test-file-operations.cpp   DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME stream-operations SOURCES test-stream-operations.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME crypto            SOURCES test-crypto.cpp            DEPENDS ${PROJECT_NAME} REQUIRES ssl)

# --- SPLIT_PENDING: system monoliths (loopback / event loop) ---------------
qb_add_test(MODULE qb-io TIER system NAME io                 SOURCES test-io.cpp                 DEPENDS ${PROJECT_NAME} REQUIRES network WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME io-plan            SOURCES test-io-plan.cpp            DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME quic               SOURCES test-quic.cpp               DEPENDS ${PROJECT_NAME} LABELS quic)
qb_add_test(MODULE qb-io TIER system NAME connection-timeout SOURCES test-connection-timeout.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME async-io           SOURCES test-async-io.cpp           DEPENDS ${PROJECT_NAME} REQUIRES network LABELS signal)
qb_add_test(MODULE qb-io TIER system NAME udp-socket         SOURCES test-udp-socket.cpp         DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME session-text       SOURCES test-session-text.cpp       DEPENDS ${PROJECT_NAME} REQUIRES network WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME session-json       SOURCES test-session-json.cpp       DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME ssl-socket         SOURCES test-ssl-socket.cpp         DEPENDS ${PROJECT_NAME} REQUIRES ssl network WINDOWS_EXCLUDE)

# -----------------------------------------------------------------------------
# Generate self-signed SSL certificates for the loopback-TLS tests (unchanged logic,
# re-pointed to the new target names). Tests load cert.pem/key.pem from their bin dir.
# -----------------------------------------------------------------------------
if (QB_HAS_SSL)
    set(SSL_CERT_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cert.pem")
    set(SSL_KEY_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/key.pem")
    find_program(OPENSSL_EXECUTABLE openssl)
    if (OPENSSL_EXECUTABLE)
        execute_process(COMMAND ${OPENSSL_EXECUTABLE} version -v
                OUTPUT_VARIABLE OPENSSL_VERSION_OUTPUT ERROR_VARIABLE OPENSSL_VERSION_ERROR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${OPENSSL_VERSION_OUTPUT}")
        if ((CMAKE_MATCH_1 GREATER 1) OR (CMAKE_MATCH_1 EQUAL 1 AND CMAKE_MATCH_2 GREATER 1) OR
            (CMAKE_MATCH_1 EQUAL 1 AND CMAKE_MATCH_2 EQUAL 1 AND CMAKE_MATCH_3 GREATER_EQUAL 1))
            set(OPENSSL_CERT_COMMAND ${OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048
                    -keyout ${SSL_KEY_OUTPUT} -out ${SSL_CERT_OUTPUT} -days 365 -nodes
                    -subj "/CN=localhost/O=QB Tests/C=US" -addext "subjectAltName = DNS:localhost")
        else ()
            set(OPENSSL_CERT_COMMAND ${OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048
                    -keyout ${SSL_KEY_OUTPUT} -out ${SSL_CERT_OUTPUT} -days 365 -nodes
                    -subj "/CN=localhost/O=QB Tests/C=US")
        endif ()
        add_custom_command(OUTPUT ${SSL_CERT_OUTPUT} ${SSL_KEY_OUTPUT}
                COMMAND ${OPENSSL_CERT_COMMAND}
                COMMENT "Generating self-signed SSL certificate for tests" VERBATIM)
        add_custom_target(generate_ssl_certs DEPENDS ${SSL_CERT_OUTPUT} ${SSL_KEY_OUTPUT})
        set(TEST_CERT_DIR "${CMAKE_BINARY_DIR}/bin/tests")
        file(MAKE_DIRECTORY ${TEST_CERT_DIR})
        add_custom_command(TARGET generate_ssl_certs POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${SSL_CERT_OUTPUT} ${SSL_KEY_OUTPUT} ${TEST_CERT_DIR}/
                COMMENT "Copying SSL certificates to test binary directory")
        # Re-pointed to the migrated target names (only wire targets that exist on this platform).
        foreach (_ssl_dep qb-io-test-system-async-io
                qb-io-test-system-ssl-socket
                qb-io-test-system-session-text
                qb-io-test-system-session-json)
            if (TARGET ${_ssl_dep})
                add_dependencies(${_ssl_dep} generate_ssl_certs)
            endif ()
        endforeach ()
        qb_status_message("SSL certificate generation enabled (OpenSSL ${OPENSSL_VERSION_OUTPUT})")
    else ()
        qb_status_message("OpenSSL executable not found, skipping certificate generation")
    endif ()
endif ()

# -----------------------------------------------------------------------------
# Per-backend libev event-loop test matrix (OPT-IN: QB_IO_EV_TEST_BACKENDS). Re-runs the
# I/O-heavy, backend-sensitive tests under additional libev backends. Re-pointed to the
# migrated target names. See the original block for the rationale and valid backend values.
# -----------------------------------------------------------------------------
if (QB_IO_EV_TEST_BACKENDS)
    set(QB_IO_BACKEND_SENSITIVE_TESTS
            qb-io-test-system-io
            qb-io-test-system-async-bases-framing
            qb-io-test-system-connection-timeout
            qb-io-test-system-session-json)
    foreach (_qb_tgt ${QB_IO_BACKEND_SENSITIVE_TESTS})
        if (TARGET ${_qb_tgt})
            set(_qb_lock "evmx_${_qb_tgt}")
            set_tests_properties(${_qb_tgt} PROPERTIES RESOURCE_LOCK "${_qb_lock}")
            foreach (_qb_be ${QB_IO_EV_TEST_BACKENDS})
                add_test(NAME ${_qb_tgt}-ev-${_qb_be} COMMAND ${_qb_tgt})
                set_tests_properties(${_qb_tgt}-ev-${_qb_be} PROPERTIES
                        ENVIRONMENT "QB_EV_BACKEND=${_qb_be}"
                        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_qb_tgt}>"
                        RESOURCE_LOCK "${_qb_lock}"
                        LABELS "module:qb-io;ev-backend;ev-backend-${_qb_be}")
            endforeach ()
        endif ()
    endforeach ()
    qb_status_message("Per-backend ev test matrix ENABLED: [${QB_IO_EV_TEST_BACKENDS}]")
endif ()
