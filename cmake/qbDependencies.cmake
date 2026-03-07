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
# qb Framework - Dependencies Module
#
# This file finds and configures all external dependencies required by the
# qb framework. It provides intelligent dependency resolution and optional
# feature detection.
# -----------------------------------------------------------------------------

if(QB_DEPENDENCIES_INCLUDED)
    return()
endif()
set(QB_DEPENDENCIES_INCLUDED TRUE)

# -----------------------------------------------------------------------------
# Find Package Modules Path
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${QB_CMAKE_DIR}")

# -----------------------------------------------------------------------------
# Required Dependencies
# -----------------------------------------------------------------------------
qb_status_message("Searching for required dependencies...")

# UUID library (using internal bundled version)
set(QB_UUID_DIR "${QB_MODULES_DIR}/uuid")

if(EXISTS "${QB_UUID_DIR}")
    set(UUID_FOUND TRUE)
    set(QB_HAS_UUID TRUE)
    qb_status_message("Using bundled UUID from: ${QB_UUID_DIR}")
    
    # The bundled UUID will be built as part of the internal modules
else()
    # Try to find system UUID as fallback
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(UUID QUIET uuid)
    endif()
    
    if(NOT UUID_FOUND)
        find_path(UUID_INCLUDE_DIR uuid/uuid.h
            PATHS
            /usr/include
            /usr/local/include
            /opt/local/include
        )
        
        find_library(UUID_LIBRARY
            NAMES uuid
            PATHS
            /usr/lib
            /usr/local/lib
            /opt/local/lib
        )
        
        if(UUID_INCLUDE_DIR AND UUID_LIBRARY)
            set(UUID_FOUND TRUE)
            set(UUID_INCLUDE_DIRS ${UUID_INCLUDE_DIR})
            set(UUID_LIBRARIES ${UUID_LIBRARY})
            # Create an IMPORTED target so include dirs are propagated automatically.
            if(NOT TARGET UUID::UUID)
                add_library(UUID::UUID UNKNOWN IMPORTED)
                set_target_properties(UUID::UUID PROPERTIES
                    IMPORTED_LOCATION             "${UUID_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${UUID_INCLUDE_DIR}"
                )
            endif()
            list(APPEND QB_EXTERNAL_LIBRARIES UUID::UUID)
            qb_status_message("Found system UUID library: ${UUID_LIBRARIES}")
        endif()
    endif()
    
    if(NOT UUID_FOUND)
        qb_warning_message("No UUID library found (neither bundled nor system)")
        set(QB_HAS_UUID FALSE)
    else()
        set(QB_HAS_UUID TRUE)
    endif()
endif()

# -----------------------------------------------------------------------------
# libev (using internal bundled version)
# -----------------------------------------------------------------------------
# qb uses a custom bundled version of libev in modules/ev
set(QB_LIBEV_DIR "${QB_MODULES_DIR}/ev")

if(EXISTS "${QB_LIBEV_DIR}")
    set(LIBEV_FOUND TRUE)
    set(QB_HAS_LIBEV TRUE)
    qb_status_message("Using bundled libev from: ${QB_LIBEV_DIR}")
    
    # The bundled libev will be built as part of the internal modules
    # We don't need to create an imported target here as it will be handled
    # by the internal module system
else()
    qb_error_message("Bundled libev not found at: ${QB_LIBEV_DIR}")
    set(LIBEV_FOUND FALSE)
    set(QB_HAS_LIBEV FALSE)
endif()

# -----------------------------------------------------------------------------
# Optional Dependencies
# -----------------------------------------------------------------------------
qb_status_message("Searching for optional dependencies...")

# OpenSSL (optional, for SSL/TLS support)
if(QB_WITH_SSL)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        qb_status_message("Found OpenSSL: ${OPENSSL_VERSION}")
        list(APPEND QB_EXTERNAL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
        set(QB_HAS_SSL TRUE)
        
        # Find Argon2 for advanced cryptographic functions
        find_package(Argon2 QUIET)
        if(Argon2_FOUND)
            qb_status_message("Found Argon2: ${ARGON2_VERSION_STRING}")
            # Use the imported target so include dirs are propagated automatically.
            list(APPEND QB_EXTERNAL_LIBRARIES Argon2::Argon2)
            set(QB_HAS_ARGON2 TRUE)
        else()
            qb_status_message("Argon2 not found - using fallback crypto methods")
            set(QB_HAS_ARGON2 FALSE)
        endif()
    else()
        qb_warning_message("OpenSSL not found - SSL/TLS support disabled")
        set(QB_HAS_SSL FALSE)
        set(QB_WITH_SSL OFF)
    endif()
else()
    qb_status_message("SSL/TLS support disabled")
    set(QB_HAS_SSL FALSE)
endif()

# ZLIB (optional, for compression support)
if(QB_WITH_COMPRESSION)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        qb_status_message("Found ZLIB: ${ZLIB_VERSION_STRING}")
        list(APPEND QB_EXTERNAL_LIBRARIES ZLIB::ZLIB)
        set(QB_HAS_COMPRESSION TRUE)
    else()
        qb_warning_message("ZLIB not found - compression support disabled")
        set(QB_HAS_COMPRESSION FALSE)
        set(QB_WITH_COMPRESSION OFF)
    endif()
else()
    qb_status_message("Compression support disabled")
    set(QB_HAS_COMPRESSION FALSE)
endif()

# Google Test (optional, for testing)
if(QB_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        qb_status_message("Found Google Test: ${GTest_VERSION}")
        set(QB_HAS_GTEST TRUE)
    else()
        qb_status_message("Google Test not found - using internal implementation")
        set(QB_HAS_GTEST FALSE)
    endif()
else()
    set(QB_HAS_GTEST FALSE)
endif()

# Google Benchmark (optional, for benchmarking)
if(QB_BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        qb_status_message("Found Google Benchmark")
        set(QB_HAS_BENCHMARK TRUE)
    else()
        qb_status_message("Google Benchmark not found - using internal implementation")
        set(QB_HAS_BENCHMARK FALSE)
    endif()
else()
    set(QB_HAS_BENCHMARK FALSE)
endif()

# Gperftools (optional, for profiling)
if(QB_WITH_PROFILING)
    find_package(Gperftools QUIET)
    if(Gperftools_FOUND)
        qb_status_message("Found gperftools")
        # Prefer the imported targets created by FindGperftools so include dirs
        # and library paths are propagated transitively.
        if(TARGET Gperftools::Profiler)
            list(APPEND QB_EXTERNAL_LIBRARIES Gperftools::Profiler)
        endif()
        if(TARGET Gperftools::TCMalloc)
            list(APPEND QB_EXTERNAL_LIBRARIES Gperftools::TCMalloc)
        endif()
        set(QB_HAS_PROFILING TRUE)
    else()
        qb_warning_message("gperftools not found - profiling support disabled")
        set(QB_HAS_PROFILING FALSE)
        set(QB_WITH_PROFILING OFF)
    endif()
else()
    set(QB_HAS_PROFILING FALSE)
endif()

# -----------------------------------------------------------------------------
# Platform-Specific Dependencies
# -----------------------------------------------------------------------------
if(QB_PLATFORM_WINDOWS)
    # Windows-specific libraries
    list(APPEND QB_EXTERNAL_LIBRARIES ws2_32 mswsock)
    qb_status_message("Added Windows-specific libraries: ws2_32, mswsock")
    
elseif(QB_PLATFORM_LINUX)
    # Linux-specific libraries
    find_library(DL_LIBRARY dl)
    if(DL_LIBRARY)
        list(APPEND QB_EXTERNAL_LIBRARIES ${DL_LIBRARY})
    endif()
    
    find_library(RT_LIBRARY rt)
    if(RT_LIBRARY)
        list(APPEND QB_EXTERNAL_LIBRARIES ${RT_LIBRARY})
    endif()
    
    qb_status_message("Added Linux-specific libraries")
    
elseif(QB_PLATFORM_MACOS)
    # macOS-specific frameworks
    find_library(FOUNDATION_FRAMEWORK Foundation)
    if(FOUNDATION_FRAMEWORK)
        list(APPEND QB_EXTERNAL_LIBRARIES ${FOUNDATION_FRAMEWORK})
    endif()
    
    qb_status_message("Added macOS-specific frameworks")
endif()

# -----------------------------------------------------------------------------
# Internal Dependencies (qb modules)
# -----------------------------------------------------------------------------
# Add internal modules directory to the path
set(QB_INTERNAL_MODULES_PATH "${QB_MODULES_DIR}")

# Internal modules list
set(QB_INTERNAL_MODULES
    ev
    nanolog
    nlohmann
    ska_hash
    uuid
)

# Add internal modules to include path
foreach(module ${QB_INTERNAL_MODULES})
    if(EXISTS "${QB_INTERNAL_MODULES_PATH}/${module}")
        include_directories("${QB_INTERNAL_MODULES_PATH}/${module}")
        qb_debug_message("Added internal module: ${module}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Dependency Resolution Functions
# -----------------------------------------------------------------------------
function(qb_resolve_dependencies target)
    # Apply all external dependencies to the target
    if(QB_EXTERNAL_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${QB_EXTERNAL_LIBRARIES})
    endif()
    
    # Apply platform-specific dependencies
    if(CMAKE_USE_PTHREADS_INIT)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endif()
    
    # Apply optional dependencies based on features
    if(QB_HAS_SSL)
        target_compile_definitions(${target} PRIVATE QB_HAS_SSL=1)
    endif()
    
    if(QB_HAS_COMPRESSION)
        target_compile_definitions(${target} PRIVATE QB_HAS_COMPRESSION=1)
    endif()
    
    if(QB_HAS_ARGON2)
        target_compile_definitions(${target} PRIVATE QB_HAS_ARGON2=1)
    endif()
    
    if(UUID_FOUND)
        target_compile_definitions(${target} PRIVATE QB_HAS_UUID=1)
    endif()
    
    if(LIBEV_FOUND)
        target_compile_definitions(${target} PRIVATE QB_HAS_LIBEV=1)
    endif()
endfunction()

# Function to check if a dependency is available
function(qb_check_dependency name result)
    string(TOUPPER ${name} name_upper)
    if(DEFINED QB_HAS_${name_upper})
        set(${result} ${QB_HAS_${name_upper}} PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Function to require a dependency
function(qb_require_dependency name)
    qb_check_dependency(${name} available)
    if(NOT available)
        qb_error_message("Required dependency '${name}' not found")
    endif()
endfunction()

# -----------------------------------------------------------------------------
# SSL Certificate Configuration
# -----------------------------------------------------------------------------
if(QB_HAS_SSL)
    # Define SSL resources directory
    set(QB_SSL_RESOURCES_DIR "${QB_ROOT_DIR}/resources/ssl")
    
    # Set SSL resources for tests
    if(EXISTS "${QB_SSL_RESOURCES_DIR}")
        set(QB_SSL_RESOURCES "${QB_SSL_RESOURCES_DIR}")
        qb_debug_message("SSL resources directory: ${QB_SSL_RESOURCES}")
    else()
        qb_debug_message("SSL resources directory not found")
    endif()
endif()

# -----------------------------------------------------------------------------
# Test Framework Configuration
# -----------------------------------------------------------------------------
if(QB_BUILD_TESTS)
    # Enable testing
    enable_testing()
    
    # Configure test discovery
    if(QB_HAS_GTEST)
        include(GoogleTest)
        qb_status_message("Using system Google Test")
    else()
        qb_status_message("Using internal Google Test")
    endif()
endif()

# -----------------------------------------------------------------------------
# Benchmark Framework Configuration
# -----------------------------------------------------------------------------
if(QB_BUILD_BENCHMARKS)
    if(QB_HAS_BENCHMARK)
        qb_status_message("Using system Google Benchmark")
    else()
        qb_status_message("Using internal Google Benchmark")
    endif()
endif()

# -----------------------------------------------------------------------------
# Dependency Summary
# -----------------------------------------------------------------------------
function(qb_print_dependencies)
    qb_status_message("Dependencies Summary:")
    qb_status_message("  Required:")
    qb_status_message("    UUID: ${UUID_FOUND}")
    qb_status_message("    libev: ${LIBEV_FOUND}")
    qb_status_message("  Optional:")
    qb_status_message("    OpenSSL: ${QB_HAS_SSL}")
    if(QB_HAS_SSL)
        qb_status_message("    Argon2: ${QB_HAS_ARGON2}")
    endif()
    qb_status_message("    ZLIB: ${QB_HAS_COMPRESSION}")
    qb_status_message("    Google Test: ${QB_HAS_GTEST}")
    qb_status_message("    Google Benchmark: ${QB_HAS_BENCHMARK}")
    qb_status_message("    gperftools: ${QB_HAS_PROFILING}")
endfunction()

# -----------------------------------------------------------------------------
# Feature Definitions
# -----------------------------------------------------------------------------
# Set compile definitions based on available dependencies
if(QB_HAS_SSL)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_SSL=1")
endif()

if(QB_HAS_COMPRESSION)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_COMPRESSION=1")
endif()

if(QB_HAS_ARGON2)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_ARGON2=1")
endif()

if(UUID_FOUND)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_UUID=1")
endif()

if(LIBEV_FOUND)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_LIBEV=1")
endif()

# Update the global compile definitions
set(QB_COMPILE_DEFINITIONS ${QB_COMPILE_DEFINITIONS} PARENT_SCOPE)

# Mark dependencies as loaded
set(QB_DEPENDENCIES_LOADED TRUE CACHE INTERNAL "qb dependencies loaded") 