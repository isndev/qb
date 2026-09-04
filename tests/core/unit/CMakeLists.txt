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
# qb-core UNIT tests — pure logic, no qb::Main engine, daemon-free, parallel-safe.
# Registered through the shared tiered helper (qb_add_test MODULE/TIER/...), which
# applies the tier:unit + module:qb-core labels, the unit TIMEOUT, and the IDE folder.
# Run just this lane: ctest -L 'tier:unit' --parallel "$(nproc)"
# -----------------------------------------------------------------------------

qb_add_test(MODULE qb-core TIER unit NAME string         SOURCES container/string.cpp        DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME unordered-map-contract SOURCES container/unordered-map-contract.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME jsonb-wrapper  SOURCES json/jsonb-wrapper.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME actor-id                SOURCES core/actor-id.cpp                 DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME coreset-edges           SOURCES core/coreset-edges.cpp            DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME type-id-identity        SOURCES core/type-id-identity.cpp         DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME actor-name-lifetime     SOURCES core/actor-name-lifetime.cpp      DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME service-index-uniqueness SOURCES core/service-index-uniqueness.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME event-header   SOURCES core/event-header.cpp        DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME event-field-shadow      SOURCES core/event-field-shadow.cpp       DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME mailbox-park-handshake  SOURCES core/mailbox-park-handshake.cpp   DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME event-traits   SOURCES type/event-traits.cpp       DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME spinlock-basic SOURCES lockfree/spinlock-basic.cpp DEPENDS ${PROJECT_NAME} LABELS lockfree)
qb_add_test(MODULE qb-core TIER unit NAME ring-wrap-batching SOURCES lockfree/ring-wrap-batching.cpp DEPENDS ${PROJECT_NAME} LABELS lockfree)
qb_add_test(MODULE qb-core TIER unit NAME spsc-cached-index SOURCES lockfree/spsc-cached-index.cpp DEPENDS ${PROJECT_NAME} LABELS lockfree)
qb_add_test(MODULE qb-core TIER unit NAME mpsc-unbounded-queue SOURCES lockfree/mpsc-unbounded-queue.cpp DEPENDS ${PROJECT_NAME} LABELS lockfree)
qb_add_test(MODULE qb-core TIER unit NAME mpsc-dequeue-parity SOURCES lockfree/mpsc-dequeue-parity.cpp DEPENDS ${PROJECT_NAME} LABELS lockfree)
qb_add_test(MODULE qb-core TIER unit NAME time           SOURCES system/time.cpp             DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME time-edge      SOURCES system/time-edge.cpp        DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME parse          SOURCES system/parse.cpp            DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER unit NAME event-router   SOURCES system/event-router.cpp     DEPENDS ${PROJECT_NAME})

# --- patterns (pure-logic helpers: resilience math, dedup map, worker-pool) --
qb_add_test(MODULE qb-core TIER unit NAME circuit-breaker        SOURCES patterns/circuit-breaker.cpp        DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER unit NAME resilience-jitter      SOURCES patterns/resilience-jitter.cpp      DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER unit NAME resilience-rate-limiter SOURCES patterns/resilience-rate-limiter.cpp DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER unit NAME resilience-bulkhead    SOURCES patterns/resilience-bulkhead.cpp    DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER unit NAME idempotency-dedup-map  SOURCES patterns/idempotency-dedup-map.cpp  DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER unit NAME routing-worker-pool    SOURCES patterns/routing-worker-pool.cpp    DEPENDS ${PROJECT_NAME} LABELS patterns)
