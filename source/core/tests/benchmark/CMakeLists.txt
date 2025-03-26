#
# qb - C++ Actor Framework
# Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
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
# qb Core Benchmarks - CMake Configuration
#
# This file configures the performance benchmarks for the qb Core component.
# It defines benchmark targets for various actor system patterns and performance
# characteristics using Google Benchmark framework.
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Benchmark source files by category
# -----------------------------------------------------------------------------

# Messaging pattern benchmarks
set(MESSAGING_BENCHMARKS
    bm-ping-pong.cpp
    bm-ping-pong-latency.cpp
)

# Throughput pattern benchmarks
set(THROUGHPUT_BENCHMARKS
    bm-disruptor-latency.cpp
    bm-multicast-latency.cpp
    bm-pipeline-latency.cpp
    bm-producer-consumer.cpp
)

# Example benchmark is commented out
# set(EXAMPLE_BENCHMARKS
#     benchmark_example.cpp
# )

# -----------------------------------------------------------------------------
# Configure benchmark targets
# -----------------------------------------------------------------------------

# Helper function to create benchmark targets with consistent naming and properties
function(create_benchmark BENCHMARK_SOURCE CATEGORY)
    # Extract benchmark name from filename (removing extension)
    get_filename_component(BENCHMARK_NAME ${BENCHMARK_SOURCE} NAME_WE)
    string(REPLACE "bm-" "" BENCHMARK_TARGET_NAME ${BENCHMARK_NAME})
    
    # Create benchmark target
    cxx_benchmark(
        ${PROJECT_NAME}-benchmark-${BENCHMARK_TARGET_NAME}  # Target name
        "${PROJECT_NAME}"                                    # Library dependency
        ${BENCHMARK_SOURCE}                                  # Source file
    )
    
    # Set properties for organization in IDEs
    set_target_properties(${PROJECT_NAME}-benchmark-${BENCHMARK_TARGET_NAME} PROPERTIES
        FOLDER "Benchmarks/Core/${CATEGORY}"
    )
    
    # Report benchmark setup
    message(STATUS "[Core Benchmarks] Added ${CATEGORY} benchmark: ${BENCHMARK_TARGET_NAME}")
endfunction()

# Create messaging benchmark targets
foreach(BENCHMARK_SOURCE ${MESSAGING_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Messaging")
endforeach()

# Create throughput benchmark targets
foreach(BENCHMARK_SOURCE ${THROUGHPUT_BENCHMARKS})
    create_benchmark(${BENCHMARK_SOURCE} "Throughput")
endforeach()

# Create example benchmark targets (currently disabled)
# foreach(BENCHMARK_SOURCE ${EXAMPLE_BENCHMARKS})
#     create_benchmark(${BENCHMARK_SOURCE} "Examples")
# endforeach()

message(STATUS "[Core Benchmarks] Configured benchmarks in ${CMAKE_CURRENT_SOURCE_DIR}")