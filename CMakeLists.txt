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
# qb Framework - Main CMake Configuration
#
# This is the main entry point for the qb high-performance C++ Actor Framework.
# It provides a modern, clean, and powerful build system.
#
# Usage:
# 1. add_subdirectory(path/to/qb) - for embedding qb in your project
# 2. find_package(qb) - for using installed qb
#
# Both methods provide the same targets: qb::core, qb::io
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.24)

# -----------------------------------------------------------------------------
# Early Configuration
# -----------------------------------------------------------------------------
# Load configuration before project() to ensure all settings are available
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/qbConfig.cmake")

# Project definition using configuration
project(${QB_FRAMEWORK_NAME}
        LANGUAGES CXX
        VERSION ${QB_FRAMEWORK_VERSION}
        DESCRIPTION "${QB_FRAMEWORK_DESCRIPTION}"
)

qb_initialize_project_configuration()

if(QB_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif()

# -----------------------------------------------------------------------------
# Core Framework Initialization
# -----------------------------------------------------------------------------
# Load all framework modules in correct order
include("${QB_CMAKE_DIR}/qbCompiler.cmake")
include("${QB_CMAKE_DIR}/qbDependencies.cmake")
include("${QB_CMAKE_DIR}/qbFunctions.cmake")

# GoogleTest / Google Benchmark (FetchContent or system); must run before test targets.
include("${QB_CMAKE_DIR}/qbFetchGoogleDeps.cmake")

# Print configuration summary
qb_print_configuration()

# -----------------------------------------------------------------------------
# Internal Modules Setup
# -----------------------------------------------------------------------------
qb_status_message("Loading internal modules...")

# Configure UUID module
set(UUID_BUILD_TESTS OFF)
set(UUID_SYSTEM_GENERATOR OFF)
set(UUID_TIME_GENERATOR OFF)
set(UUID_USING_CXX20_SPAN OFF)

# Load bundled modules (ev, uuid, etc.)
add_subdirectory("${QB_MODULES_DIR}/uuid")
add_subdirectory("${QB_MODULES_DIR}/ev")

# -----------------------------------------------------------------------------
# Core Libraries
# -----------------------------------------------------------------------------
qb_status_message("Building core libraries...")

# Build qb-io library
add_subdirectory("${QB_SOURCE_DIR}/io")

# Build qb-core library (depends on qb-io)
add_subdirectory("${QB_SOURCE_DIR}/core")

# -----------------------------------------------------------------------------
# Framework Targets and Aliases
# -----------------------------------------------------------------------------
# Create convenience aliases for the main libraries
if(TARGET qb-io)
    add_library(qb::io ALIAS qb-io)
    list(APPEND QB_CORE_LIBRARIES qb-io)
    set(QB_IO_FOUND TRUE)
endif()

if(TARGET qb-core)
    add_library(qb::core ALIAS qb-core)
    list(APPEND QB_CORE_LIBRARIES qb-core)
    set(QB_CORE_FOUND TRUE)
endif()

if(TARGET ev)
    set(QB_EV_FOUND TRUE)
endif()

# Update global library lists
set(QB_ALL_LIBRARIES ${QB_CORE_LIBRARIES} ${QB_MODULE_LIBRARIES})

# -----------------------------------------------------------------------------
# Test resources (SSL certificates, etc.)
# -----------------------------------------------------------------------------
if(QB_BUILD_TESTS)
    qb_setup_test_resources()
endif()

# -----------------------------------------------------------------------------
# Code Coverage Setup
# -----------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32 AND QB_BUILD_COVERAGE)
    include("${QB_CMAKE_DIR}/CodeCoverage.cmake")

    find_program(LCOV_PATH lcov)
    if(NOT GCOV_PATH)
        find_program(GCOV_PATH gcov)
    endif()
    
    if(LCOV_PATH AND GCOV_PATH)
        qb_status_message("Code coverage enabled")
        
        set(COVERAGE_LCOV_EXCLUDES
            '/usr/*'
            '*benchmark*'
            '*modules*'
            '*examples/*'
            '*test*'
        )
        
        SETUP_TARGET_FOR_COVERAGE_LCOV(
            NAME qb-coverage
            EXECUTABLE ctest
            DEPENDENCIES qb-core
            LCOV_ARGS --ignore-errors mismatch --ignore-errors unused
        )
        
        SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(
            NAME qb-coverage-xml
            EXECUTABLE ctest
            DEPENDENCIES qb-core
        )
        
        SETUP_TARGET_FOR_COVERAGE_GCOVR_HTML(
            NAME qb-coverage-html
            EXECUTABLE ctest
            DEPENDENCIES qb-core
        )
    else()
        qb_warning_message("Coverage requested but tools not found (lcov, gcov)")
    endif()
endif()

# -----------------------------------------------------------------------------
# Documentation
# -----------------------------------------------------------------------------
if(QB_BUILD_DOCS)
    add_subdirectory("${QB_ROOT_DIR}/docs")
endif()

# -----------------------------------------------------------------------------
# Installation Configuration
# -----------------------------------------------------------------------------
if(QB_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    
    # Collect the library targets that should be installed.
    # Bundled dependencies (ev, stduuid) MUST be in the same export set so
    # CMake rewrites their names with the qb:: namespace in the transitive
    # dependency list of qb::io / qb::core.
    set(_QB_INSTALL_TARGETS)
    if(TARGET qb-io)
        list(APPEND _QB_INSTALL_TARGETS qb-io)
    endif()
    if(TARGET qb-core)
        list(APPEND _QB_INSTALL_TARGETS qb-core)
    endif()
    if(TARGET ev)
        list(APPEND _QB_INSTALL_TARGETS ev)
    endif()
    if(TARGET stduuid)
        list(APPEND _QB_INSTALL_TARGETS stduuid)
    endif()
    
    # Install library targets with an EXPORT set so CMake can generate
    # the *Targets.cmake files required by find_package(qb).
    if(_QB_INSTALL_TARGETS)
        install(TARGETS ${_QB_INSTALL_TARGETS}
            EXPORT qbTargets
            RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()
    
    # Install the export set
    install(EXPORT qbTargets
        FILE        qbTargets.cmake
        NAMESPACE   qb::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
    )
    
    # Install public headers
    install(
        DIRECTORY "${QB_INCLUDE_DIR}/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING 
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.tpp"
    )
    
    # Install header-only internal modules (nlohmann, ska_hash, nanolog)
    foreach(_hdr_mod ${QB_HEADER_ONLY_MODULES})
        set(_hdr_mod_dir "${QB_MODULES_DIR}/${_hdr_mod}")
        if(IS_DIRECTORY "${_hdr_mod_dir}")
            install(
                DIRECTORY "${_hdr_mod_dir}/"
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${_hdr_mod}
                FILES_MATCHING
                    PATTERN "*.h"
                    PATTERN "*.hpp"
            )
        endif()
    endforeach()
    
    # Version compatibility file
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/qbConfigVersion.cmake"
        VERSION ${QB_FRAMEWORK_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    
    # Package config file (from template, for find_package(qb))
    configure_package_config_file(
        "${QB_CMAKE_DIR}/qbConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/qbInstallConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
    )
    
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qbInstallConfig.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
        RENAME qbConfig.cmake
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qbConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
    )
    
    # Install FindArgon2 module so consumers can resolve the Argon2 dependency
    if(QB_HAS_ARGON2)
        install(FILES "${QB_CMAKE_DIR}/FindArgon2.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
        )
    endif()

    # Install FindNgtcp2 so find_package(qb) consumers of a QUIC-enabled build can
    # recreate the Ngtcp2:: imported targets that qb::io links transitively.
    if(QB_HAS_QUIC)
        install(FILES "${QB_CMAKE_DIR}/FindNgtcp2.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
        )
    endif()
    
    qb_status_message("Installation configured:")
    qb_status_message("  Prefix: ${CMAKE_INSTALL_PREFIX}")
    qb_status_message("  Libraries: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
    qb_status_message("  Headers: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
    qb_status_message("  CMake: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/qb")
endif()

# -----------------------------------------------------------------------------
# Export Framework Configuration
# -----------------------------------------------------------------------------
# Mark framework as successfully loaded
set(QB_FOUND TRUE CACHE BOOL "qb framework found and configured")
set(QB_LIBRARIES ${QB_ALL_LIBRARIES} CACHE STRING "All qb libraries")
set(QB_INCLUDE_DIRS "${QB_INCLUDE_DIR};${QB_MODULES_DIR}" CACHE STRING "qb include directories")

# Set target variables for compatibility
set(QB_CORE_TARGET qb::core)
set(QB_IO_TARGET qb::io)
set(QB_EV_TARGET ev)

# -----------------------------------------------------------------------------
# Parent Scope Export (for add_subdirectory usage)
# -----------------------------------------------------------------------------
get_directory_property(hasParent PARENT_DIRECTORY)
if(hasParent)
    # Export all important variables to parent scope
    set(QB_FOUND TRUE PARENT_SCOPE)
    set(QB_CORE_FOUND ${QB_CORE_FOUND} PARENT_SCOPE)
    set(QB_IO_FOUND ${QB_IO_FOUND} PARENT_SCOPE)
    set(QB_EV_FOUND ${QB_EV_FOUND} PARENT_SCOPE)
    
    # Export targets
    set(QB_CORE_TARGET ${QB_CORE_TARGET} PARENT_SCOPE)
    set(QB_IO_TARGET ${QB_IO_TARGET} PARENT_SCOPE)
    set(QB_EV_TARGET ${QB_EV_TARGET} PARENT_SCOPE)
    
    # Export library lists
    set(QB_LIBRARIES ${QB_LIBRARIES} PARENT_SCOPE)
    set(QB_ALL_LIBRARIES ${QB_ALL_LIBRARIES} PARENT_SCOPE)
    set(QB_INCLUDE_DIRS "${QB_INCLUDE_DIR};${QB_MODULES_DIR}" PARENT_SCOPE)
    
    # Export configuration
    set(QB_FRAMEWORK_VERSION ${QB_FRAMEWORK_VERSION} PARENT_SCOPE)
    set(QB_FRAMEWORK_VERSION_MAJOR ${QB_FRAMEWORK_VERSION_MAJOR} PARENT_SCOPE)
    set(QB_FRAMEWORK_VERSION_MINOR ${QB_FRAMEWORK_VERSION_MINOR} PARENT_SCOPE)
    set(QB_FRAMEWORK_VERSION_PATCH ${QB_FRAMEWORK_VERSION_PATCH} PARENT_SCOPE)
    set(QB_HAS_SSL ${QB_HAS_SSL} PARENT_SCOPE)
    set(QB_HAS_COMPRESSION ${QB_HAS_COMPRESSION} PARENT_SCOPE)
    set(QB_HAS_ARGON2 ${QB_HAS_ARGON2} PARENT_SCOPE)
    set(QB_HAS_QUIC ${QB_HAS_QUIC} PARENT_SCOPE)
    set(QB_WITH_QUIC ${QB_WITH_QUIC} PARENT_SCOPE)
    set(QB_SSL_RESOURCES ${QB_SSL_RESOURCES} PARENT_SCOPE)
    
    # Export build flags and settings
    set(QB_BUILD_TESTS ${QB_BUILD_TESTS} PARENT_SCOPE)
    set(QB_BUILD_BENCHMARKS ${QB_BUILD_BENCHMARKS} PARENT_SCOPE)
    set(QB_HAS_GTEST ${QB_HAS_GTEST} PARENT_SCOPE)
    set(QB_HAS_BENCHMARK ${QB_HAS_BENCHMARK} PARENT_SCOPE)
    
    # Export compatibility variables
    set(OPENSSL_FOUND ${QB_HAS_SSL} PARENT_SCOPE)
    set(ZLIB_FOUND ${QB_HAS_COMPRESSION} PARENT_SCOPE)
    set(QB_IO_WITH_SSL ${QB_HAS_SSL} PARENT_SCOPE)
    
    qb_status_message("Framework variables exported to parent scope")
endif()

# Print final summary
qb_print_dependencies()

qb_status_message("========================================")
qb_status_message("qb Framework Successfully Configured!")
qb_status_message("Available libraries: ${QB_ALL_LIBRARIES}")
qb_status_message("Available targets: qb::core, qb::io")
qb_status_message("========================================")
