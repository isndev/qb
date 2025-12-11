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
include(GNUInstallDirs)
include(CheckCXXCompilerFlag)

# -----------------------------------------------------------------------------
# Framework Information
# -----------------------------------------------------------------------------
set(QB_FRAMEWORK_NAME "qb")
set(QB_FRAMEWORK_DESCRIPTION "High-performance C++ Actor Framework")
set(QB_FRAMEWORK_VERSION "2.0.0")
set(QB_FRAMEWORK_VERSION_MAJOR 2)
set(QB_FRAMEWORK_VERSION_MINOR 0)
set(QB_FRAMEWORK_VERSION_PATCH 0)

# Framework paths
set(QB_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "qb framework root directory")
get_filename_component(QB_ROOT_DIR "${QB_ROOT_DIR}" ABSOLUTE)
set(QB_INCLUDE_DIR "${QB_ROOT_DIR}/include")
set(QB_CMAKE_DIR "${QB_ROOT_DIR}/cmake")
set(QB_MODULES_DIR "${QB_ROOT_DIR}/modules")
set(QB_SOURCE_DIR "${QB_ROOT_DIR}/source")

# -----------------------------------------------------------------------------
# Build Configuration Options
# -----------------------------------------------------------------------------
option(QB_BUILD_TESTS "Build qb tests" ON)
option(QB_BUILD_EXAMPLES "Build qb examples" ON)
option(QB_BUILD_BENCHMARKS "Build qb benchmarks" OFF)
option(QB_BUILD_DOCS "Build qb documentation" OFF)
option(QB_BUILD_SHARED_LIBS "Build shared libraries instead of static" OFF)
option(QB_INSTALL "Install qb framework" ON)

# Performance options
option(QB_ENABLE_OPTIMIZATIONS "Enable performance optimizations" ON)
option(QB_ENABLE_LTO "Enable Link Time Optimization" OFF)
option(QB_ENABLE_NATIVE_ARCH "Enable native architecture optimizations" OFF)

# Feature options
option(QB_WITH_LOGGING "Enable logging support" ON)
option(QB_WITH_SSL "Enable SSL/TLS support" ON)
option(QB_WITH_COMPRESSION "Enable compression support" ON)
option(QB_WITH_PROFILING "Enable profiling support" OFF)

# Debug options
option(QB_DEBUG_MEMORY "Enable memory debugging" OFF)
option(QB_DEBUG_ACTOR "Enable actor debugging" OFF)
option(QB_STDOUT_LOGGING "Enable stdout logging fallback" OFF)

# -----------------------------------------------------------------------------
# Platform Detection
# -----------------------------------------------------------------------------
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
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(QB_ARCH "x64")
    set(QB_ARCH_64 TRUE)
else()
    set(QB_ARCH "x86")
    set(QB_ARCH_32 TRUE)
endif()

# ARM detection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64|ARM64)")
    set(QB_ARCH_ARM TRUE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
        set(QB_ARCH_ARM64 TRUE)
    else()
        set(QB_ARCH_ARM32 TRUE)
    endif()
endif()

# -----------------------------------------------------------------------------
# Build Type Configuration
# -----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Define available build types
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

# -----------------------------------------------------------------------------
# Compiler Configuration
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable colored output if supported
if(NOT WIN32)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fcolor-diagnostics)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        add_compile_options(-fdiagnostics-color=always)
    endif()
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
set(QB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(QB_LIBRARY_DIR "${CMAKE_BINARY_DIR}/lib")
set(QB_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/lib")

# Set runtime output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${QB_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${QB_LIBRARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${QB_ARCHIVE_DIR}")

# Per-configuration output directories
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${QB_OUTPUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${QB_LIBRARY_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${QB_ARCHIVE_DIR}")
endforeach()

# -----------------------------------------------------------------------------
# Definitions and Preprocessor Macros
# -----------------------------------------------------------------------------
set(QB_COMPILE_DEFINITIONS)

# Framework version
list(APPEND QB_COMPILE_DEFINITIONS
    "QB_VERSION_MAJOR=${QB_FRAMEWORK_VERSION_MAJOR}"
    "QB_VERSION_MINOR=${QB_FRAMEWORK_VERSION_MINOR}"
    "QB_VERSION_PATCH=${QB_FRAMEWORK_VERSION_PATCH}"
    "QB_VERSION=\"${QB_FRAMEWORK_VERSION}\""
)

# Platform definitions
if(QB_PLATFORM_WINDOWS)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_PLATFORM_WINDOWS=1")
elseif(QB_PLATFORM_MACOS)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_PLATFORM_MACOS=1")
elseif(QB_PLATFORM_LINUX)
    list(APPEND QB_COMPILE_DEFINITIONS "QB_PLATFORM_LINUX=1")
endif()

# Architecture definitions
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

# Feature definitions
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