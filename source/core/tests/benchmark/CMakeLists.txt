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
# qb-core BENCHMARKS — google-benchmark perf harnesses, grouped by what they measure:
#   micro/     primitive throughput, no qb::Main (lock-free queue/router, spinlock, json)
#   system/    engine bring-up / push allocator / scheduler / coroutine spawn
#   messaging/ actor-to-actor delivery topologies + ask round-trips
#   topology/  chain / fan-out / diamond latency
# Targets: qb-core-bench-<name>. Built only when QB_BUILD_BENCHMARKS is ON.
# Shared harness headers live in ../shared (BenchmarkCores / BenchmarkActors /
# MpscFanInHarness / LatencyFlush / TestEvent / BenchmarkActorArgs).
# -----------------------------------------------------------------------------

set(_QBC_BENCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# qbc_bench(<subgroup> <name>) → qb-core-bench-<name> from <subgroup>/<name>.cpp
function(qbc_bench SUBGROUP NAME)
    qb_add_benchmark(
        NAME    qb-core-bench-${NAME}
        SOURCES ${SUBGROUP}/${NAME}.cpp
        DEPENDS ${PROJECT_NAME}
        MODULE  qb-core
    )
    if(TARGET qb-core-bench-${NAME})
        set_target_properties(qb-core-bench-${NAME} PROPERTIES
            FOLDER "Benchmarks/Core/${SUBGROUP}")
    endif()
endfunction()

# --- micro/ ---
qbc_bench(micro mpsc-mailbox-fanin)
qbc_bench(micro mpsc-router-dispatch)
qbc_bench(micro spinlock-contention)
qbc_bench(micro jsonb-dump)

# --- system/ ---
qbc_bench(system push-allocated-bigmsg)
qbc_bench(system callback-burst-throughput)
qbc_bench(system engine-lifecycle)
qbc_bench(system actor-spawn-throughput)
qbc_bench(system coroutine-spawn-latency)

# --- messaging/ ---
qbc_bench(messaging fan-in-contention)
qbc_bench(messaging producer-consumer-throughput)
qbc_bench(messaging broadcast-vs-explicit-fanout)
qbc_bench(messaging payload-size-throughput)
qbc_bench(messaging messaging-api-oneway)
qbc_bench(messaging core-distance-pingpong)
qbc_bench(messaging ping-pong-throughput)
qbc_bench(messaging ping-pong-latency)
qbc_bench(messaging forward-vs-direct)
qbc_bench(messaging ask-roundtrip)
qbc_bench(messaging ask-all-fanout)

# --- topology/ ---
qbc_bench(topology pipeline-chain-latency)
qbc_bench(topology multicast-latency)
qbc_bench(topology topology-zoo)

qb_status_message("Configured qb-core benchmarks (tiered: micro/system/messaging/topology)")
