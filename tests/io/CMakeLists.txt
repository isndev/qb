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
# qb IO Tests - CMake Configuration
#
# This file configures the test build system for the qb IO component.
# It includes the system tests subdirectory which contains implementation
# tests for the IO library features.
# -----------------------------------------------------------------------------

# Tiered test tree (docs/tests-audit/_spec-qb-io.md): unit/ (pure logic, daemon-free) +
# system/ (event loop on kernel objects / loopback). The legacy flat coroutine/ dir is kept
# transitionally for the monoliths still being decomposed by the split wave.
# This directory is now also entered when only QB_BUILD_BENCHMARKS is on (the benchmarks live
# below it), so the test trees carry their own gate rather than relying on the caller's.
if (QB_BUILD_TESTS)
    add_subdirectory(unit)
    add_subdirectory(system)
endif ()

# Add performance benchmarks if benchmarking is enabled
if (QB_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif ()
