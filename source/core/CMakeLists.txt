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

# Configure library properties with versioning
# SOVERSION sets the ABI compatibility version
set_target_properties(${PROJECT_NAME} PROPERTIES
        VERSION ${QB_VERSION}
        SOVERSION ${QB_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

# Setup include directories for both build and install interfaces
target_include_directories(${PROJECT_NAME}
        PUBLIC
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include>"
        PRIVATE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/source/core/include>"
        INTERFACE
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# Add module directories to the include path
target_include_directories(${PROJECT_NAME}
        PUBLIC "$<BUILD_INTERFACE:${QB_DIRECTORY}/modules>"
)

# Set any additional compile definitions if needed
target_compile_definitions(${PROJECT_NAME}
        PUBLIC
        QB_CORE_VERSION=${QB_VERSION}
)

# -----------------------------------------------------------------------------
# Installation configuration
# -----------------------------------------------------------------------------
if (${QB_PREFIX_UPPER}_INSTALL)
    include(GNUInstallDirs)

    install(TARGETS ${PROJECT_NAME}
            EXPORT ${PROJECT_NAME}Targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Generate and install export file
    install(EXPORT ${PROJECT_NAME}Targets
            FILE ${PROJECT_NAME}Targets.cmake
            NAMESPACE qb::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
    )
endif ()

# -----------------------------------------------------------------------------
# Tests configuration
# -----------------------------------------------------------------------------
# Add tests if enabled in the parent project
if (${QB_PREFIX_UPPER}_BUILD_TEST)
    message(STATUS "[${PROJECT_NAME}] Building tests")
    add_subdirectory(tests)
endif ()