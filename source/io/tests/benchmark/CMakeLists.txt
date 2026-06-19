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
# qb IO Benchmarks - CMake Configuration
#
# This file configures the performance benchmarks for the qb IO component.
# It follows the same conventions as qb-core benchmarks: bm-*.cpp files become
# ${PROJECT_NAME}-benchmark-* targets through qb_add_benchmark.
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Benchmark source files by category
# -----------------------------------------------------------------------------

# Async listener, callback, and broadcast scratch hot paths
set(ASYNC_BENCHMARKS
        bm-io-plan.cpp
        bm-ev-backends.cpp
)

# Coroutine scheduler, frame, await, and timer workloads
set(COROUTINE_BENCHMARKS
        bm-coroutine.cpp
)

# Buffer primitives used by transports and protocols
set(BUFFER_BENCHMARKS
        bm-pipe-buffer.cpp
)

# File helpers and file-backed streams
set(FILE_BENCHMARKS
        bm-file-stream.cpp
)

# URI parsing and encoding utilities
set(URI_BENCHMARKS
        bm-uri.cpp
)

# Protocol framing and JSON pipe integration
set(PROTOCOL_BENCHMARKS
        bm-protocol-framing.cpp
        bm-json-pipe.cpp
)

# Optional compression providers and pipe adapters
if(QB_HAS_COMPRESSION)
    set(COMPRESSION_BENCHMARKS
            bm-compression.cpp
    )
endif()

# Optional OpenSSL-backed crypto helpers
if(QB_HAS_SSL)
    set(CRYPTO_BENCHMARKS
            bm-crypto.cpp
    )
endif()

# -----------------------------------------------------------------------------
# Configure benchmark targets
# -----------------------------------------------------------------------------

function(create_benchmark BENCHMARK_SOURCE CATEGORY)
    get_filename_component(BENCHMARK_NAME ${BENCHMARK_SOURCE} NAME_WE)
    string(REPLACE "bm-" "" BENCHMARK_TARGET_NAME ${BENCHMARK_NAME})

    qb_add_benchmark(
            NAME ${PROJECT_NAME}-benchmark-${BENCHMARK_TARGET_NAME}
            SOURCES ${BENCHMARK_SOURCE}
            DEPENDS ${PROJECT_NAME}
    )

    set_target_properties(${PROJECT_NAME}-benchmark-${BENCHMARK_TARGET_NAME} PROPERTIES
            FOLDER "Benchmarks/IO/${CATEGORY}"
    )

    qb_debug_message("Added ${CATEGORY} benchmark: ${BENCHMARK_TARGET_NAME}")
endfunction()

foreach (BENCHMARK_SOURCE ${ASYNC_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Async")
endforeach ()

foreach (BENCHMARK_SOURCE ${COROUTINE_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Coroutine")
endforeach ()

foreach (BENCHMARK_SOURCE ${BUFFER_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Buffer")
endforeach ()

foreach (BENCHMARK_SOURCE ${FILE_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "File")
endforeach ()

foreach (BENCHMARK_SOURCE ${URI_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "URI")
endforeach ()

foreach (BENCHMARK_SOURCE ${PROTOCOL_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Protocol")
endforeach ()

foreach (BENCHMARK_SOURCE ${COMPRESSION_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Compression")
endforeach ()

foreach (BENCHMARK_SOURCE ${CRYPTO_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Crypto")
endforeach ()

qb_status_message("Configured benchmarks in ${CMAKE_CURRENT_SOURCE_DIR}")
