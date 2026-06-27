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
# qb-io UNIT tests — pure logic: NO daemon, NO socket, NO qb::Main. The coroutine
# scheduler/event-loop pump is part of the SUT (driven with no socket → still unit).
# REQUIRES ssl compile-gates on QB_HAS_SSL (crypto needs OpenSSL to LINK, not a TLS socket
# → labelled ssl, never network/live).
# Run: ctest -L 'tier:unit' -L 'module:qb-io' --parallel "$(nproc)"
# -----------------------------------------------------------------------------

# --- core (uri / json-pipe / allocator / endpoint / udp-identity / cpu / raii / uuid) ---
qb_add_test(MODULE qb-io TIER unit NAME uri-parse         SOURCES core/uri-parse.cpp         DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME json-pipe         SOURCES core/json-pipe.cpp         DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME pipe-allocator    SOURCES core/pipe-allocator.cpp    DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME endpoint-address  SOURCES core/endpoint-address.cpp  DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME udp-identity      SOURCES core/udp-identity.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME cpu-topology      SOURCES core/cpu-topology.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME raii-helpers      SOURCES core/raii-helpers.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME uuid-threadsafety SOURCES core/uuid-threadsafety.cpp DEPENDS ${PROJECT_NAME})

# --- protocol (buffered-io / base framing / json depth / json-session-parse / quic state) ---
qb_add_test(MODULE qb-io TIER unit NAME buffered-io-session      SOURCES protocol/buffered-io-session.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME protocol-base-framing    SOURCES protocol/protocol-base-framing.cpp    DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME json-depth-guard         SOURCES protocol/json-depth-guard.cpp         DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME json-session-parse       SOURCES protocol/json-session-parse.cpp       DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME quic-protocol-statemachine SOURCES protocol/quic-protocol-statemachine.cpp DEPENDS ${PROJECT_NAME})

# --- stream (templates over scripted transport / file-backed / limits) ---
qb_add_test(MODULE qb-io TIER unit NAME stream-templates SOURCES stream/stream-templates.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME stream-file-io   SOURCES stream/stream-file-io.cpp   DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME stream-limits    SOURCES stream/stream-limits.cpp    DEPENDS ${PROJECT_NAME})

# --- file (sys::file / pipe transfer / self-locate) ---
qb_add_test(MODULE qb-io TIER unit NAME file-sys           SOURCES file/file-sys.cpp           DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME file-error-paths   SOURCES file/file-error-paths.cpp   DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME file-pipe-transfer SOURCES file/file-pipe-transfer.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME self-locate        SOURCES file/self-locate.cpp        DEPENDS ${PROJECT_NAME})

# --- crypto (OpenSSL link-gated) ---
qb_add_test(MODULE qb-io TIER unit NAME crypto-primitives SOURCES crypto/crypto-primitives.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl)
qb_add_test(MODULE qb-io TIER unit NAME kdf-and-tokens    SOURCES crypto/kdf-and-tokens.cpp    DEPENDS ${PROJECT_NAME} REQUIRES ssl)
qb_add_test(MODULE qb-io TIER unit NAME asymmetric-keys   SOURCES crypto/asymmetric-keys.cpp   DEPENDS ${PROJECT_NAME} REQUIRES ssl)
qb_add_test(MODULE qb-io TIER unit NAME crypto-jwt        SOURCES crypto/crypto-jwt.cpp        DEPENDS ${PROJECT_NAME} REQUIRES ssl)
qb_add_test(MODULE qb-io TIER unit NAME crypto-reachable-edges SOURCES crypto/crypto-reachable-edges.cpp DEPENDS ${PROJECT_NAME} REQUIRES ssl)

# --- compression (zlib gzip/deflate, link-gated on QB_HAS_COMPRESSION) ---
qb_add_test(MODULE qb-io TIER unit NAME compression-codec SOURCES compression/compression-codec.cpp DEPENDS ${PROJECT_NAME} REQUIRES compression)

# --- ssl (SSL_CTX config, no socket) / async (transport traits) / quic (FakeBackend adapter) ---
qb_add_test(MODULE qb-io TIER unit NAME ssl-context-config     SOURCES ssl/ssl-context-config.cpp     DEPENDS ${PROJECT_NAME} REQUIRES ssl)
qb_add_test(MODULE qb-io TIER unit NAME async-transport-traits SOURCES async/async-transport-traits.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-io TIER unit NAME quic-adapter           SOURCES quic/quic-adapter.cpp          DEPENDS ${PROJECT_NAME} LABELS quic)

# --- coroutine (pure: scheduler/awaiter/generator/channel/sync/combinator/scope/etc.) ---
qb_add_test(MODULE qb-io TIER unit NAME scheduler-lifecycle         SOURCES coroutine/scheduler-lifecycle.cpp         DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME awaiter-protocol            SOURCES coroutine/awaiter-protocol.cpp            DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME exception-propagation       SOURCES coroutine/exception-propagation.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME channel-payload-ownership   SOURCES coroutine/channel-payload-ownership.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coroutine-sync-primitives   SOURCES coroutine/coroutine-sync-primitives.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coroutine-combinators       SOURCES coroutine/coroutine-combinators.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME scope-structured-concurrency SOURCES coroutine/scope-structured-concurrency.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME shared-task-fanout          SOURCES coroutine/shared-task-fanout.cpp          DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME patterns-idioms             SOURCES coroutine/patterns-idioms.cpp             DEPENDS ${PROJECT_NAME} LABELS coroutine idiom)
qb_add_test(MODULE qb-io TIER unit NAME channel-sync-ops            SOURCES coroutine/channel-sync-ops.cpp            DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME stream-transforms           SOURCES coroutine/stream-transforms.cpp           DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME generator-sync              SOURCES coroutine/generator-sync.cpp              DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME generator-async             SOURCES coroutine/generator-async.cpp             DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME cancellation-token          SOURCES coroutine/cancellation-token.cpp          DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME retry-policy                SOURCES coroutine/retry-policy.cpp                DEPENDS ${PROJECT_NAME} LABELS coroutine)

qb_debug_message("Configured qb-io unit tests")
