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
# qb Core Library - CMake Configuration
#
# This file configures the build system for the qb Core component, which 
# provides the central actor system implementation and message passing framework.
# -----------------------------------------------------------------------------

# Define project with same version as parent project
project(qb-core
        LANGUAGES CXX
        VERSION ${QB_FRAMEWORK_VERSION}
        DESCRIPTION "Actor system implementation for the qb framework"
)

# -----------------------------------------------------------------------------
# Library Configuration
# -----------------------------------------------------------------------------
# -----------------------------------------------------------------------------
# HAZARD -- ONE SOURCE FILE IS NOT A TYPO. THIS AMALGAMATION IS LOAD-BEARING.
#
# core.cpp #includes ActorId.cpp, Event.cpp, VirtualCore.cpp, Actor.cpp, CoreSet.cpp and
# Main.cpp rather than compiling them separately, so qb-core is ONE translation unit. Listing
# those six here instead is a one-line edit that changes behaviour, and it is the SAME SHAPE as
# qb-io's list -- the measurement, and the three defects a split arms (one of which 3.0 has
# since removed), is in the HAZARD block at src/qb/io/CMakeLists.txt:36. Read it before
# touching this.
#
# One TU is also why a two-TU defect INSIDE this library is invisible here while staying fully
# live against qb's 217 test translation units and every consumer TU: a non-template definition
# added to a header on both paths (Actor.h / VirtualCore.h reach both libqb-core.a and every
# consumer) builds green and fails at the CONSUMER's link. scripts/check-header-linkage.py is
# the guard for that, because a comment is not one.
# -----------------------------------------------------------------------------
set(CORE_SOURCES
        core.cpp
)

# Core depends on the IO library for networking and communication
set(CORE_DEPENDENCIES qb-io)

# Collect definitions
set(CORE_DEFINITIONS)

# -----------------------------------------------------------------------------
# Feature-based definitions
# -----------------------------------------------------------------------------
if (QB_DEBUG_ACTOR)
    list(APPEND CORE_DEFINITIONS QB_DEBUG_ACTOR=1)
    qb_debug_message("Building with actor debugging")
endif ()

if (QB_DEBUG_MEMORY)
    list(APPEND CORE_DEFINITIONS QB_DEBUG_MEMORY=1)
    qb_debug_message("Building with memory debugging")
endif ()

# -----------------------------------------------------------------------------
# Create the Core library
# -----------------------------------------------------------------------------
qb_add_library(
        NAME ${PROJECT_NAME}
        SOURCES ${CORE_SOURCES}
        DEPENDS ${CORE_DEPENDENCIES}
        DEFINES ${CORE_DEFINITIONS}
)

# Set the export name to core (instead of qb-core)
set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME core)

# -----------------------------------------------------------------------------
# Tests configuration
# -----------------------------------------------------------------------------
# QB_BUILD_BENCHMARKS is in the condition because the benchmarks live UNDER tests/core/ (in
# tests/core/benchmark/, added from tests/core/CMakeLists.txt). See the sibling note in
# src/qb/io/CMakeLists.txt: gating on QB_BUILD_TESTS alone made
# `-DQB_BUILD_TESTS=OFF -DQB_BUILD_BENCHMARKS=ON` a silent no-op that still reported
# "- Benchmarks: ON". tests/core/CMakeLists.txt gates unit/ and system/ on QB_BUILD_TESTS.
if (QB_BUILD_TESTS OR QB_BUILD_BENCHMARKS)
    if (QB_BUILD_TESTS)
        qb_status_message("Building qb-core tests")
    endif ()
    # Tests live BESIDE the include root, never inside it: a test header swept into
    # install(DIRECTORY src/ ...) is the failure mode this layout exists to prevent.
    # Added from here rather than from the top level so the suite inherits PROJECT_NAME
    # (= qb-core), which every qb_add_test call in it names as its DEPENDS.
    add_subdirectory("${QB_ROOT_DIR}/tests/core" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif ()