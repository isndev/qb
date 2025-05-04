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
project(${QB_PREFIX}-core
        LANGUAGES CXX
        VERSION ${QB_VERSION}
        DESCRIPTION "Actor system implementation for the qb framework"
)

# -----------------------------------------------------------------------------
# Library source files
# -----------------------------------------------------------------------------
set(CORE_SOURCES
        core.cpp
)

# -----------------------------------------------------------------------------
# Library target configuration
# -----------------------------------------------------------------------------
# Define the Core library using the custom cxx_library macro from parent project
# Core depends on the IO library for networking and communication
cxx_library(
        NAME ${PROJECT_NAME}
        FLAGS ${cxx_default_lib}
        SOURCES ${CORE_SOURCES}
        DEPENDENCIES ${QB_PREFIX}-io
)

# Set the export name to core (instead of qb-core)
set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME core)

# Configure the module using our shared configuration function
qb_configure_module(
        NAME ${PROJECT_NAME}
        COMPONENT CORE
)

# Add private include directories specific to this module
target_include_directories(${PROJECT_NAME}
        PRIVATE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/source/core/include>"
)

# -----------------------------------------------------------------------------
# Tests configuration
# -----------------------------------------------------------------------------
# Add tests if enabled in the parent project
if (${QB_PREFIX_UPPER}_BUILD_TEST)
    message(STATUS "[${PROJECT_NAME}] Building tests")
    add_subdirectory(tests)
endif ()