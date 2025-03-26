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
# qb - Main CMake Configuration
# 
# This file configures the build system for the qb Actor Framework, handling
# compiler settings, dependencies, build targets, and testing infrastructure.
# -----------------------------------------------------------------------------

# Minimum required CMake version for C++17 support and modern CMake features
cmake_minimum_required(VERSION 3.13)

# Set policies to use newer CMake behaviors if available
if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW) # Use <PackageName>_ROOT variables
endif()

# Define the project with language and description
project(qb 
    VERSION 2.0.0.0
    LANGUAGES CXX
    DESCRIPTION "C++17 Actor Framework"
    HOMEPAGE_URL "https://cpp.actor"
)

# -----------------------------------------------------------------------------
# Global project configuration and versioning
# -----------------------------------------------------------------------------
# Extract version components from project version
set(QB_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(QB_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(QB_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(QB_VERSION_TWEAK ${PROJECT_VERSION_TWEAK})
set(QB_VERSION "${PROJECT_VERSION}")

# Define project-wide prefixes for variables
set(QB_PREFIX ${PROJECT_NAME})
string(TOUPPER ${QB_PREFIX} QB_PREFIX_UPPER)
set(QB_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set(QB_PATH ${CMAKE_CURRENT_SOURCE_DIR})

# -----------------------------------------------------------------------------
# Compiler configuration
# -----------------------------------------------------------------------------
# Set C++17 as required standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(${QB_PREFIX_UPPER}_CXX_FLAGS "")

# Choose stricter warning flags for different compilers
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(${QB_PREFIX_UPPER}_WARNING_FLAGS "-Wall -Wextra -Wpedantic -Werror")
elseif(MSVC)
    set(${QB_PREFIX_UPPER}_WARNING_FLAGS "/W4 /WX")
endif()

# Add custom CMake modules path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Disable non-standard C++ extensions except on Cygwin
if(NOT CYGWIN)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Export compile commands for IDE integration and static analysis tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Uncomment to enable verbose output during build
#set(CMAKE_VERBOSE_MAKEFILE ON)

# -----------------------------------------------------------------------------
# Build options
# -----------------------------------------------------------------------------
# Core build options
option(BUILD_SHARED_LIBS "Build shared libraries instead of static libraries" OFF)
option(${QB_PREFIX_UPPER}_BUILD_TEST "Build qb Tests" OFF)
option(${QB_PREFIX_UPPER}_BUILD_BENCHMARK "Build qb Benchmarks" OFF)
option(${QB_PREFIX_UPPER}_BUILD_DOC "Build qb Documentation" OFF)
option(${QB_PREFIX_UPPER}_BUILD_EXAMPLES "Build qb examples" OFF)
option(${QB_PREFIX_UPPER}_INSTALL "Generate installation target" ON)

# Performance and debugging options
option(${QB_PREFIX_UPPER}_WITH_LOG "Enable nanolog" ON)
option(${QB_PREFIX_UPPER}_STDOUT_LOG "Print logs to stdout" ON)
option(${QB_PREFIX_UPPER}_WITH_TCMALLOC "Use TCMalloc for improved performance" OFF)
option(${QB_PREFIX_UPPER}_BUILD_COVERAGE "Enable code coverage reporting" OFF)

# Set target architecture (defaults to 'native' for best performance)
if (NOT ${QB_PREFIX_UPPER}_BUILD_ARCH)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
        set(${QB_PREFIX_UPPER}_BUILD_ARCH "native")
    else()
        set(${QB_PREFIX_UPPER}_BUILD_ARCH "")
    endif()
endif()

# Default build type to Release if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Display configuration summary
message(STATUS "=== ${PROJECT_NAME} Configuration ===")
message(STATUS "Version: ${QB_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build architecture: ${${QB_PREFIX_UPPER}_BUILD_ARCH}")
message(STATUS "Build tests: ${${QB_PREFIX_UPPER}_BUILD_TEST}")
message(STATUS "Build benchmarks: ${${QB_PREFIX_UPPER}_BUILD_BENCHMARK}")
message(STATUS "Enable logging: ${${QB_PREFIX_UPPER}_WITH_LOG}")
message(STATUS "Enable TCMalloc: ${${QB_PREFIX_UPPER}_WITH_TCMALLOC}")
message(STATUS "===================================")

# -----------------------------------------------------------------------------
# Output directories configuration
# -----------------------------------------------------------------------------
# Use standardized output directory structure
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# -----------------------------------------------------------------------------
# Testing and benchmarking configuration
# -----------------------------------------------------------------------------
# Check if googletest module is available when tests are enabled
if (${QB_PREFIX_UPPER}_BUILD_TEST AND NOT EXISTS "${QB_PATH}/modules/googletest/CMakeLists.txt")
    message(WARNING "Cannot build tests: googletest module not loaded, try -> git submodule update --init --recursive")
    set(${QB_PREFIX_UPPER}_BUILD_TEST OFF)
endif()

# Check if googlebenchmark module is available when benchmarks are enabled
if (${QB_PREFIX_UPPER}_BUILD_BENCHMARK AND NOT EXISTS "${QB_PATH}/modules/googlebenchmark/CMakeLists.txt")
    message(WARNING "Cannot build benchmarks: googlebenchmark module not loaded, try -> git submodule update --init --recursive")
    set(${QB_PREFIX_UPPER}_BUILD_BENCHMARK OFF)
endif()

# Configure test environment
set(BUILD_TESTING OFF)
if (${QB_PREFIX_UPPER}_BUILD_TEST)
    include(CTest)
    enable_testing()
    if (${QB_PREFIX_UPPER}_BUILD_BENCHMARK)
        # Configure and add Google Benchmark
        set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(GOOGLEBENCHMARK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/googlebenchmark)
        message(STATUS "Adding Google Benchmark: ${GOOGLEBENCHMARK_DIR}")
        add_subdirectory(${GOOGLEBENCHMARK_DIR})
    endif()
    
    # Configure Google Test with MSVC shared runtime if needed
    if (MSVC)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    endif()
    
    # Add Google Test framework
    set(GOOGLETEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/googletest)
    add_subdirectory(${GOOGLETEST_DIR})
endif()

# -----------------------------------------------------------------------------
# Code coverage configuration
# -----------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32 AND ${QB_PREFIX_UPPER}_BUILD_COVERAGE)
    message(STATUS "Code coverage enabled")
    include(cmake/CodeCoverage.cmake)
    APPEND_COVERAGE_COMPILER_FLAGS()
    
    # Check for required tools
    find_program(LCOV_PATH lcov REQUIRED)
    find_program(GCOV_PATH gcov REQUIRED)
    find_package(Python REQUIRED)
    
    if (LCOV_PATH AND GCOV_PATH AND Python_FOUND)
        set(${QB_PREFIX_UPPER}_COVERAGE ON)
    else()
        message(WARNING "Coverage requested but tools not found (lcov, gcov, python)")
        set(${QB_PREFIX_UPPER}_COVERAGE OFF)
    endif()
endif()

# -----------------------------------------------------------------------------
# Compiler and linker configuration
# -----------------------------------------------------------------------------
# Load custom compiler/linker configuration
include(cmake/utils.cmake)
config_compiler_and_linker()  # Defined in internal_utils.cmake.

# -----------------------------------------------------------------------------
# Include paths
# -----------------------------------------------------------------------------
# Use target_include_directories for all targets instead of global include
# but we keep these for backward compatibility
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/modules)

# -----------------------------------------------------------------------------
# External dependencies
# -----------------------------------------------------------------------------
# Optional dependencies for specific modules
find_package(ZLIB)
if (ZLIB_FOUND)
    message(STATUS "ZLIB found: ${ZLIB_VERSION}")
else()
    message(STATUS "ZLIB not found, some modules requiring libz will be skipped")
endif()

find_package(OpenSSL)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    # SSL resources needed for tests
    set(${QB_PREFIX_UPPER}_SSL_RESOURCES 
        ${CMAKE_CURRENT_SOURCE_DIR}/ressources/ssl/key.pem 
        ${CMAKE_CURRENT_SOURCE_DIR}/ressources/ssl/cert.pem
    )
else()
    message(STATUS "OpenSSL not found, some modules requiring libssl will be skipped")
endif()

# -----------------------------------------------------------------------------
# Performance optimization - TCMalloc configuration (Linux only)
# -----------------------------------------------------------------------------
if (NOT WIN32 AND ${QB_PREFIX_UPPER}_WITH_TCMALLOC)
    set(GPERFTOOLS_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    find_package(Gperftools)
    if (GPERFTOOLS_FOUND)
        message(STATUS "TCMalloc found and will be used")
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
            set(${QB_PREFIX_UPPER}_CXX_FLAGS "-flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
        endif()
        link_libraries(unwind)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${${QB_PREFIX_UPPER}_CXX_FLAGS}")
        link_libraries(${GPERFTOOLS_TCMALLOC})
    else()
        message(WARNING "TCMalloc requested but not found, performance may be affected")
        set(${QB_PREFIX_UPPER}_WITH_TCMALLOC OFF)
    endif()
endif()

# -----------------------------------------------------------------------------
# Component configuration and targets
# -----------------------------------------------------------------------------
# UUID library configuration
set(UUID_BUILD_TESTS OFF)
set(UUID_SYSTEM_GENERATOR OFF)
set(UUID_TIME_GENERATOR OFF)
set(UUID_USING_CXX20_SPAN OFF)
add_subdirectory(modules/uuid)

# Core qb components
add_subdirectory(modules/ev)
add_subdirectory(source/io)
add_subdirectory(source/core)

# Documentation
add_subdirectory(docs)

# Add examples if enabled
if(${QB_PREFIX_UPPER}_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# -----------------------------------------------------------------------------
# Coverage target setup
# -----------------------------------------------------------------------------
if (${QB_PREFIX_UPPER}_COVERAGE)
    set(COVERAGE_LCOV_EXCLUDES
            '/usr/*'
            '*benchmark*'
            '*modules*'
            '*sample/*'
            '*examples/*'
            '*test*'
            )
    SETUP_TARGET_FOR_COVERAGE_LCOV(NAME ${PROJECT_NAME}-coverage
            EXECUTABLE ctest
            DEPENDENCIES qb-core)
            
    set(COVERAGE_GCOVR_EXCLUDES
            ".*benchmark.*"
            ".*modules.*"
            ".*sample.*"
            ".*examples.*"
            ".*test.*"
            )
    SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(NAME ${PROJECT_NAME}-coverage-xml
            EXECUTABLE ctest
            DEPENDENCIES qb-core)
endif()

# -----------------------------------------------------------------------------
# Installation configuration
# -----------------------------------------------------------------------------
if(${QB_PREFIX_UPPER}_INSTALL)
    include(GNUInstallDirs)
    
    # Install headers
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
                       PATTERN "*.hpp"
    )
    
    # Generate and install CMake package configuration files
    include(CMakePackageConfigHelpers)
    
    # Create version file
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/qbConfigVersion.cmake"
        VERSION ${QB_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    
    # Create a config file that loads all our exported targets
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qbConfig.cmake"
        "# qb CMake configuration file\n\n"
        "# Compute the installation prefix relative to this file\n"
        "get_filename_component(_IMPORT_PREFIX \"\${CMAKE_CURRENT_LIST_FILE}\" PATH)\n"
        "get_filename_component(_IMPORT_PREFIX \"\${_IMPORT_PREFIX}\" PATH)\n"
        "get_filename_component(_IMPORT_PREFIX \"\${_IMPORT_PREFIX}\" PATH)\n\n"
        "# Load all exported targets\n"
        "# First load event loop library\n"
        "include(\"\${_IMPORT_PREFIX}/lib/cmake/qb/evTargets.cmake\" OPTIONAL)\n"
        "# Then load IO library which depends on ev\n"
        "include(\"\${_IMPORT_PREFIX}/lib/cmake/qb/qb-ioTargets.cmake\" OPTIONAL)\n"
        "# Finally load core library which depends on io\n"
        "include(\"\${_IMPORT_PREFIX}/lib/cmake/qb/qb-coreTargets.cmake\" OPTIONAL)\n\n"
        "# Note: If any dependencies are missing, find_package may be used to locate them here\n\n"
        "# Set QB_FOUND to TRUE\n"
        "set(QB_FOUND TRUE)\n"
        "set(QB_VERSION ${QB_VERSION})\n"
        "set(QB_INCLUDE_DIRS \"\${_IMPORT_PREFIX}/include\")\n"
        "set(QB_LIBRARIES qb::qb-core)\n"
        "set(QB_IO_LIBRARIES qb::qb-io)\n"
    )
    
    # Install the config and version files
    install(
        FILES 
            "${CMAKE_CURRENT_BINARY_DIR}/qbConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/qbConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
    )
    
    # Display installation summary
    message(STATUS "Installation configuration:")
    message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
    message(STATUS "  Library installation path: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
    message(STATUS "  Header installation path: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
    message(STATUS "  CMake configuration installation path: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/qb")
endif()

# -----------------------------------------------------------------------------
# Variables propagation to parent scope
# -----------------------------------------------------------------------------
# Share configuration with parent CMake context if this is a subproject
get_directory_property(hasParent PARENT_DIRECTORY)
if(hasParent)
    set(CMAKE_CXX_FLAGS "${${QB_PREFIX_UPPER}_CXX_FLAGS}" PARENT_SCOPE)
    set(QB_PREFIX "${QB_PREFIX}" PARENT_SCOPE)
    set(QB_PREFIX_UPPER "${QB_PREFIX_UPPER}" PARENT_SCOPE)
    set(QB_DIRECTORY "${QB_DIRECTORY}" PARENT_SCOPE)
    set(OPENSSL_FOUND "${OPENSSL_FOUND}" PARENT_SCOPE)
    set(ZLIB_FOUND "${ZLIB_FOUND}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_BUILD_ARCH "${${QB_PREFIX_UPPER}_BUILD_ARCH}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_BUILD_TEST "${${QB_PREFIX_UPPER}_BUILD_TEST}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_BUILD_BENCHMARK "${${QB_PREFIX_UPPER}_BUILD_BENCHMARK}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_BUILD_COVERAGE "${${QB_PREFIX_UPPER}_BUILD_COVERAGE}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_SSL_RESOURCES "${${QB_PREFIX_UPPER}_SSL_RESOURCES}" PARENT_SCOPE)
endif()