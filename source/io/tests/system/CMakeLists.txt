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
# REQUIRES ssl|quic compile-gate on QB_HAS_*; REQUIRES network labels loopback tests;
# WINDOWS_EXCLUDE skips POSIX-only raw-syscall harnesses on Windows.
# -----------------------------------------------------------------------------

# --- async-bases (input/output/io CRTP framing over local pipe/socketpair) ---
qb_add_test(MODULE qb-io TIER system NAME async-bases-framing SOURCES async-bases/async-bases-framing.cpp DEPENDS ${PROJECT_NAME} WINDOWS_EXCLUDE)

# --- async (timers / callbacks / event loop / kernel events / connect timeout) ---
qb_add_test(MODULE qb-io TIER system NAME timer-timeout          SOURCES async/timer-timeout.cpp          DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME callback-dispatch      SOURCES async/callback-dispatch.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME timer-stress           SOURCES async/timer-stress.cpp           DEPENDS ${PROJECT_NAME} LABELS slow)
qb_add_test(MODULE qb-io TIER system NAME event-loop-lifecycle   SOURCES async/event-loop-lifecycle.cpp   DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME kernel-events          SOURCES async/kernel-events.cpp          DEPENDS ${PROJECT_NAME} LABELS signal)
qb_add_test(MODULE qb-io TIER system NAME event-combined         SOURCES async/event-combined.cpp         DEPENDS ${PROJECT_NAME} LABELS signal slow WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME async-io-plan-contracts SOURCES async/async-io-plan-contracts.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME async-connect-timeout  SOURCES async/async-connect-timeout.cpp  DEPENDS ${PROJECT_NAME} REQUIRES network)
# coroutine runtime split outputs (live in async/ — driven by the scheduler/runners)
qb_add_test(MODULE qb-io TIER system NAME cancellation-awaiters   SOURCES async/cancellation-awaiters.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME deadline-combinator     SOURCES async/deadline-combinator.cpp     DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME retry-runner            SOURCES async/retry-runner.cpp            DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME scheduler-runtime       SOURCES async/scheduler-runtime.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME scheduler-stress        SOURCES async/scheduler-stress.cpp        DEPENDS ${PROJECT_NAME} LABELS coroutine slow)
qb_add_test(MODULE qb-io TIER system NAME coroutine-capture-safety SOURCES async/coroutine-capture-safety.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME coroutine-memory-pressure SOURCES async/coroutine-memory-pressure.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine slow)

# --- coroutine (channel async/lifetime / stream sources / timer-sleep / tcp loopback) ---
qb_add_test(MODULE qb-io TIER system NAME coro-timer-sleep       SOURCES coroutine/coro-timer-sleep.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME channel-async          SOURCES coroutine/channel-async.cpp          DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME channel-lifetime       SOURCES coroutine/channel-lifetime.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine asan)
qb_add_test(MODULE qb-io TIER system NAME stream-async-sources   SOURCES coroutine/stream-async-sources.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME tcp-connector-loopback SOURCES coroutine/tcp-connector-loopback.cpp DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)

# --- tcp (loopback socket surface / connector state machine / async connector / TLS loopback) ---
qb_add_test(MODULE qb-io TIER system NAME tcp-socket-loopback        SOURCES tcp/tcp-socket-loopback.cpp        DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME socket-error-paths         SOURCES tcp/socket-error-paths.cpp         DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME transport-accept-loopback  SOURCES tcp/transport-accept-loopback.cpp  DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME secure-transport-loopback  SOURCES tcp/secure-transport-loopback.cpp  DEPENDS ${PROJECT_NAME} REQUIRES ssl network WINDOWS_EXCLUDE) # POSIX-only TLS busy-poll handshake harness, mirrors ssl-socket-loopback
qb_add_test(MODULE qb-io TIER system NAME tcp-connector-state-machine SOURCES tcp/tcp-connector-state-machine.cpp DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME connector-async            SOURCES tcp/connector-async.cpp            DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME ssl-socket-loopback        SOURCES tcp/ssl-socket-loopback.cpp        DEPENDS ${PROJECT_NAME} REQUIRES ssl network WINDOWS_EXCLUDE) # POSIX-only harness: both sides BUSY-POLL the non-blocking TLS handshake; on Windows the client completes + disconnect()s before the server's poll-loop observes completion → server recv hits WSAECONNRESET/ABORTED. Production async-event-loop SSL works on Windows (qbm-http). See follow-up.
qb_add_test(MODULE qb-io TIER system NAME accept-transient-errors        SOURCES tcp/accept-transient-errors.cpp        DEPENDS ${PROJECT_NAME} REQUIRES network WINDOWS_EXCLUDE) # POSIX-only: setrlimit(RLIMIT_NOFILE)/dup to force EMFILE remap
qb_add_test(MODULE qb-io TIER system NAME io-handler-broadcast-reentrancy SOURCES tcp/io-handler-broadcast-reentrancy.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)

# --- udp (socket options / transport datagram / datagram loopback) ---
qb_add_test(MODULE qb-io TIER system NAME socket-options    SOURCES udp/socket-options.cpp    DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME transport-datagram SOURCES udp/transport-datagram.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME udp-datagram      SOURCES udp/udp-datagram.cpp      DEPENDS ${PROJECT_NAME} REQUIRES network)

# --- tls (loopback TLS text / peer verification / starttls upgrade) ---
qb_add_test(MODULE qb-io TIER system NAME tls-text-roundtrip    SOURCES tls/tls-text-roundtrip.cpp    DEPENDS ${PROJECT_NAME} REQUIRES ssl network)
qb_add_test(MODULE qb-io TIER system NAME tls-peer-verification SOURCES tls/tls-peer-verification.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl network)
qb_add_test(MODULE qb-io TIER system NAME tls-starttls-upgrade  SOURCES tls/tls-starttls-upgrade.cpp  DEPENDS ${PROJECT_NAME} REQUIRES ssl network)

# --- session (text/binary command loopback / json / text protocol roundtrip) ---
qb_add_test(MODULE qb-io TIER system NAME text-session-loopback SOURCES session/text-session-loopback.cpp DEPENDS ${PROJECT_NAME} REQUIRES network WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME session-json          SOURCES session/session-json.cpp          DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME text-protocol-roundtrip SOURCES session/text-protocol-roundtrip.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)

# --- io (blocking + async file ops) / quic (native + loopback handshake) ---
qb_add_test(MODULE qb-io TIER system NAME file-blocking-io SOURCES io/file-blocking-io.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME quic-handshake   SOURCES quic/quic-handshake.cpp DEPENDS ${PROJECT_NAME} REQUIRES quic ssl network)

# -----------------------------------------------------------------------------
# Generate self-signed SSL certificates for the loopback-TLS tests (re-pointed to the
# migrated target names). Tests load cert.pem/key.pem from their bin dir.
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
        foreach (_ssl_dep qb-io-test-system-ssl-socket-loopback
                qb-io-test-system-secure-transport-loopback
                qb-io-test-system-tls-text-roundtrip
                qb-io-test-system-tls-peer-verification
                qb-io-test-system-tls-starttls-upgrade
                qb-io-test-system-session-json
                qb-io-test-system-text-session-loopback
                qb-io-test-system-quic-handshake)
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
# Per-backend libev event-loop test matrix (OPT-IN: QB_IO_EV_TEST_BACKENDS), re-pointed to
# the migrated target names. See the original block for rationale + valid backend values.
# -----------------------------------------------------------------------------
if (QB_IO_EV_TEST_BACKENDS)
    set(QB_IO_BACKEND_SENSITIVE_TESTS
            qb-io-test-system-tcp-socket-loopback
            qb-io-test-system-async-bases-framing
            qb-io-test-system-async-connect-timeout
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
