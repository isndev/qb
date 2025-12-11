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
if(QB_DEBUG_ACTOR)
    list(APPEND CORE_DEFINITIONS QB_DEBUG_ACTOR=1)
    qb_debug_message("Building with actor debugging")
endif()

if(QB_DEBUG_MEMORY)
    list(APPEND CORE_DEFINITIONS QB_DEBUG_MEMORY=1)
    qb_debug_message("Building with memory debugging")
endif()

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
if(QB_BUILD_TESTS)
    qb_status_message("Building qb-core tests")
    add_subdirectory(tests)
endif()