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
# qb-io BENCHMARKS — google-benchmark perf harnesses, grouped by subject.
# Targets: qb-io-bench-<name>. REQUIRES ssl|compression compile-gate on QB_HAS_*.
# -----------------------------------------------------------------------------

# qbio_bench(<subgroup> <name> [REQUIRES...]) → qb-io-bench-<name> from <subgroup>/<name>.cpp
function(qbio_bench SUBGROUP NAME)
    qb_add_benchmark(
        NAME    qb-io-bench-${NAME}
        SOURCES ${SUBGROUP}/${NAME}.cpp
        DEPENDS ${PROJECT_NAME}
        MODULE  qb-io
        REQUIRES ${ARGN})
    if (TARGET qb-io-bench-${NAME})
        set_target_properties(qb-io-bench-${NAME} PROPERTIES FOLDER "Benchmarks/IO/${SUBGROUP}")
    endif ()
endfunction()

qbio_bench(async        async-hotpaths)
qbio_bench(async        ev-backends)
qbio_bench(coroutine    coroutine-scheduler-throughput)
qbio_bench(crypto       crypto-primitives          ssl)
qbio_bench(compression  compress-codecs            compression)
qbio_bench(protocol     framing-scanners)
qbio_bench(serialization json-pipe-serialize)
qbio_bench(uri          uri-parse-encode)
qbio_bench(io           pipe-buffer-throughput)
qbio_bench(io           file-stream)
qbio_bench(async        timer-dispatch)
qbio_bench(coroutine    coroutine-scope)
qbio_bench(coroutine    coroutine-pipeline)
qbio_bench(crypto       crypto-extras              ssl)
qbio_bench(session      session-json               ssl)
qbio_bench(transport    async-bases-framing)

# These two benchmarks reuse the gtest-based shared fixtures (shared/loopback_fixture.h,
# shared/scripted_stream_transport.h), so they need GoogleTest's headers on the include path.
foreach (_b qb-io-bench-session-json qb-io-bench-async-bases-framing)
    if (TARGET ${_b})
        if (TARGET GTest::gtest)
            target_link_libraries(${_b} PRIVATE GTest::gtest)
        elseif (TARGET gtest)
            target_link_libraries(${_b} PRIVATE gtest)
        endif ()
    endif ()
endforeach ()

qb_status_message("Configured qb-io benchmarks (tiered: async/coroutine/crypto/compression/protocol/serialization/session/transport/uri/io)")
