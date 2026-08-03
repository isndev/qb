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
# qb Framework - Configuration Module
#
# This file defines all the core configuration, constants, and options
# for the qb framework. It should be included early in the build process.
# -----------------------------------------------------------------------------

# Prevent multiple inclusions
if(QB_CONFIG_INCLUDED)
    return()
endif()
set(QB_CONFIG_INCLUDED TRUE)

# Include CMake built-in modules
include(CMakeParseArguments)

# -----------------------------------------------------------------------------
# Framework Information
# -----------------------------------------------------------------------------
set(QB_FRAMEWORK_NAME "qb")
set(QB_FRAMEWORK_DESCRIPTION "High-performance C++ Actor Framework")
set(QB_FRAMEWORK_VERSION "2.7.0")
set(QB_FRAMEWORK_VERSION_MAJOR 2)
set(QB_FRAMEWORK_VERSION_MINOR 7)
set(QB_FRAMEWORK_VERSION_PATCH 0)

# Framework paths
set(QB_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "qb framework root directory")
get_filename_component(QB_ROOT_DIR "${QB_ROOT_DIR}" ABSOLUTE)
set(QB_INCLUDE_DIR "${QB_ROOT_DIR}/include")
# CACHE INTERNAL so that QB_CMAKE_DIR is globally visible across all
# add_subdirectory() scopes (e.g. qbm modules added after the qb subtree).
# Without CACHE, the variable stays local to qb's subdirectory scope and
# qb_add_test / qb_add_benchmark cannot find deploy_runtime_dlls.cmake.
set(QB_CMAKE_DIR  "${QB_ROOT_DIR}/cmake"   CACHE INTERNAL "qb cmake scripts directory")
# modules/ now holds ONLY genuine third-party upstream code (nlohmann). Everything else that used
# to live there -- ev, uuid, nanolog, ska_hash -- is a qb FORK, i.e. qb's own source, and moved
# under include/qb/vendor/ so an installed qb owns every top-level name it drops in the consumer's
# include root. See QB_VENDOR_DIR below.
set(QB_MODULES_DIR "${QB_ROOT_DIR}/modules")
# qb-owned forks. Physically inside QB_INCLUDE_DIR on purpose: the build tree and the installed
# tree then expose them at the SAME relative path (qb/vendor/<fork>/...) through the SAME single
# include root, so there is no build-interface/install-interface pair to drift apart.
set(QB_VENDOR_DIR "${QB_INCLUDE_DIR}/qb/vendor")
set(QB_SOURCE_DIR "${QB_ROOT_DIR}/source")

# -----------------------------------------------------------------------------
# Build Configuration Options
# -----------------------------------------------------------------------------
# Defaults differ by role, deliberately. A STANDALONE checkout is the qb repo itself: tests and
# examples on. An EMBEDDED qb is somebody's dependency: they asked for a library, not for 350 test
# executables and a googletest download. Before this, a plain FetchContent of qb pulled googletest
# and built the whole suite by default.
#
# `BUILD_TESTING` is CMake's standard switch (CTest module) and is honoured when the consumer sets
# it, so `-DBUILD_TESTING=ON` still gets qb's tests in a superproject that wants them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_qb_default_tests ON)
    set(_qb_default_examples ON)
else()
    if(DEFINED BUILD_TESTING)
        set(_qb_default_tests ${BUILD_TESTING})
    else()
        set(_qb_default_tests OFF)
    endif()
    set(_qb_default_examples OFF)
endif()
option(QB_BUILD_TESTS "Build qb tests" ${_qb_default_tests})
option(QB_BUILD_EXAMPLES "Build qb examples" ${_qb_default_examples})
unset(_qb_default_tests)
unset(_qb_default_examples)
option(QB_BUILD_BENCHMARKS "Build qb benchmarks" OFF)
# Dependency resolution strategy:
#   QB_DEPS_FETCH_FALLBACK ON (default): each fetchable dependency is first looked up
#     on the system (find_package); if absent, it is built from source via FetchContent.
#     This is the "use system if present, else git" behavior.
#   QB_USE_SYSTEM_* ON: force find_package(... REQUIRED) and never fetch (fail if absent).
# Only CMake-native dependencies are fetchable (GoogleTest, Google Benchmark, Zlib).
# OpenSSL / Argon2 / libngtcp2 are never fetched (no clean CMake source build) — they
# must be provided by the system and gate optional features when absent.
option(QB_DEPS_FETCH_FALLBACK "Build fetchable deps from source (FetchContent) when not found on the system" ON)
# Force system packages (find_package REQUIRED, no FetchContent fallback) for these.
option(QB_USE_SYSTEM_GTEST "Require a system GTest (find_package CONFIG REQUIRED), never fetch" OFF)
option(QB_USE_SYSTEM_BENCHMARK "Require a system Google Benchmark (find_package CONFIG REQUIRED), never fetch" OFF)

set(QB_GOOGLETEST_GIT_TAG "v1.15.2" CACHE STRING "Git tag (or SHA) for FetchContent googletest")
set(QB_GOOGLEBENCHMARK_GIT_TAG "v1.9.2" CACHE STRING "Git tag (or SHA) for FetchContent googlebenchmark")
set(QB_ZLIB_GIT_TAG "v1.3.1" CACHE STRING "Git tag (or SHA) for the FetchContent zlib fallback build")
mark_as_advanced(QB_GOOGLETEST_GIT_TAG QB_GOOGLEBENCHMARK_GIT_TAG QB_ZLIB_GIT_TAG)
option(QB_BUILD_DOCS "Build qb documentation" OFF)
# Defaults to the standard BUILD_SHARED_LIBS so `cmake -DBUILD_SHARED_LIBS=ON` also
# switches qb to shared, while still allowing an explicit qb-only override.
option(QB_BUILD_SHARED_LIBS "Build qb libraries as shared objects instead of static" ${BUILD_SHARED_LIBS})
# Standalone: qb owns the install. Embedded: it does NOT -- an embedded qb adding its own
# install(TARGETS/DIRECTORY) rules injects qb's headers and CMake package files into the PARENT's
# `cmake --install`, so a consumer packaging their own app silently ships qb's development tree.
# A superproject that genuinely wants to install qb sets -DQB_INSTALL=ON.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(QB_INSTALL "Install qb framework" ON)
else()
    option(QB_INSTALL "Install qb framework" OFF)
endif()
# Same contract for the qbm modules registered through qb_register_module(): a module's
# install/export rules run only when the build that owns them asked to be installable.
# Defaults to QB_INSTALL because the two are never usefully split -- an installed qbm-http
# whose find_dependency(qb) has nothing to find is not a package.
option(QBM_INSTALL "Install qbm modules registered via qb_register_module()" ${QB_INSTALL})

# Performance options
option(QB_ENABLE_OPTIMIZATIONS "Enable performance optimizations" ON)
option(QB_ENABLE_LTO "Enable Link Time Optimization" OFF)
# ON by default: tune codegen for the build-host CPU (-march=native / -mcpu=native).
# Gives the best performance on the machine that built it. Turn OFF for portable/
# distributable binaries that must run on a different (possibly older) CPU.
# OFF by default: `-march=native` bakes the BUILD machine's instruction set into the artefact, so a
# binary built on a newer CPU dies with SIGILL on an older one. That is the right default for a
# library other people consume and ship; a benchmark run turns it on explicitly.
option(QB_ENABLE_NATIVE_ARCH "Enable native architecture optimizations (-march=native)" OFF)
option(QB_ENABLE_FAST_MATH "Enable -ffast-math / /fp:fast (breaks IEEE-754 compliance)" OFF)

# Coverage
option(QB_BUILD_COVERAGE "Enable code coverage instrumentation (Debug builds only)" OFF)

# Feature options
option(QB_WITH_LOGGING "Enable logging support" ON)
option(QB_WITH_SSL "Enable SSL/TLS support" ON)
option(QB_WITH_COMPRESSION "Enable compression support" ON)
# Tri-state: AUTO (enable iff libngtcp2 is found, quiet when absent), ON (require
# it, warn if missing), OFF (disabled). AUTO mirrors how SSL/compression behave.
set(QB_WITH_QUIC "AUTO" CACHE STRING "QUIC transport via libngtcp2: AUTO, ON, or OFF")
set_property(CACHE QB_WITH_QUIC PROPERTY STRINGS AUTO ON OFF)
option(QB_WITH_PROFILING "Enable profiling support" OFF)

# Debug options
option(QB_DEBUG_MEMORY "Enable memory debugging (legacy alias for QB_SANITIZE=address,undefined)" OFF)
option(QB_DEBUG_ACTOR "Enable actor debugging" OFF)
option(QB_STDOUT_LOGGING "Enable stdout logging fallback" OFF)

# Sanitizers: comma-separated list applied to every qb / qbm / test target and their
# link step (e.g. "address,undefined", "thread", "memory", "leak"). Empty = disabled.
# Use the `sanitize` / `sanitize-thread` CMake presets for ready-made configurations.
set(QB_SANITIZE "" CACHE STRING "Sanitizers to enable (comma-separated, e.g. address,undefined); empty = off")
# Back-compat: QB_DEBUG_MEMORY historically turned on ASan + UBSan.
if(QB_DEBUG_MEMORY AND NOT QB_SANITIZE)
    set(QB_SANITIZE "address,undefined")
endif()

# -----------------------------------------------------------------------------
# Build Type Configuration
# -----------------------------------------------------------------------------
# Default to Release for a STANDALONE build only. An embedded qb must not pick a build type for its
# parent, and on a multi-config generator (Visual Studio, Ninja Multi-Config) CMAKE_BUILD_TYPE is
# empty BY DESIGN -- writing it there is meaningless at best and misleading at worst, since the
# per-config choice is made at build time.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Define available build types
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

# Emit compile_commands.json for clangd / CLion / tooling. Polite: only set a default,
# so a parent project embedding qb can still override it.
if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# -----------------------------------------------------------------------------
# Compiler Configuration
# -----------------------------------------------------------------------------
set(QB_CXX_STANDARD 20 CACHE STRING "C++ standard required by qb targets")
set_property(CACHE QB_CXX_STANDARD PROPERTY STRINGS 20 23)
set(_QB_SUPPORTED_CXX_STANDARDS 20 23)
if(NOT QB_CXX_STANDARD IN_LIST _QB_SUPPORTED_CXX_STANDARDS)
    message(FATAL_ERROR "qb supports QB_CXX_STANDARD=20 or 23")
endif()

# Only touch the GLOBAL standard variables when qb is the top-level project.
#
# Setting them unconditionally leaks qb's language level into any parent that consumes this
# framework through add_subdirectory() or FetchContent: the parent's own sources get recompiled at
# qb's standard, and a toolchain that already set one is silently overridden --
#   "Warning: Standard CMAKE_CXX_STANDARD value defined in conan_toolchain.cmake to 20
#    has been modified to 17 by .../qb-src/cmake/qbConfig.cmake"
# Reported as isndev/qb#9, against the C++17 line; the 2.6 line had the same defect with a
# different value.
#
# Nothing is lost by scoping it. qb's targets get their language level as a USAGE REQUIREMENT --
# `target_compile_features(<t> PUBLIC cxx_std_${QB_CXX_STANDARD})` in qbFunctions.cmake -- which is
# both stricter (it applies to qb's own targets whatever the parent sets) and transitive (anything
# linking qb-core / qb-io / a qbm module is compiled at least at that level). The global variables
# were only ever a default for the standalone build.
#
# `PROJECT_IS_TOP_LEVEL` is not usable here: this file is included BEFORE project(). Comparing the
# source directories is the equivalent test at this point.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_CXX_STANDARD ${QB_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Position-independent code. Required so the bundled static dependencies (ev, llhttp) can be linked
# into a shared qb-io / qbm-* (or into any consumer's shared library) without "recompile with -fPIC"
# link errors on Linux.
#
# qb's OWN targets carry this as a target property (see _qb_apply_target_properties), which is where
# the requirement actually belongs. The global below is set only for a standalone build, where it
# also covers the vendored third-party targets qb does not create itself. Setting it unconditionally
# forced -fPIC onto every target of any project embedding qb -- the same parent-scope pollution as
# CMAKE_CXX_STANDARD above, and equally unnecessary.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# -----------------------------------------------------------------------------
# Library Configuration
# -----------------------------------------------------------------------------
# Core libraries
set(QB_CORE_LIBRARIES)
set(QB_IO_LIBRARIES)
set(QB_ALL_LIBRARIES)

# Module libraries
set(QB_MODULE_LIBRARIES)

# External dependencies
set(QB_EXTERNAL_LIBRARIES)

# -----------------------------------------------------------------------------
# Output Directories
# -----------------------------------------------------------------------------
# Tidy bin/lib layout, but only when a parent project hasn't already chosen one.
# This keeps qb polite when embedded via add_subdirectory (it won't hijack the
# parent's output tree) while still giving a clean layout for standalone builds.
set(QB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(QB_LIBRARY_DIR "${CMAKE_BINARY_DIR}/lib")
set(QB_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/lib")

if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${QB_OUTPUT_DIR}")
endif()
if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${QB_LIBRARY_DIR}")
endif()
if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${QB_ARCHIVE_DIR}")
endif()

# Per-configuration output directories (multi-config generators)
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config_upper)
    if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper})
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${QB_OUTPUT_DIR}")
    endif()
    if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper})
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${QB_LIBRARY_DIR}")
    endif()
    if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper})
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${QB_ARCHIVE_DIR}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Post-project Configuration
# -----------------------------------------------------------------------------
# Must run after project() has enabled CXX: GNUInstallDirs needs a known target
# architecture, CMAKE_SIZEOF_VOID_P is only reliable after language enablement,
# and compiler-specific color flags need CMAKE_CXX_COMPILER_ID.
macro(qb_initialize_project_configuration)
    include(GNUInstallDirs)

    # Platform detection
    set(QB_PLATFORM_WINDOWS FALSE)
    set(QB_PLATFORM_MACOS FALSE)
    set(QB_PLATFORM_LINUX FALSE)
    if(WIN32)
        set(QB_PLATFORM "Windows")
        set(QB_PLATFORM_WINDOWS TRUE)
    elseif(APPLE)
        set(QB_PLATFORM "macOS")
        set(QB_PLATFORM_MACOS TRUE)
    elseif(UNIX)
        set(QB_PLATFORM "Linux")
        set(QB_PLATFORM_LINUX TRUE)
    else()
        set(QB_PLATFORM "Unknown")
        message(WARNING "Unknown platform detected")
    endif()

    # Architecture detection
    set(QB_ARCH_64 FALSE)
    set(QB_ARCH_32 FALSE)
    set(QB_ARCH_ARM FALSE)
    set(QB_ARCH_ARM64 FALSE)
    set(QB_ARCH_ARM32 FALSE)
    set(_qb_system_processor "${CMAKE_SYSTEM_PROCESSOR}")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 _qb_system_processor)
    endif()

    if(_qb_system_processor MATCHES "^(arm64|aarch64|ARM64)$")
        set(QB_ARCH_ARM TRUE)
        set(QB_ARCH_ARM64 TRUE)
    elseif(_qb_system_processor MATCHES "^(arm|armv[0-9].*|aarch32|ARM)$")
        set(QB_ARCH_ARM TRUE)
        set(QB_ARCH_ARM32 TRUE)
    endif()

    if(QB_ARCH_ARM64)
        set(QB_ARCH "arm64")
        set(QB_ARCH_64 TRUE)
    elseif(QB_ARCH_ARM32)
        set(QB_ARCH "arm")
        set(QB_ARCH_32 TRUE)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(QB_ARCH "x64")
        set(QB_ARCH_64 TRUE)
    else()
        set(QB_ARCH "x86")
        set(QB_ARCH_32 TRUE)
    endif()

    # Publish platform/arch flags as CACHE INTERNAL so they are visible in EVERY
    # scope, including sibling qbm/* directories loaded by the top-level project.
    foreach(_v
        QB_PLATFORM QB_PLATFORM_WINDOWS QB_PLATFORM_MACOS QB_PLATFORM_LINUX
        QB_ARCH QB_ARCH_64 QB_ARCH_32 QB_ARCH_ARM QB_ARCH_ARM64 QB_ARCH_ARM32)
        set(${_v} "${${_v}}" CACHE INTERNAL "")
    endforeach()

    # Enable colored output if supported
    if(NOT WIN32)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            add_compile_options(-fcolor-diagnostics)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
            add_compile_options(-fdiagnostics-color=always)
        endif()
    endif()

    # Definitions and preprocessor macros.
    # Clear any stale CACHE INTERNAL shadow published by qbDependencies on a
    # prior configure BEFORE rebuilding: a bare `set(QB_COMPILE_DEFINITIONS)`
    # only unsets the normal variable, so `list(APPEND)` would read through to
    # the cached value and accumulate outdated entries across reconfigures
    # (e.g. a bumped QB_VERSION, producing a -Wmacro-redefined warning).
    unset(QB_COMPILE_DEFINITIONS CACHE)
    set(QB_COMPILE_DEFINITIONS "")
    list(APPEND QB_COMPILE_DEFINITIONS
        "QB_VERSION_MAJOR=${QB_FRAMEWORK_VERSION_MAJOR}"
        "QB_VERSION_MINOR=${QB_FRAMEWORK_VERSION_MINOR}"
        "QB_VERSION_PATCH=${QB_FRAMEWORK_VERSION_PATCH}"
        "QB_VERSION=\"${QB_FRAMEWORK_VERSION}\""
    )

    if(QB_PLATFORM_WINDOWS)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_PLATFORM_WINDOWS=1")
    elseif(QB_PLATFORM_MACOS)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_PLATFORM_MACOS=1")
    elseif(QB_PLATFORM_LINUX)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_PLATFORM_LINUX=1")
    endif()

    if(QB_ARCH_64)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_ARCH_64=1")
    else()
        list(APPEND QB_COMPILE_DEFINITIONS "QB_ARCH_32=1")
    endif()

    if(QB_ARCH_ARM)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_ARCH_ARM=1")
        if(QB_ARCH_ARM64)
            list(APPEND QB_COMPILE_DEFINITIONS "QB_ARCH_ARM64=1")
        endif()
    endif()

    if(QB_WITH_LOGGING)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_WITH_LOGGING=1")
    endif()
    if(QB_WITH_SSL)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_WITH_SSL=1")
    endif()
    if(QB_WITH_COMPRESSION)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_WITH_COMPRESSION=1")
    endif()
    if(QB_DEBUG_MEMORY)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_DEBUG_MEMORY=1")
    endif()
    if(QB_DEBUG_ACTOR)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_DEBUG_ACTOR=1")
    endif()
    if(QB_STDOUT_LOGGING)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_STDOUT_LOGGING=1")
    endif()
endmacro()

# -----------------------------------------------------------------------------
# Internal State Variables
# -----------------------------------------------------------------------------
set(QB_LIBRARIES_CREATED FALSE CACHE INTERNAL "Track if qb libraries have been created")
set(QB_MODULES_LOADED FALSE CACHE INTERNAL "Track if qb modules have been loaded")

# -----------------------------------------------------------------------------
# Utility Functions
# -----------------------------------------------------------------------------
function(qb_status_message)
    message(STATUS "[qb] ${ARGN}")
endfunction()

function(qb_debug_message)
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
        message(STATUS "[qb-debug] ${ARGN}")
    endif()
endfunction()

function(qb_warning_message)
    message(WARNING "[qb] ${ARGN}")
endfunction()

function(qb_error_message)
    message(FATAL_ERROR "[qb] ${ARGN}")
endfunction()

# -----------------------------------------------------------------------------
# Configuration Summary
# -----------------------------------------------------------------------------
function(qb_print_configuration)
    qb_status_message("========================================")
    qb_status_message("qb Framework Configuration")
    qb_status_message("========================================")
    qb_status_message("Version: ${QB_FRAMEWORK_VERSION}")
    qb_status_message("Platform: ${QB_PLATFORM} (${QB_ARCH})")
    qb_status_message("Build Type: ${CMAKE_BUILD_TYPE}")
    qb_status_message("Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    qb_status_message("Root Directory: ${QB_ROOT_DIR}")
    qb_status_message("Features:")
    qb_status_message("  - Tests: ${QB_BUILD_TESTS}")
    qb_status_message("  - Examples: ${QB_BUILD_EXAMPLES}")
    qb_status_message("  - Benchmarks: ${QB_BUILD_BENCHMARKS}")
    qb_status_message("  - Logging: ${QB_WITH_LOGGING}")
    qb_status_message("  - SSL: ${QB_WITH_SSL}")
    qb_status_message("  - Compression: ${QB_WITH_COMPRESSION}")
    qb_status_message("  - Optimizations: ${QB_ENABLE_OPTIMIZATIONS}")
    qb_status_message("========================================")
endfunction()

# Mark configuration as loaded
set(QB_CONFIG_LOADED TRUE CACHE INTERNAL "qb configuration loaded")
