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

# --- async-bases (input/output/io CRTP framing over a local loopback socket pair) ---
# Portable: the probes drive the async bases over qb::io::tcp loopback sockets (ephemeral port),
# not raw POSIX pipe()/socketpair() — so it builds and runs on Windows too (was WINDOWS_EXCLUDE).
qb_add_test(MODULE qb-io TIER system NAME async-bases-framing SOURCES async-bases/async-bases-framing.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)

# --- async (timers / callbacks / event loop / kernel events / connect timeout) ---
qb_add_test(MODULE qb-io TIER system NAME timer-timeout          SOURCES async/timer-timeout.cpp          DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME callback-dispatch      SOURCES async/callback-dispatch.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME timer-stress           SOURCES async/timer-stress.cpp           DEPENDS ${PROJECT_NAME} LABELS slow)
qb_add_test(MODULE qb-io TIER system NAME event-loop-lifecycle   SOURCES async/event-loop-lifecycle.cpp   DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME kernel-events          SOURCES async/kernel-events.cpp          DEPENDS ${PROJECT_NAME} LABELS signal)
# event-combined couples signal + FILE-CHANGE (ev::stat/inotify) events and delivers SIGINT via
# `system("kill -2")` (POSIX); the file watcher has no reliable Windows equivalent. Signal handling
# on Windows is already covered by kernel-events (above, not excluded). Hence WINDOWS_EXCLUDE.
qb_add_test(MODULE qb-io TIER system NAME event-combined         SOURCES async/event-combined.cpp         DEPENDS ${PROJECT_NAME} LABELS signal slow WINDOWS_EXCLUDE)
qb_add_test(MODULE qb-io TIER system NAME async-io-plan-contracts SOURCES async/async-io-plan-contracts.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME async-connect-timeout  SOURCES async/async-connect-timeout.cpp  DEPENDS ${PROJECT_NAME} REQUIRES network)
# coroutine runtime split outputs (live in async/ — driven by the scheduler/runners)
qb_add_test(MODULE qb-io TIER system NAME cancellation-awaiters   SOURCES async/cancellation-awaiters.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME deadline-combinator     SOURCES async/deadline-combinator.cpp     DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME retry-runner            SOURCES async/retry-runner.cpp            DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME scheduler-runtime       SOURCES async/scheduler-runtime.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME scheduler-stress        SOURCES async/scheduler-stress.cpp        DEPENDS ${PROJECT_NAME} LABELS coroutine slow)
qb_add_test(MODULE qb-io TIER system NAME loop-turn-fairness    SOURCES async/loop-turn-fairness.cpp    DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME coroutine-capture-safety SOURCES async/coroutine-capture-safety.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME coroutine-memory-pressure SOURCES async/coroutine-memory-pressure.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine slow)

# --- coroutine (channel async/lifetime / stream sources / timer-sleep / tcp loopback) ---
qb_add_test(MODULE qb-io TIER system NAME coro-timer-sleep       SOURCES coroutine/coro-timer-sleep.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME channel-async          SOURCES coroutine/channel-async.cpp          DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME channel-lifetime       SOURCES coroutine/channel-lifetime.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine asan)
qb_add_test(MODULE qb-io TIER system NAME stream-async-sources   SOURCES coroutine/stream-async-sources.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME tcp-connector-loopback SOURCES coroutine/tcp-connector-loopback.cpp DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME offload                SOURCES coroutine/offload.cpp                DEPENDS ${PROJECT_NAME} LABELS coroutine)

# --- tcp (loopback socket surface / connector state machine / async connector / TLS loopback) ---
qb_add_test(MODULE qb-io TIER system NAME tcp-socket-loopback        SOURCES tcp/tcp-socket-loopback.cpp        DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME socket-error-paths         SOURCES tcp/socket-error-paths.cpp         DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME transport-accept-loopback  SOURCES tcp/transport-accept-loopback.cpp  DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME secure-transport-loopback  SOURCES tcp/secure-transport-loopback.cpp  DEPENDS ${PROJECT_NAME} REQUIRES ssl network) # teardown already ordered (client reads the echo, joins, THEN closes); its exclusion mirrored ssl-socket-loopback's and lifts with it (2026-08-29, Huly QB-5) — Windows confirms at the 3.1 validation
qb_add_test(MODULE qb-io TIER system NAME tcp-connector-state-machine SOURCES tcp/tcp-connector-state-machine.cpp DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME connector-async            SOURCES tcp/connector-async.cpp            DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME ssl-socket-loopback        SOURCES tcp/ssl-socket-loopback.cpp        DEPENDS ${PROJECT_NAME} REQUIRES ssl network) # exclusion LIFTED 2026-08-29 (Huly QB-5): the Windows failure was a TEARDOWN RACE, not the busy-poll — the client disconnect()ed while the server thread still sat in the handshake poll (WSAECONNRESET, reported loudly by the de-flaked harness), and a server-side close can RST-purge bytes the client had not drained. Every race-prone case now ends through shared/ssl_fixtures' teardown_rendezvous (both directions, released by the join guard so a fatal ASSERT cannot wedge a join). Proven 30x release + 10x TSan clean on macOS; Windows confirms at the 3.1 validation.
qb_add_test(MODULE qb-io TIER system NAME ssl-context-handshake      SOURCES tcp/ssl-context-handshake.cpp      DEPENDS ${PROJECT_NAME} REQUIRES ssl network) # teardown already ordered (every test joins its server before the client socket destructs); its exclusion mirrored ssl-socket-loopback's and lifts with it (2026-08-29, Huly QB-5)
qb_add_test(MODULE qb-io TIER system NAME ssl-first-connect-errno    SOURCES tcp/ssl-first-connect-errno.cpp    DEPENDS ${PROJECT_NAME} REQUIRES ssl network) # ONE test, alone in its binary: the process's first TLS n_connect must report its errno (Windows: the first SSL_CTX_new cleared WSAGetLastError, so connector::run() read 0 and failed the connect). A single test is the only order --gtest_shuffle cannot change.
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
qb_add_test(MODULE qb-io TIER system NAME tls-worker-deadline SOURCES tls/tls-worker-deadline.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl network)

# --- session (text/binary command loopback / json / text protocol roundtrip) ---
qb_add_test(MODULE qb-io TIER system NAME text-session-loopback SOURCES session/text-session-loopback.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME session-json          SOURCES session/session-json.cpp          DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME text-protocol-roundtrip SOURCES session/text-protocol-roundtrip.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME varint-netstring-roundtrip SOURCES session/varint-netstring-roundtrip.cpp DEPENDS ${PROJECT_NAME} REQUIRES network) # Huly QB-80
# --- io (blocking + async file ops, the logger at a fork's exit) / quic (native + loopback handshake) ---
qb_add_test(MODULE qb-io TIER system NAME file-blocking-io SOURCES io/file-blocking-io.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME logger-fork-exit SOURCES io/logger-fork-exit.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER system NAME quic-handshake   SOURCES quic/quic-handshake.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
qb_add_test(MODULE qb-io TIER system NAME quic-endpoint-loop SOURCES quic/quic-endpoint-loop.cpp DEPENDS ${PROJECT_NAME} REQUIRES quic ssl network)
# Huly QB-1246: close() no longer arms its timer, so an endpoint with static storage -- closed after its thread's loop -- ends cleanly at exit.
qb_add_test(MODULE qb-io TIER system NAME quic-endpoint-static-exit SOURCES quic/quic-endpoint-static-exit.cpp DEPENDS ${PROJECT_NAME} REQUIRES quic ssl network LABELS coroutine)
if (TARGET qb-io-test-system-quic-handshake AND NOT QB_INSTALL)
    target_compile_definitions(qb-io-test-system-quic-handshake PRIVATE QB_IO_QUIC_TEST_HOOKS=1)
endif ()

# -----------------------------------------------------------------------------
# Generate self-signed SSL certificates for the loopback-TLS tests (re-pointed to the
# migrated target names). Tests load cert.pem/key.pem from their bin dir.
# -----------------------------------------------------------------------------
if (QB_HAS_SSL)
    set(SSL_CERT_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cert.pem")
    set(SSL_KEY_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/key.pem")
    # The openssl command of the OpenSSL qb found, before one on PATH (cmake/qbOpenSSL.cmake, Huly QB-1175); what it was
    # chosen from goes to qb-io-test-unit-openssl-command, which makes the choice again.
    set(_qb_openssl_defined FALSE) # OPENSSL_EXECUTABLE as the choice finds it: a variable or the cache entry, and the
    if(DEFINED OPENSSL_EXECUTABLE) # entry with its help string
        set(_qb_openssl_defined TRUE)
    endif()
    set(_qb_openssl_named "${OPENSSL_EXECUTABLE}")
    set(_qb_openssl_cached_defined FALSE)
    if(DEFINED CACHE{OPENSSL_EXECUTABLE})
        set(_qb_openssl_cached_defined TRUE)
    endif()
    set(_qb_openssl_cached "$CACHE{OPENSSL_EXECUTABLE}")
    get_property(_qb_openssl_help CACHE OPENSSL_EXECUTABLE PROPERTY HELPSTRING)
    set(_qb_openssl_chosen "<none>") # what qb last wrote there
    if(DEFINED CACHE{_QB_OPENSSL_COMMAND_CHOSEN})
        set(_qb_openssl_chosen "$CACHE{_QB_OPENSSL_COMMAND_CHOSEN}")
    endif()
    _qb_openssl_command(OPENSSL_EXECUTABLE _qb_openssl_by_hand)
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
        # BYPRODUCTS, because this POST_BUILD is the ONLY writer of bin/tests/{cert,key}.pem on
        # a host with openssl (qb_setup_test_resources drops its flat copy when this target
        # exists -- see the single-writer argument in qbFunctions.cmake). Undeclared, those two
        # files were not nodes in the ninja graph: `ninja -t clean` left them, so a certificate
        # from an earlier configuration survived a clean and could satisfy a later SSL run.
        # Declaring them also makes a reintroduced second writer a generate-time error rather
        # than a race that has to be argued about.
        add_custom_command(TARGET generate_ssl_certs POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${SSL_CERT_OUTPUT} ${SSL_KEY_OUTPUT} ${TEST_CERT_DIR}/
                BYPRODUCTS "${TEST_CERT_DIR}/cert.pem" "${TEST_CERT_DIR}/key.pem"
                COMMENT "Copying SSL certificates to test binary directory")
        foreach (_ssl_dep qb-io-test-system-ssl-socket-loopback
                qb-io-test-system-secure-transport-loopback
                qb-io-test-system-tls-text-roundtrip
                qb-io-test-system-tls-peer-verification
                qb-io-test-system-tls-starttls-upgrade
                qb-io-test-system-session-json
                qb-io-test-system-tls-worker-deadline
                qb-io-test-system-text-session-loopback
                qb-io-test-system-quic-handshake
                qb-io-test-system-quic-endpoint-loop
                qb-io-test-system-quic-endpoint-static-exit)
            if (TARGET ${_ssl_dep})
                add_dependencies(${_ssl_dep} generate_ssl_certs)
            endif ()
        endforeach ()
        qb_status_message("SSL certificate generation enabled (${OPENSSL_VERSION_OUTPUT}, ${OPENSSL_EXECUTABLE})")
    else ()
        if(_qb_openssl_by_hand)
            qb_status_message("OPENSSL_EXECUTABLE is set to '${OPENSSL_EXECUTABLE}': skipping certificate generation")
        else()
            qb_status_message("No openssl command that makes a certificate here: skipping certificate generation")
        endif()
    endif ()
    # The command used is the one already set, else the first that makes a certificate of the found OpenSSL's, the
    # default search's and PATH's -- none, generation skipped (Huly QB-1175): the test, registered either way, is handed
    # what the choice was made from -- OPENSSL_EXECUTABLE and its cache entry as they were, qb's own help string and
    # last choice, this configure's own two searches -- not its verdict, and the command the generator runs.
    set(_qb_openssl_generator "")
    if(OPENSSL_CERT_COMMAND)
        list(GET OPENSSL_CERT_COMMAND 0 _qb_openssl_generator)
    endif()
    set(_qb_openssl_where "")
    if(CMAKE_CROSSCOMPILING)
        set(_qb_openssl_where NO_CMAKE_FIND_ROOT_PATH)
    endif()
    find_program(_qb_openssl_default openssl ${_qb_openssl_where} NO_CACHE)
    find_program(_qb_openssl_path openssl NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH
                 NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
    add_test(NAME qb-io-test-unit-openssl-command
             COMMAND "${CMAKE_COMMAND}" "-DUSED=${OPENSSL_EXECUTABLE}" "-DBY_HAND=${_qb_openssl_by_hand}"
                     "-DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}" "-DVARIABLE=${_qb_openssl_named}"
                     "-DVARIABLE_DEFINED=${_qb_openssl_defined}" "-DCACHED=${_qb_openssl_cached}"
                     "-DCACHED_DEFINED=${_qb_openssl_cached_defined}" "-DCACHED_HELP=${_qb_openssl_help}"
                     "-DOWN_HELP=${_QB_OPENSSL_COMMAND_HELP}" "-DCHOSEN=${_qb_openssl_chosen}"
                     "-DGENERATOR=${_qb_openssl_generator}"
                     "-DDEFAULT=${_qb_openssl_default}" "-DPATH_ONLY=${_qb_openssl_path}"
                     "-DCROSSCOMPILING=${CMAKE_CROSSCOMPILING}"
                     "-DPROBE_DIR=${CMAKE_CURRENT_BINARY_DIR}/openssl-command-probe"
                     -P "${QB_ROOT_DIR}/tests/io/unit/build-contracts/openssl-command.cmake")
    set_tests_properties(qb-io-test-unit-openssl-command PROPERTIES LABELS "tier:unit;module:qb-io" TIMEOUT 240)
    unset(_qb_openssl_where)
    unset(_qb_openssl_default)
    unset(_qb_openssl_path)
    unset(_qb_openssl_by_hand)
    unset(_qb_openssl_named)
    unset(_qb_openssl_generator)
    unset(_qb_openssl_defined)
    unset(_qb_openssl_cached_defined)
    unset(_qb_openssl_cached)
    unset(_qb_openssl_help)
    unset(_qb_openssl_chosen)
endif ()

# A teardown completes when a handler throws, or when an acceptor has none (Huly QB-256).
qb_add_test(MODULE qb-io TIER system NAME teardown-throwing-handlers SOURCES tcp/teardown-throwing-handlers.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)

# A connect falls back through a list of addresses (Huly QB-164): the async connector, callback and coroutine
# forms, past a refused address and within an attempt's share of the deadline past a silent one; a socket handed
# over open gets one attempt; TLS falls back on TCP only and a TLS failure is final; the blocking twins.
qb_add_test(MODULE qb-io TIER system NAME connect-fallback SOURCES tcp/connect-fallback.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)

# A listener's TLS context replaced while it serves (Huly QB-205): the next accept presents the new certificate,
# a connection accepted before keeps its own, a context that failed to load is refused.
qb_add_test(MODULE qb-io TIER system NAME tls-context-reload SOURCES tls/tls-context-reload.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl network)

# OCSP through qb::io::ssl::Context's typed callbacks (Huly QB-83): the staple reaches the client check byte for byte,
# the check decides, every typed callback that throws fails closed, a typed callback and its raw setter share one slot.
qb_add_test(MODULE qb-io TIER system NAME tls-ocsp-callbacks SOURCES tls/tls-ocsp-callbacks.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl network)

# SO_REUSEPORT asked for by name (Huly QB-78): reuse_address shares no port any more, reuse_port and the listen option
# share one only when every listener asked, Windows refuses with ENOPROTOOPT, and Linux balances the accept.
qb_add_test(MODULE qb-io TIER system NAME listen-reuse-port SOURCES tcp/listen-reuse-port.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)

# The offload pool's lazy start and its sizing (Huly QB-69), ALONE in its binary: the pool is process-wide and starts
# once, so the case needs a process nothing has offloaded from -- and ctest shuffles a binary's cases (QB_TESTS_SHUFFLE),
# which made "first in offload.cpp" fail whenever another case drew the earlier slot.
qb_add_test(MODULE qb-io TIER system NAME offload-pool SOURCES coroutine/offload-pool.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)

# CoroutineScheduler::dump() and the opt-in suspension tracking (Huly QB-71): off by default the roots by name, on
# every awaiter's kind, chains root -> leaf, records that age and go with their coroutine, names that go with their frame.
qb_add_test(MODULE qb-io TIER system NAME suspension-tracking SOURCES coroutine/suspension-tracking.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)

# A standalone io object's dispose() ends the transport its connection ended with (Huly QB-1060): after a
# disconnect() the peer reads EOF and nothing is reset, unread input included; a peer's EOF leaves no CLOSE_WAIT
# socket; a hook that closes the transport again closes nothing else.
qb_add_test(MODULE qb-io TIER system NAME standalone-dispose-ends-transport SOURCES tcp/standalone-dispose-ends-transport.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
# A standalone object that ends its own connection gives its descriptor to its thread's loop, which closes it after
# the peer -- on the peer's EOF, an error, linger() or 16 MiB dropped, at most 64 at a time -- so a peer that keeps
# writing is cut instead of blocked for ever and the object's last bytes survive its destruction or its reconnection;
# a size guard or a linger of zero closes at once; an acceptor and a datagram object keep their socket (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME close-after-peer SOURCES tcp/close-after-peer.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
# The loop's side of that close: a blocking run() returns when the last closing socket is gone, the bound runs on the
# loop, a thread that ends closes what its loop held, and the number the loop closes is heard again by the connection
# that takes it -- the file to run under each QB_EV_BACKEND, which its fixture asserts (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME close-after-peer-loop SOURCES tcp/close-after-peer-loop.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
# A TLS socket that gives its descriptor away detaches its SSL object from it: a write on the transport afterwards
# reaches nothing, where it wrote a TLS record of the ended connection into whichever socket took the number (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME close-after-peer-tls SOURCES tcp/close-after-peer-tls.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl network)
# A TLS connection this side ends tells its peer so: one close_notify before the FIN, never awaited, read by a raw
# OpenSSL peer as a clean end where 3.3 gave it an unexpected EOF -- every own-ended close, a session's lingering close
# included; none when the peer ended it, before the handshake, or when queued output is dropped; a socket that cannot
# take the alert is closed without it; and a saved session resumes after either side closed (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME tls-close-notify SOURCES tls/tls-close-notify.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl network)
# A TLS read that must write first -- a ClientHello answering a HelloRequest, a server's answer to a client's
# renegotiation, a post-handshake Certificate -- goes on when the socket can take that write: the io objects watch
# writability for it and read again, where they waited for readability and the connection stalled (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME tls-read-wants-write SOURCES tls/tls-read-wants-write.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl network)
# Where the close after the peer meets the end of a thread's loop (Huly QB-1270 with QB-1246): a connection a coroutine
# frame ends while its thread's listener is destroyed is closed by that teardown, one an object with static storage
# ends after it is closed in the call (a child process), one handed over on a thread with no loop builds it, and
# clear() closes as many closing sockets as a loop may hold.
qb_add_test(MODULE qb-io TIER system NAME close-after-peer-teardown SOURCES tcp/close-after-peer-teardown.cpp DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
# Each of the three allocations of a closing socket's node fails in turn: closed at once, nothing left registered.
# Alone in its binary: it replaces the global allocation functions (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME close-after-peer-alloc SOURCES tcp/close-after-peer-alloc.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
# A server session that ends its own connection with everything written is released at once and its descriptor
# closes after the peer, on the loop; one that lingered, has output queued, was ended by its size guard or has a
# linger of zero closes with the session; close_after_deliver() lingers in time alone (a peer still sending 64 MiB
# reads the reply); a disposed session somebody still holds -- io, input or output -- is not dispatched (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME session-close-after-peer SOURCES tcp/session-close-after-peer.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)
# The loop's closes at the descriptor limit -- never above their cap under a limit of 256, given back at once to a
# connect or an accept that EMFILE refuses -- and over AF_UNIX stream sockets (Huly QB-1270).
qb_add_test(MODULE qb-io TIER system NAME close-after-peer-posix SOURCES tcp/close-after-peer-posix.cpp DEPENDS ${PROJECT_NAME} REQUIRES network WINDOWS_EXCLUDE) # POSIX-only: fork, setrlimit(RLIMIT_NOFILE), AF_UNIX

# The standalone loop drivers park in the backend instead of sleeping blind (Huly QB-1103): a reply ends
# run_until's and run_sync's idle park at poll latency (the median of 200 loopback round trips awaited on
# the loop is under 500 us, against ~2 ms on the Windows base), and a loop holding one idle io watcher makes
# at most two iterations per millisecond under run_until and run_for (ev_iteration, against ~9 per
# millisecond under the 50 us sleep on Linux and a hot spin under run_for). Serial: medians and wake
# counts are read on a quiet process.
qb_add_test(MODULE qb-io TIER system NAME loop-drivers-park SOURCES async/loop-drivers-park.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine serial REQUIRES network)

# The async connector's deadline is on the monotonic clock (Huly QB-1194): the time of day is injected into the
# connector this file instantiates (its ev_time() reads), never changed on the system; a step forward does not cut
# the deadline short, a step back does not extend it, a timeout past the clock's end is none.
qb_add_test(MODULE qb-io TIER system NAME connector-clock SOURCES tcp/connector-clock.cpp DEPENDS ${PROJECT_NAME} REQUIRES network)

# with_timeout's time left is measured on the loop's clock (Huly QB-1194): both times of day it could read -- the
# loop's (through qev's ev_now_set) and the system's (the ev_time() this file compiles) -- are injected, never changed
# on the system; a step back does not lengthen an idle timeout, a step forward does not shorten it.
qb_add_test(MODULE qb-io TIER system NAME timeout-clock SOURCES async/timeout-clock.cpp DEPENDS ${PROJECT_NAME})

# -----------------------------------------------------------------------------
# Per-backend libev event-loop test matrix (OPT-IN: QB_IO_EV_TEST_BACKENDS), re-pointed to
# the migrated target names. See the original block for rationale + valid backend values.
# Kept LAST in this file: it re-registers targets declared above it, and `if (TARGET ...)`
# passes over a target declared below it without a word, as it does over one whose REQUIRES
# gate is off.
# -----------------------------------------------------------------------------
if (QB_IO_EV_TEST_BACKENDS)
    # event-loop-lifecycle carries the matrix's anti-vacuity guard (Huly QB-81): its
    # ResolveBackendFlagsHonoursTheMatrixEnvironment case reads the QB_EV_BACKEND this variant
    # sets and FAILS if the named backend is not compiled in or did not come up at runtime --
    # the sensitive binaries never check, so without it a variant whose backend fell back
    # to auto would pass while testing another backend than the one its name promises.
    # connector-clock and timeout-clock (Huly QB-1194) hold a connect deadline and a timeout's
    # time left on the monotonic clock: each runs under every backend's wait.
    # close-after-peer, close-after-peer-loop and session-close-after-peer (Huly QB-1270): the loop closes a
    # descriptor number that the system hands out again at once, and a second watcher is started on a number
    # whose first watcher was stopped in the same pass -- each backend must hear the new socket.
    # tls-close-notify (Huly QB-1270): the alert is written before the descriptor goes to the loop, and the
    # loop then reads what a TLS peer answers as bytes -- each backend must hear that socket to its EOF.
    # tls-read-wants-write (Huly QB-1270): a TLS read that must write first moves its watcher from readability to
    # writability and back, and is fed the read again in the pass it modified it -- each backend must hear the change.
    set(QB_IO_BACKEND_SENSITIVE_TESTS
            qb-io-test-system-tcp-socket-loopback
            qb-io-test-system-async-bases-framing
            qb-io-test-system-async-connect-timeout
            qb-io-test-system-session-json
            qb-io-test-system-tls-worker-deadline
            qb-io-test-system-offload
            qb-io-test-system-connector-clock
            qb-io-test-system-timeout-clock
            qb-io-test-system-close-after-peer
            qb-io-test-system-close-after-peer-loop
            qb-io-test-system-session-close-after-peer
            qb-io-test-system-tls-close-notify
            qb-io-test-system-tls-read-wants-write
            qb-io-test-system-event-loop-lifecycle)
    foreach (_qb_tgt ${QB_IO_BACKEND_SENSITIVE_TESTS})
        if (TARGET ${_qb_tgt})
            set(_qb_lock "evmx_${_qb_tgt}")
            set_tests_properties(${_qb_tgt} PROPERTIES RESOURCE_LOCK "${_qb_lock}")
            get_test_property(${_qb_tgt} LABELS _qb_base_labels)
            get_test_property(${_qb_tgt} TIMEOUT _qb_base_timeout)
            foreach (_qb_be ${QB_IO_EV_TEST_BACKENDS})
                add_test(NAME ${_qb_tgt}-ev-${_qb_be} COMMAND ${_qb_tgt})
                set_tests_properties(${_qb_tgt}-ev-${_qb_be} PROPERTIES
                        ENVIRONMENT "QB_EV_BACKEND=${_qb_be}"
                        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_qb_tgt}>"
                        RESOURCE_LOCK "${_qb_lock}"
                        LABELS "${_qb_base_labels};ev-backend;ev-backend-${_qb_be}"
                        TIMEOUT "${_qb_base_timeout}")
            endforeach ()
        endif ()
    endforeach ()
    qb_status_message("Per-backend ev test matrix ENABLED: [${QB_IO_EV_TEST_BACKENDS}]")
endif ()
