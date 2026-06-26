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
# TRANSITIONAL — split-pending coroutine monoliths.
# These large files are being dissolved/split into unit/coroutine/* and system/* per
# docs/tests-audit/_spec-qb-io.md §2/§3 (basic+comprehensive+regression dissolve; channel/
# stream/cancellation/retry/edge-cases/safety/tcp split across tiers). Registered as-is (temp
# tier) so the suite stays green until the split wave replaces them; this directory is deleted
# once empty.
# -----------------------------------------------------------------------------

# pure coroutine-logic monoliths (true tier=unit)
qb_add_test(MODULE qb-io TIER unit NAME coro-basic           SOURCES test-coroutine-basic.cpp            DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-comprehensive   SOURCES test-coroutine-comprehensive.cpp    DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-generator       SOURCES test-coroutine-generator.cpp        DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-channel         SOURCES test-coroutine-channel.cpp          DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-stream          SOURCES test-coroutine-stream.cpp           DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-stream-advanced SOURCES test-coroutine-stream-advanced.cpp  DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-cancellation    SOURCES test-coroutine-cancellation.cpp     DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-retry           SOURCES test-coroutine-retry.cpp            DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-io TIER unit NAME coro-regression      SOURCES test-coroutine-regression.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine)

# scheduler-stress / memory-pressure monoliths (true tier=system)
qb_add_test(MODULE qb-io TIER system NAME coro-edge-cases    SOURCES test-coroutine-edge-cases.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine slow)
qb_add_test(MODULE qb-io TIER system NAME coro-safety        SOURCES test-coroutine-safety.cpp           DEPENDS ${PROJECT_NAME} LABELS coroutine)

# TCP loopback coroutine monoliths (true tier=system, network)
qb_add_test(MODULE qb-io TIER system NAME coro-tcp-client-integration SOURCES test-coroutine-tcp-client-integration.cpp DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME coro-tcp-client-io          SOURCES test-coroutine-tcp-client-io.cpp          DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
qb_add_test(MODULE qb-io TIER system NAME coro-tcp-connector          SOURCES test-coroutine-tcp-connector.cpp          DEPENDS ${PROJECT_NAME} REQUIRES network LABELS coroutine)
