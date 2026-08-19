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
set(QB_FRAMEWORK_VERSION "3.0.0")
set(QB_FRAMEWORK_VERSION_MAJOR 3)
set(QB_FRAMEWORK_VERSION_MINOR 0)
set(QB_FRAMEWORK_VERSION_PATCH 0)

# Framework paths
set(QB_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "qb framework root directory")
get_filename_component(QB_ROOT_DIR "${QB_ROOT_DIR}" ABSOLUTE)
set(QB_INCLUDE_DIR "${QB_ROOT_DIR}/src")
# CACHE INTERNAL so that QB_CMAKE_DIR is globally visible across all
# add_subdirectory() scopes (e.g. qbm modules added after the qb subtree).
# Without CACHE, the variable stays local to qb's subdirectory scope and
# qb_add_test / qb_add_benchmark cannot find deploy_runtime_dlls.cmake.
set(QB_CMAKE_DIR  "${QB_ROOT_DIR}/cmake"   CACHE INTERNAL "qb cmake scripts directory")
# src/ IS qb's include root -- the ONLY one, since 3.0 stopped vendoring nlohmann and deleted
# modules/ with it. What a consumer types after `#include <` is exactly what lives under src/, each
# header's implementation beside it. The qb FORKS -- qev, uuid, nanolog, ska_hash -- live under
# src/qb/vendor/ so that an installed qb owns every top-level name it drops in the consumer's
# include root; that root is now exactly `qb`. nlohmann was the one genuine third-party upstream
# and is now resolved by find_package / FetchContent instead (see QB_USE_SYSTEM_NLOHMANN below).
# qb-owned forks. Physically inside QB_INCLUDE_DIR on purpose: the build tree and the installed
# tree then expose them at the SAME relative path (qb/vendor/<fork>/...) through the SAME single
# include root, so there is no build-interface/install-interface pair to drift apart.
set(QB_VENDOR_DIR "${QB_INCLUDE_DIR}/qb/vendor")
set(QB_SOURCE_DIR "${QB_INCLUDE_DIR}/qb")   # by the rule above: implementation beside its header

# The event loop is NOT under vendor/, and that is a statement rather than a filing preference.
# `vendor/` means "a copy we re-pull from upstream": upstream libev is unmaintained at 4.33/4.35,
# every one of this fork's 58 exported symbols is renamed, all thirteen files are renamed, and it
# carries qb's own fixes (Windows keep-alive, the wepoll and kqueue backends, the header
# namespacing). Telling the next maintainer "do not touch, upstream owns this" would be false and
# it is the engine of qb-io. It is published standalone as isndev/qev, and this tree is the source
# of truth for that repo -- `dev/agent/check-qev-identity.py` in the superproject asserts the two
# are byte-identical. The BSD-2 attribution is untouched by the move: it travels with LICENSE,
# THIRD-PARTY-NOTICES and the per-file copyright headers, and qb/scripts/check-vendor-attribution.py
# still covers it at the new path.
set(QB_EV_SRC_DIR "${QB_SOURCE_DIR}/ev")

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
set(QB_NLOHMANN_GIT_TAG "v3.12.0" CACHE STRING "Git tag (or SHA) for the FetchContent nlohmann_json fallback")
mark_as_advanced(QB_GOOGLETEST_GIT_TAG QB_GOOGLEBENCHMARK_GIT_TAG QB_ZLIB_GIT_TAG
                 QB_NLOHMANN_GIT_TAG)
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
# OFF by default: `-march=native` bakes the BUILD machine's instruction set into the artefact, so a
# binary built on a newer CPU dies with SIGILL on an older one. That is the right default for a
# library other people consume and ship; turn it ON explicitly (or use the `release-native` /
# `benchmarks` presets) to tune codegen for the build host. There is no configuration in which ON
# is the default -- `base` is OFF in both preset files, and a bare `cmake -D...` with no preset
# reads this line. (A second comment block here used to open "ON by default", left over from when
# it was; seven doc files copied that claim. The option has been OFF, and the two blocks
# contradicted each other in the same six lines.)
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
# nlohmann/json source selection. Tri-state, same shape as QB_WITH_QUIC:
#   AUTO (default) -- use the system nlohmann_json if find_package finds one; otherwise FetchContent
#                     it from the pinned tag ${QB_NLOHMANN_GIT_TAG}, subject to
#                     QB_DEPS_FETCH_FALLBACK (the same system-first/git-fallback policy zlib,
#                     GoogleTest and Google Benchmark already follow).
#   ON             -- REQUIRE the system one; hard-fail at configure time if it is absent.
#   OFF            -- never probe the system; always FetchContent the pinned tag.
# 3.0 STOPPED VENDORING nlohmann. qb used to carry modules/nlohmann/json.hpp as the fallback, and
# that copy was an untagged post-3.12.0 develop snapshot (450 added / 264 removed lines against the
# v3.12.0 tag) which nonetheless declared NLOHMANN_JSON_VERSION_* = 3/12/0. nlohmann guards against
# version mixing with an inline namespace whose name encodes the version (nlohmann::json_abi_v3_12_0),
# so a program linking that copy together with a genuine 3.12.0 got ONE inline namespace spanning two
# different definition sets -- an ODR violation no linker diagnoses, because the label lied. Deleting
# it removes the lie; a FetchContent'd copy is the real tag, and the namespace tag means what it says.
# Dropping it also removes `nlohmann/` from qb's installed include root (now exactly `qb`), a name qb
# has no business claiming in a consumer's include namespace and the reason `brew link` collided with
# a distro's own nlohmann-json package.
# See the long note in qbDependencies.cmake for why nlohmann (unlike the qev/uuid/nanolog/ska_hash
# forks) could never simply be re-prefixed out of the way.
set(QB_USE_SYSTEM_NLOHMANN "AUTO" CACHE STRING "nlohmann/json source: AUTO (system if found, else fetch), ON (require system), OFF (always fetch)")
set_property(CACHE QB_USE_SYSTEM_NLOHMANN PROPERTY STRINGS AUTO ON OFF)
if(NOT QB_USE_SYSTEM_NLOHMANN MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "[qb] QB_USE_SYSTEM_NLOHMANN must be AUTO, ON or OFF (got '${QB_USE_SYSTEM_NLOHMANN}')")
endif()
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
# parent, and on a multi-config generator (Visual Studio, Ninja Multi-Config, Xcode)
# CMAKE_BUILD_TYPE is empty BY DESIGN -- writing it there is meaningless at best and misleading at
# worst, since the per-config choice is made at build time.
#
# The multi-config test is the GLOBAL PROPERTY, not CMAKE_CONFIGURATION_TYPES. This file is
# include()d from qb/CMakeLists.txt BEFORE project() (line 47 vs 50), and the generator does not
# populate CMAKE_CONFIGURATION_TYPES until project() runs -- so `NOT CMAKE_CONFIGURATION_TYPES`
# was ALWAYS true here and guarded nothing. `cmake -S qb -G "Ninja Multi-Config"` therefore wrote
# CMAKE_BUILD_TYPE=Release into a cache that also had CMAKE_CONFIGURATION_TYPES=Debug;Release;...,
# exactly the state the paragraph above says must never happen, with no diagnostic.
# GENERATOR_IS_MULTI_CONFIG *is* set pre-project() (verified: 1 for Ninja Multi-Config and Xcode,
# 0 for Ninja), because it is a property of the generator the driver already selected.
get_property(_qb_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_BUILD_TYPE AND NOT _qb_multi_config)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Define available build types -- for the cmake-gui / ccmake drop-down only.
# Guarded on the cache entry EXISTING: set_property(CACHE ...) is a hard error when it does not,
# and on a multi-config generator nothing above creates it. That killed every multi-config
# configure of the SUPERPROJECT outright ("set_property could not find CACHE variable
# CMAKE_BUILD_TYPE"), because there the standalone branch above does not fire either -- so the
# same missing-guard bug was silent in one tree and fatal in the other.
if(DEFINED CACHE{CMAKE_BUILD_TYPE})
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

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

# Position-independent code. Required so the bundled static dependencies (qev, llhttp) can be linked
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

# Per-configuration output directories (multi-config generators).
# Same pre-project() problem as the build-type block above: CMAKE_CONFIGURATION_TYPES is empty
# here unless the user passed it on the command line, so this loop ran zero times on every
# multi-config generator -- a multi-config cache carried 0 CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG>
# entries. Fall back to the same four names the STRINGS property offers when the generator is
# multi-config but has not told us its list yet.
set(_qb_config_types ${CMAKE_CONFIGURATION_TYPES})
if(_qb_multi_config AND NOT _qb_config_types)
    set(_qb_config_types Debug Release RelWithDebInfo MinSizeRel)
endif()
foreach(config ${_qb_config_types})
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
unset(_qb_config_types)

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

# qb_feature_degraded(<what>) - report an optional feature that was REQUESTED but could not be
# enabled because its dependency was missing.
#
# Default (QB_REQUIRE_FEATURES=OFF) is the historical behaviour: warn, set QB_HAS_<x>=FALSE, and
# carry on. That is right for a developer build and wrong for a distribution build -- a hermetic
# packaging environment (vcpkg, buildd, a brew sandbox with no network) that cannot see OpenSSL or
# zlib produced a QUIETLY REDUCED package at exit 0, and the packager's only way to notice was to
# inspect the artefact afterwards. QB_REQUIRE_FEATURES=ON turns every such downgrade into a
# configure-time failure, which is what a package recipe wants.
#
# Only reachable when the feature was asked for: an AUTO/OFF resolution is not a degradation and
# does not come through here.
option(QB_REQUIRE_FEATURES "Fail configure when a requested optional feature cannot be enabled (for distribution builds)" OFF)
function(qb_feature_degraded)
    if(QB_REQUIRE_FEATURES)
        message(FATAL_ERROR
            "[qb] ${ARGN}\n"
            "     QB_REQUIRE_FEATURES=ON, so this downgrade is an error rather than a warning.\n"
            "     Install the missing dependency, or turn the feature off explicitly "
            "(e.g. -DQB_WITH_COMPRESSION=OFF) to accept a reduced build.")
    else()
        message(WARNING "[qb] ${ARGN}")
    endif()
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
    # Say so when the switch cannot do anything here. The qb repository ships no examples/ tree --
    # the examples are a SEPARATE submodule owned by the qb-dev superproject, and only its
    # examples/CMakeLists.txt reads this option. A standalone `cmake -S qb -DQB_BUILD_EXAMPLES=ON`
    # therefore builds nothing extra (measured: 0 executable targets, byte-identical to OFF) while
    # this line printed a bare "ON" and looked like it had.
    if(EXISTS "${QB_ROOT_DIR}/examples/CMakeLists.txt")
        qb_status_message("  - Examples: ${QB_BUILD_EXAMPLES}")
    else()
        qb_status_message("  - Examples: ${QB_BUILD_EXAMPLES} (not built by qb itself - this repository ships no examples/ tree; the switch is read by the qb-dev superproject's examples/ submodule)")
    endif()
    qb_status_message("  - Benchmarks: ${QB_BUILD_BENCHMARKS}")
    qb_status_message("  - Logging: ${QB_WITH_LOGGING}")
    qb_status_message("  - SSL: ${QB_WITH_SSL}")
    qb_status_message("  - Compression: ${QB_WITH_COMPRESSION}")
    qb_status_message("  - Optimizations: ${QB_ENABLE_OPTIMIZATIONS}")
    qb_status_message("========================================")
endfunction()

# Mark configuration as loaded
set(QB_CONFIG_LOADED TRUE CACHE INTERNAL "qb configuration loaded")

# Appended at the END of this file ON PURPOSE, not next to QB_FRAMEWORK_VERSION where it
# reads more naturally. qb's readme cites this file by line ~30 times, and llm-guard /
# cite-check treat those citations as a guarded contract. Inserting 26 lines at the top
# shifted every one of them -- measured: three content-aware citations went red immediately
# and the rest (bare backticks no guard reads) would have rotted silently. Adding at the end
# moves nothing. Keep new top-level settings here for the same reason.
# -----------------------------------------------------------------------------
# Public-type ABI tokens
# -----------------------------------------------------------------------------
# A tripwire for one specific defect class: a PUBLIC TYPE whose identity -- and therefore
# layout -- is chosen by a build macro rather than by qb's source.
#
# Until 3.0.0 `qb::unordered_map` / `qb::unordered_set` aliased ska:: under NDEBUG and std::
# otherwise. Both are data members of public classes (qb::VirtualCore, qb::Main,
# qb::router::*) and of qbm's public headers, and the two spellings have different layouts
# (sizeof(qb::unordered_map<int,int>) measured 32 with NDEBUG, 40 without). A consumer
# compiled without NDEBUG -- CMAKE_BUILD_TYPE=Debug, or simply UNSET, which is the default --
# against a Release-built libqb read a ska map through std layout and aborted at run time
# (`std::overflow_error: __next_prime overflow`). Nothing at configure time said a word:
# qbmModuleConfig.cmake.in gated version skew and QB_HAS_* skew, and was silent on the one
# macro that changed a public type.
#
# The alias is now unconditional (see the @warning in qb/system/container/unordered_map.h).
# This token records WHICH implementation that is. It is compared between the qb a prebuilt
# qbm module was compiled against and the qb a consumer resolves; change the implementation
# and the token must change with it, so every module built against the old one fails loudly
# at find_package() instead of at run time.
#
# It is deliberately a CONSTANT, not something derived from CMAKE_BUILD_TYPE: the whole point
# of the fix is that this value no longer depends on how anyone is building.
#
# CACHE INTERNAL for the same reason QB_CMAKE_DIR is (see below): qbm modules are added from
# the SUPERPROJECT root scope (`qb_load_modules()` at qb-dev/CMakeLists.txt:56), not from qb's,
# so a plain set() here is invisible where qbmModuleConfig.cmake.in is configured -- measured:
# the generated qbm-httpConfig.cmake came out with `set(QBM_HTTP_QB_ABI_UNORDERED_MAP "")`.
# An empty token would have made the skew gate below fire on every module, which is how this
# was caught: the positive control is not optional.
set(QB_ABI_UNORDERED_MAP "ska" CACHE INTERNAL "qb::unordered_map/set implementation (public-type ABI token)")
