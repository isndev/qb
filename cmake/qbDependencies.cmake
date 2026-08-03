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
set(QB_UUID_DIR "${QB_VENDOR_DIR}/uuid")

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
# qb uses a custom fork of libev, vendored under include/qb/vendor/qev
set(QB_EV_DIR "${QB_VENDOR_DIR}/qev")

if(EXISTS "${QB_EV_DIR}")
    set(LIBEV_FOUND TRUE)
    set(QB_HAS_LIBEV TRUE)
    qb_status_message("Using bundled libev from: ${QB_EV_DIR}")
    
    # The bundled libev will be built as part of the internal modules
    # We don't need to create an imported target here as it will be handled
    # by the internal module system
else()
    qb_error_message("Bundled libev not found at: ${QB_EV_DIR}")
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

# ZLIB (optional, for compression support). Zlib is CMake-buildable, so it supports
# the system-first / git-fallback policy (QB_DEPS_FETCH_FALLBACK).
if(QB_WITH_COMPRESSION)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        qb_status_message("Found ZLIB: ${ZLIB_VERSION_STRING}")
    elseif(QB_DEPS_FETCH_FALLBACK)
        qb_status_message("ZLIB not found on system - building from source (tag ${QB_ZLIB_GIT_TAG})")
        include(FetchContent)
        set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            zlib
            GIT_REPOSITORY https://github.com/madler/zlib.git
            GIT_TAG ${QB_ZLIB_GIT_TAG}
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(zlib)
        # madler/zlib exposes `zlib` (shared) and `zlibstatic` but no ZLIB::ZLIB target;
        # normalize so the rest of the build (and qb-io) can link ZLIB::ZLIB uniformly.
        if(NOT TARGET ZLIB::ZLIB)
            if(QB_BUILD_SHARED_LIBS AND TARGET zlib)
                add_library(ZLIB::ZLIB ALIAS zlib)
            elseif(TARGET zlibstatic)
                add_library(ZLIB::ZLIB ALIAS zlibstatic)
            elseif(TARGET zlib)
                add_library(ZLIB::ZLIB ALIAS zlib)
            endif()
        endif()
        if(TARGET ZLIB::ZLIB)
            set(ZLIB_FOUND TRUE)
        endif()
    endif()

    if(ZLIB_FOUND OR TARGET ZLIB::ZLIB)
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

# QUIC transport (optional, via libngtcp2). Tri-state QB_WITH_QUIC:
#   AUTO (default) - enable iff libngtcp2 is found; quiet (no warning) when absent
#   ON  / TRUE / 1 - require libngtcp2; warn and disable if it (or SSL) is missing
#   OFF / FALSE/ 0 - disabled outright
string(TOUPPER "${QB_WITH_QUIC}" _qb_quic_mode)
if(_qb_quic_mode STREQUAL "OFF" OR _qb_quic_mode STREQUAL "FALSE" OR
   _qb_quic_mode STREQUAL "0" OR _qb_quic_mode STREQUAL "NO" OR _qb_quic_mode STREQUAL "N")
    qb_status_message("QUIC transport support disabled")
    set(QB_HAS_QUIC FALSE)
else()
    # AUTO or an explicit truthy value. `required` distinguishes ON (must find,
    # warn on failure) from AUTO (best-effort, stay quiet on failure).
    set(_qb_quic_required TRUE)
    if(_qb_quic_mode STREQUAL "AUTO")
        set(_qb_quic_required FALSE)
    endif()
    if(NOT QB_HAS_SSL)
        if(_qb_quic_required)
            qb_warning_message("QUIC requested but SSL/TLS support is disabled - QUIC support disabled")
        endif()
        set(QB_HAS_QUIC FALSE)
    else()
        find_package(Ngtcp2 QUIET)
        if(Ngtcp2_FOUND)
            qb_status_message("Found libngtcp2 QUIC transport stack")
            list(APPEND QB_EXTERNAL_LIBRARIES
                Ngtcp2::ngtcp2
                Ngtcp2::crypto_ossl
            )
            set(QB_HAS_QUIC TRUE)
        elseif(_qb_quic_required)
            qb_warning_message("libngtcp2 not found - QUIC support disabled")
            set(QB_HAS_QUIC FALSE)
        else()
            qb_status_message("libngtcp2 not found - QUIC transport auto-disabled")
            set(QB_HAS_QUIC FALSE)
        endif()
    endif()
endif()

# Google Test / Google Benchmark: resolved in qbFetchGoogleDeps.cmake (FetchContent or
# find_package when QB_USE_SYSTEM_* is ON). Defaults are set here for summary printing.
if(NOT QB_BUILD_TESTS)
    set(QB_HAS_GTEST FALSE)
endif()
if(NOT QB_BUILD_BENCHMARKS)
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
# nlohmann/json — the ONE genuine third-party upstream dependency
# -----------------------------------------------------------------------------
# qev, uuid, nanolog and ska_hash are qb FORKS: qb's own code, never swappable, so qb owns their
# include path (qb/vendor/...). nlohmann is the opposite -- it is upstream, widely deployed, and a
# consumer very plausibly already has it. That asymmetry has a consequence that no amount of
# path-renaming can fix: nlohmann::json is a TYPE that crosses the API boundary (qb::json is an
# alias for it, qb/json.h defines to_json/from_json for qb::uuid). If qb compiled against a private
# copy and the consumer against theirs, the program holds two definitions of the same class -- an
# ODR violation. Re-prefixing the header path would not change that; it would only make the two
# copies harder to notice. The only real fix is to use the CONSUMER's copy, so: find_package first.
#
# nlohmann does encode SOME of its configuration in an inline namespace (json_abi_v3_12_0, plus
# _diag / _ldvcmp suffixes, json.hpp:85-115), which turns a version or diagnostics mismatch into a
# LINK error -- loud, fine. But JSON_NOEXCEPTION and JSON_USE_IMPLICIT_CONVERSIONS change the class
# and do NOT feed the tag: those still mismatch silently. That residue is inherent to shipping a
# prebuilt library whose API exposes a header-configurable third-party type; it is only fully
# closed by building qb in the consumer's own tree (FetchContent / add_subdirectory).
find_package(nlohmann_json 3.11 QUIET)

add_library(qb-nlohmann INTERFACE)
set_target_properties(qb-nlohmann PROPERTIES EXPORT_NAME nlohmann)
include(GNUInstallDirs)

if(nlohmann_json_FOUND)
    set(QB_USES_SYSTEM_NLOHMANN TRUE)
    target_link_libraries(qb-nlohmann INTERFACE nlohmann_json::nlohmann_json)
    qb_status_message("Using system nlohmann_json ${nlohmann_json_VERSION}")
else()
    set(QB_USES_SYSTEM_NLOHMANN FALSE)
    # Fallback: the bundled copy in modules/. SYSTEM so a third party's warnings are not qb's
    # problem. <nlohmann/json.hpp> stays that library's canonical spelling -- deliberately, because
    # a consumer who has their own copy SHOULD win the include race here (the opposite of the
    # forks, where any consumer header winning was the bug).
    target_include_directories(qb-nlohmann SYSTEM INTERFACE
        "$<BUILD_INTERFACE:${QB_MODULES_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    qb_status_message("Using bundled nlohmann_json from: ${QB_MODULES_DIR}/nlohmann")
endif()

# -----------------------------------------------------------------------------
# Internal Dependencies (qb modules)
# -----------------------------------------------------------------------------
set(QB_INTERNAL_MODULES_PATH "${QB_MODULES_DIR}")

# Third-party header-only trees still shipped from modules/ (installed verbatim at their canonical
# path by qb/CMakeLists.txt). The qb-owned forks are NOT here: they live under include/qb/vendor/
# and are covered by qb's ordinary public-header install rule.
set(QB_HEADER_ONLY_MODULES nlohmann)

# Internal modules list (for logging / diagnostics only)
set(QB_INTERNAL_MODULES qev uuid nanolog ska_hash ${QB_HEADER_ONLY_MODULES})

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

    if(QB_HAS_QUIC)
        target_compile_definitions(${target} PRIVATE QB_HAS_QUIC=1)
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
# enable_testing() is called from the root project (qb-dev/CMakeLists.txt)
# to ensure ctest discovers all tests. Do not duplicate here.

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
    qb_status_message("    QUIC/libngtcp2: ${QB_HAS_QUIC}")
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

if(QB_HAS_QUIC)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_QUIC=1")
endif()

if(UUID_FOUND)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_UUID=1")
endif()

if(LIBEV_FOUND)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_LIBEV=1")
endif()

# Publish the finalized compile definitions to CACHE INTERNAL so qb's common
# target usage properties see the full set from every scope (qb itself relies on
# the local variable; qbm/* and examples, added by the top-level project, rely on
# this cache copy). Same rationale as QB_CXX_FLAGS_* in qbCompiler and
# QB_PLATFORM_* in qbConfig.
set(QB_COMPILE_DEFINITIONS "${QB_COMPILE_DEFINITIONS}" CACHE INTERNAL "")

# Mark dependencies as loaded
set(QB_DEPENDENCIES_LOADED TRUE CACHE INTERNAL "qb dependencies loaded")
