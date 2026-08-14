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
# being decomposed per dev/tests-audit/_spec-qb-core.md §2; they are registered as-is
# (tier:system) so the suite stays green until the split wave replaces them.
# -----------------------------------------------------------------------------

# --- engine / runtime -------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME main-lifecycle   SOURCES engine/main-lifecycle.cpp   DEPENDS ${PROJECT_NAME} LABELS signal serial requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME sigterm-shutdown SOURCES engine/sigterm-shutdown.cpp DEPENDS ${PROJECT_NAME} LABELS signal serial)
# `serial` + `signal`: drives Main::stop(), which is process-wide state. Its second case ASSERTS a
# wedge (a KillEvent-only no_default_events actor cannot be stopped), so it detaches one engine
# thread on purpose -- see the file header. Bounded, and it is what the corrected docs rest on.
qb_add_test(MODULE qb-core TIER system NAME no-default-events SOURCES engine/no-default-events.cpp DEPENDS ${PROJECT_NAME} LABELS signal serial)
# engine-io-smoke drives the LOGGING BACKEND itself -- it points the global logger at a throwaway
# temp path with qb::io::log::init() and reads the rolled files back. That namespace, and nanolog
# behind it, only exist when QB_WITH_LOGGING is ON, and the test uses them unguarded, so with
# logging off it does not compile: 14 x "'log' is not a class, namespace, or enumeration" plus
# 3 x "use of undeclared identifier 'nanolog'". Nothing caught that, because no preset and no CI
# lane builds with QB_WITH_LOGGING=OFF -- the libraries and all three qbm modules DO build in that
# configuration; only this test does not. Registering it conditionally is the honest fix: with the
# backend compiled out there is nothing here to smoke-test.
if (QB_WITH_LOGGING)
    qb_add_test(MODULE qb-core TIER system NAME engine-io-smoke  SOURCES runtime/engine-io-smoke.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)
endif ()

# --- actor ------------------------------------------------------------------
# actor-add and actor-dependency each bring up a SECOND VirtualCore -- actor-add's
# BroadcastKillLeavesNoSurvivors puts its sender on core 0 and 1024 targets on core 1
# (actor/actor-add.cpp:298-301), and three of actor-dependency's cases run cores 0+1 to
# completion (actor/actor-dependency.cpp:141-143, :155-157, :169-171). Neither is guarded by
# hardware_concurrency(), so both belong under the shared qb-multicore lock the label derives.
qb_add_test(MODULE qb-core TIER system NAME actor-add        SOURCES actor/actor-add.cpp         DEPENDS ${PROJECT_NAME} LABELS requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME actor-dependency SOURCES actor/actor-dependency.cpp  DEPENDS ${PROJECT_NAME} LABELS requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME actor-callback   SOURCES actor/actor-callback.cpp    DEPENDS ${PROJECT_NAME})

# --- timer ------------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME async-callback-ordering SOURCES timer/async-callback-ordering.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine)

# --- messaging --------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME messaging-api           SOURCES messaging/messaging-api.cpp           DEPENDS ${PROJECT_NAME} LABELS requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME messaging-reply-forward SOURCES messaging/messaging-reply-forward.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME messaging-oversize-event SOURCES messaging/oversize-event-probe.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME messaging-relocatable-payload SOURCES messaging/relocatable-payload.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME messaging-send-nontrivial SOURCES messaging/send-nontrivial-payload.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)

# --- event ------------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME service-event-ring SOURCES event/service-event-ring.cpp DEPENDS ${PROJECT_NAME} LABELS requires-multicore)

# --- coroutine --------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME coroutine-basics   SOURCES coroutine/coroutine-basics.cpp   DEPENDS ${PROJECT_NAME} LABELS coroutine)
qb_add_test(MODULE qb-core TIER system NAME coroutine-detached SOURCES coroutine/coroutine-detached.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME coroutine-scope    SOURCES coroutine/coroutine-scope.cpp    DEPENDS ${PROJECT_NAME} LABELS coroutine)
# `serial`: it swaps std::cerr's streambuf to read the diagnostic back, which is process-global.
qb_add_test(MODULE qb-core TIER system NAME coroutine-escaped-exception SOURCES coroutine/coroutine-escaped-exception.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine serial)
qb_add_test(MODULE qb-core TIER system NAME ask-roundtrip      SOURCES coroutine/ask-roundtrip.cpp      DEPENDS ${PROJECT_NAME} LABELS coroutine requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME ask-patterns       SOURCES coroutine/ask-patterns.cpp       DEPENDS ${PROJECT_NAME} LABELS coroutine requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME coroutine-resilience SOURCES coroutine/coroutine-resilience.cpp DEPENDS ${PROJECT_NAME} LABELS coroutine patterns)

# --- lifecycle --------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME actor-lifecycle-ordering SOURCES lifecycle/actor-lifecycle-ordering.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME kill-during-reap SOURCES lifecycle/kill-during-reap.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME actor-state-roundtrip    SOURCES lifecycle/actor-state-roundtrip.cpp    DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME actor-resource-cleanup   SOURCES lifecycle/actor-resource-cleanup.cpp   DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME actor-invalid-reference  SOURCES lifecycle/actor-invalid-reference.cpp  DEPENDS ${PROJECT_NAME})
# main-shutdown is the most exposed of the three that were missing `requires-multicore`: both of
# its cases build cores 0+1, start the engine ASYNC (`main.start(true)`) and then assert a
# WALL-CLOCK bound -- `elapsed.count() < 10` at lifecycle/main-shutdown.cpp:63 and :80. A timing
# assertion measured on an oversubscribed engine is a timing assertion measuring the scheduler,
# not the shutdown path it names.
qb_add_test(MODULE qb-core TIER system NAME main-shutdown            SOURCES lifecycle/main-shutdown.cpp            DEPENDS ${PROJECT_NAME} LABELS slow requires-multicore)

# --- core hardening (range checks, customization points) --------------------
qb_add_test(MODULE qb-core TIER system NAME main-core-range            SOURCES core/main-core-range.cpp            DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME actor-customization-points SOURCES core/actor-customization-points.cpp DEPENDS ${PROJECT_NAME})

# --- concurrency ------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME actor-message-serialization SOURCES concurrency/actor-message-serialization.cpp DEPENDS ${PROJECT_NAME})
qb_add_test(MODULE qb-core TIER system NAME qos1-backpressure           SOURCES concurrency/qos1-backpressure.cpp           DEPENDS ${PROJECT_NAME} LABELS slow requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME shutdown-saturation         SOURCES concurrency/shutdown-saturation.cpp         DEPENDS ${PROJECT_NAME} LABELS slow requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME shutdown-multibucket-residue SOURCES concurrency/shutdown-multibucket-residue.cpp DEPENDS ${PROJECT_NAME} LABELS slow requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME unhandled-event-disposal SOURCES concurrency/unhandled-event-disposal.cpp DEPENDS ${PROJECT_NAME})

# --- regression -------------------------------------------------------------
qb_add_test(MODULE qb-core TIER system NAME router-callback-regressions SOURCES regression/router-callback-regressions.cpp DEPENDS ${PROJECT_NAME})

# --- patterns (resilience / aggregate / saga / routing / pubsub / discovery) -
qb_add_test(MODULE qb-core TIER system NAME supervisor-strategies   SOURCES patterns/supervisor-strategies.cpp   DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER system NAME resilience-rate-limiter SOURCES patterns/resilience-rate-limiter.cpp DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME resilience-bulkhead     SOURCES patterns/resilience-bulkhead.cpp     DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME aggregate-scatter       SOURCES patterns/aggregate-scatter.cpp       DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME aggregate-quorum        SOURCES patterns/aggregate-quorum.cpp        DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME aggregate-batcher       SOURCES patterns/aggregate-batcher.cpp       DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME request-deadline        SOURCES patterns/request-deadline.cpp        DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME streaming-ask-stream    SOURCES patterns/streaming-ask-stream.cpp    DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME idempotency-answer      SOURCES patterns/idempotency-answer.cpp      DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME saga-cancel             SOURCES patterns/saga-cancel.cpp             DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME discovery-ping-require  SOURCES patterns/discovery-ping-require.cpp  DEPENDS ${PROJECT_NAME} LABELS patterns coroutine)
qb_add_test(MODULE qb-core TIER system NAME routing-dispatch        SOURCES patterns/routing-dispatch.cpp        DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER system NAME pubsub-fanout           SOURCES patterns/pubsub-fanout.cpp           DEPENDS ${PROJECT_NAME} LABELS patterns)
qb_add_test(MODULE qb-core TIER system NAME pubsub-dead-subscribers SOURCES patterns/pubsub-dead-subscribers.cpp DEPENDS ${PROJECT_NAME} LABELS patterns)

# --- init (async onInit: state machine / stash / deadline / activation) ------
qb_add_test(MODULE qb-core TIER system NAME init-lifecycle     SOURCES init/init-lifecycle.cpp     DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME init-ask-gate      SOURCES init/init-ask-gate.cpp      DEPENDS ${PROJECT_NAME} LABELS init coroutine requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME init-service-actor SOURCES init/init-service-actor.cpp DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME init-stash-lifetime SOURCES init/init-stash-lifetime.cpp DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME init-patterns      SOURCES init/init-patterns.cpp      DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME init-multicore     SOURCES init/init-multicore.cpp     DEPENDS ${PROJECT_NAME} LABELS init coroutine requires-multicore)
qb_add_test(MODULE qb-core TIER system NAME init-deadline      SOURCES init/init-deadline.cpp      DEPENDS ${PROJECT_NAME} LABELS init coroutine slow)
qb_add_test(MODULE qb-core TIER system NAME init-stress        SOURCES init/init-stress.cpp        DEPENDS ${PROJECT_NAME} LABELS init coroutine slow)
qb_add_test(MODULE qb-core TIER system NAME init-handle        SOURCES init/init-handle.cpp        DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME init-gate-edges    SOURCES init/init-gate-edges.cpp    DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME init-gate-disposal SOURCES init/init-gate-disposal.cpp DEPENDS ${PROJECT_NAME} LABELS init coroutine)
qb_add_test(MODULE qb-core TIER system NAME init-clock         SOURCES init/init-clock.cpp         DEPENDS ${PROJECT_NAME} LABELS init coroutine)

qb_debug_message("Configured ${CMAKE_CURRENT_SOURCE_DIR} with the tiered test helper")
