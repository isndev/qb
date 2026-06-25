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
# qb-core SYSTEM tests — in-process qb::Main engine + event loop, NO external daemon.
# Registered through the shared tiered helper (tier:system + module:qb-core labels,
# system TIMEOUT, IDE folder). Feature tags per file: coroutine / init / patterns /
# slow / signal / serial / requires-multicore.
#
# NOTE: the files still listed flat at the bottom (SPLIT_PENDING) are large monoliths
# being decomposed per docs/tests-audit/_spec-qb-core.md §2; they are registered as-is
# (tier:system) so the suite stays green until the split wave replaces them.
# -----------------------------------------------------------------------------

# --- engine / runtime -------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME main-lifecycle   SOURCES engine/main-lifecycle.cpp   DEPENDS ${PROJECT_NAME} LABELS signal serial requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME engine-io-smoke  SOURCES runtime/engine-io-smoke.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)

# --- actor ------------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME actor-add        SOURCES actor/actor-add.cpp         DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME actor-dependency SOURCES actor/actor-dependency.cpp  DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME actor-callback   SOURCES actor/actor-callback.cpp    DEPENDS ${PROJECT_NAME})

# --- timer ------------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME async-callback-ordering SOURCES timer/async-callback-ordering.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)

# --- messaging --------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME messaging-api           SOURCES messaging/messaging-api.cpp           DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME messaging-reply-forward SOURCES messaging/messaging-reply-forward.cpp DEPENDS ${PROJECT_NAME})

# --- event ------------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME service-event-ring SOURCES event/service-event-ring.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)

# --- coroutine --------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME coroutine-basics   SOURCES coroutine/coroutine-basics.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-core TIER system NAME coroutine-detached SOURCES coroutine/coroutine-detached.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-core TIER system NAME coroutine-scope    SOURCES coroutine/coroutine-scope.cpp    DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-core TIER system NAME ask-roundtrip      SOURCES coroutine/ask-roundtrip.cpp      DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-core TIER system NAME ask-patterns       SOURCES coroutine/ask-patterns.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)

# --- lifecycle --------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME actor-lifecycle-ordering SOURCES lifecycle/actor-lifecycle-ordering.cpp DEPENDS ${PROJECT_NAME} LABELS slow)
qb_add_test(MODULE qb-core TIER system NAME actor-state-roundtrip    SOURCES lifecycle/actor-state-roundtrip.cpp    DEPENDS ${PROJECT_NAME} LABELS slow)
qb_add_test(MODULE qb-core TIER system NAME actor-resource-cleanup   SOURCES lifecycle/actor-resource-cleanup.cpp   DEPENDS ${PROJECT_NAME} LABELS slow)
qb_add_test(MODULE qb-core TIER system NAME actor-invalid-reference  SOURCES lifecycle/actor-invalid-reference.cpp  DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME main-shutdown            SOURCES lifecycle/main-shutdown.cpp            DEPENDS ${PROJECT_NAME} LABELS slow)

# --- core hardening (range checks, customization points) --------------------
qb_add_test(MODULE qb-core TIER system NAME main-core-range            SOURCES core/main-core-range.cpp            DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME actor-customization-points SOURCES core/actor-customization-points.cpp DEPENDS ${PROJECT_NAME})

# --- concurrency ------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME actor-message-serialization SOURCES concurrency/actor-message-serialization.cpp DEPENDS ${PROJECT_NAME} LABELS slow)
qb_add_test(MODULE qb-core TIER system NAME qos1-backpressure           SOURCES concurrency/qos1-backpressure.cpp           DEPENDS ${PROJECT_NAME} LABELS slow requires-multicore)

# --- regression -------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME router-callback-regressions SOURCES regression/router-callback-regressions.cpp DEPENDS ${PROJECT_NAME})

# --- patterns ---------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME supervisor-strategies SOURCES patterns/supervisor-strategies.cpp DEPENDS ${PROJECT_NAME} LABELS patterns slow)

# --- SPLIT_PENDING (monoliths, registered flat until decomposed per spec §2) -
qb_add_test(MODULE qb-core TIER system NAME patterns-enrich     SOURCES test-actor-patterns-enrich.cpp     DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME coroutine-resilience SOURCES test-actor-coroutine-resilience.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine patterns)
qb_add_test(MODULE qb-core TIER system NAME async-init          SOURCES test-actor-async-init.cpp          DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME async-init-torture  SOURCES test-actor-async-init-torture.cpp  DEPENDS ${PROJECT_NAME} LABELS init coroutine slow requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME init-robustness     SOURCES test-actor-init-robustness.cpp     DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME routing-pubsub      SOURCES test-actor-routing-pubsub.cpp      DEPENDS ${PROJECT_NAME} LABELS patterns)

qb_debug_message("Configured ${CMAKE_CURRENT_SOURCE_DIR} with the tiered test helper")
