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
# qb IO Library - CMake Configuration
#
# This file configures the build system for the qb IO component, which provides
# network I/O, socket handling, and core communication functionality.
# -----------------------------------------------------------------------------

# Define project with same version as parent project
project(qb-io
        LANGUAGES CXX
        VERSION ${QB_FRAMEWORK_VERSION}
        DESCRIPTION "I/O and networking library for the qb framework"
)

# -----------------------------------------------------------------------------
# Library Configuration
# -----------------------------------------------------------------------------
# Define the library using the new qb framework functions
set(IO_SOURCES
    src/io.cpp
)

# Collect dependencies
set(IO_DEPENDENCIES ev stduuid)

# Collect definitions
set(IO_DEFINITIONS)

# -----------------------------------------------------------------------------
# Logging configuration
# -----------------------------------------------------------------------------
if(QB_WITH_LOGGING)
    list(APPEND IO_DEFINITIONS QB_WITH_LOGGING=1)
    qb_debug_message("Building with logger support")
else()
    qb_debug_message("Building without logger support")
    if(QB_STDOUT_LOGGING)
        list(APPEND IO_DEFINITIONS QB_STDOUT_LOGGING=1)
        qb_debug_message("Enabling stdout logging")
    endif()
endif()

# -----------------------------------------------------------------------------
# Platform-specific dependencies
# -----------------------------------------------------------------------------
if(QB_PLATFORM_WINDOWS)
    list(APPEND IO_DEPENDENCIES ws2_32)
    qb_debug_message("Adding Windows-specific dependency: ws2_32")
endif()

# -----------------------------------------------------------------------------
# Optional features based on detected dependencies
# -----------------------------------------------------------------------------
if(QB_HAS_COMPRESSION)
    list(APPEND IO_DEPENDENCIES ZLIB::ZLIB)
    list(APPEND IO_DEFINITIONS QB_HAS_COMPRESSION=1)
    qb_debug_message("Building with ZLIB compression support")
endif()

if(QB_HAS_SSL)
    list(APPEND IO_DEPENDENCIES OpenSSL::SSL OpenSSL::Crypto)
    list(APPEND IO_DEFINITIONS QB_HAS_SSL=1)
    qb_debug_message("Building with OpenSSL SSL/TLS support")
    
    if(QB_HAS_ARGON2)
        list(APPEND IO_DEPENDENCIES ${ARGON2_LIBRARIES})
        list(APPEND IO_DEFINITIONS QB_HAS_ARGON2=1)
        qb_debug_message("Building with Argon2 advanced crypto support")
    endif()
endif()

# -----------------------------------------------------------------------------
# Create the IO library
# -----------------------------------------------------------------------------
qb_add_library(
    NAME ${PROJECT_NAME}
    SOURCES ${IO_SOURCES}
    DEPENDS ${IO_DEPENDENCIES}
    DEFINES ${IO_DEFINITIONS}
)

# Set the export name to io (instead of qb-io)
set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME io)

# -----------------------------------------------------------------------------
# Tests configuration
# -----------------------------------------------------------------------------
if(QB_BUILD_TESTS)
    qb_status_message("Building qb-io tests")
    add_subdirectory(tests)
endif()
