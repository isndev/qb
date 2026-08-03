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

# vcpkg's automatic per-target applocal DLL copy is OFF: this project already deploys runtime DLLs
# itself (deploy_runtime_dlls.cmake, invoked from qb_add_test / qb_add_benchmark). With both active,
# two mechanisms copy the SAME DLLs into the SAME directory from targets that link in parallel, and
# one holds a file the other is writing -- MSBuild then fails the build with
# "The process cannot access the file because it is being used by another process" followed by
# MSB3073 on `vcpkg z-applocal`. It is a build-infrastructure race, not a compile error, and it
# aborted the whole Windows job before a single test ran. Setting this before project() so the
# vcpkg toolchain sees it.
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "qb deploys runtime DLLs itself; vcpkg's per-target applocal races with it" FORCE)

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

if (QB_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif ()

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
# ON, not OFF: qb requires C++20 at minimum, so std::span always exists. With OFF, stduuid takes its
# gsl fallback branch -- which points BUILD_INTERFACE and INSTALL_INTERFACE at a `gsl/` directory
# that was deleted when qb moved to C++20, and runs `install(DIRECTORY gsl ...)` on it. That made
# `cmake --install` FAIL outright ("modules/uuid/cmake_install.cmake:41"), breaking the documented
# find_package(qb) integration mode. Nothing noticed because no CI job ever ran cmake --install.
set(UUID_USING_CXX20_SPAN ON)

# Tell the vendored forks they are EMBEDDED in qb and must publish their headers under the
# qb/vendor/<fork>/ prefix instead of dropping bare names (qev.h, uuid.h, event.h ...) into the
# consumer's include root. Both projects keep working standalone when this is unset.
set(QB_VENDOR_INCLUDE_PREFIX "qb/vendor")

# Load qb's own vendored forks (qev, uuid). Header-only forks (nanolog, ska_hash) need no build.
add_subdirectory("${QB_VENDOR_DIR}/uuid")
add_subdirectory("${QB_VENDOR_DIR}/qev")

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
if (TARGET qb-io)
    add_library(qb::io ALIAS qb-io)
    list(APPEND QB_CORE_LIBRARIES qb-io)
    set(QB_IO_FOUND TRUE)
endif ()

if (TARGET qb-core)
    add_library(qb::core ALIAS qb-core)
    list(APPEND QB_CORE_LIBRARIES qb-core)
    set(QB_CORE_FOUND TRUE)
endif ()

if (TARGET qev)
    set(QB_EV_FOUND TRUE)
endif ()

# Update global library lists
set(QB_ALL_LIBRARIES ${QB_CORE_LIBRARIES} ${QB_MODULE_LIBRARIES})

# -----------------------------------------------------------------------------
# Test resources (SSL certificates, etc.)
# -----------------------------------------------------------------------------
if (QB_BUILD_TESTS)
    qb_setup_test_resources()
endif ()

# -----------------------------------------------------------------------------
# Code Coverage Setup
# -----------------------------------------------------------------------------
if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32 AND QB_BUILD_COVERAGE)
    include("${QB_CMAKE_DIR}/CodeCoverage.cmake")

    find_program(LCOV_PATH lcov)
    if (NOT GCOV_PATH)
        find_program(GCOV_PATH gcov)
    endif ()

    if (LCOV_PATH AND GCOV_PATH)
        qb_status_message("Code coverage enabled")

        # '*vendor*' alongside '*modules*': the forks (qev, uuid, nanolog, ska_hash) moved under
        # include/qb/vendor/. Without it they would start counting towards qb's coverage figure --
        # libev's ev.c alone is several thousand lines of third-party-derived C.
        set(COVERAGE_LCOV_EXCLUDES
                '/usr/*'
                '*benchmark*'
                '*modules*'
                '*/include/qb/vendor/*'
                '*examples/*'
                '*test*'
        )

        set(COVERAGE_GCOVR_EXCLUDES
                '/usr/.*'
                '${PROJECT_BINARY_DIR}/_deps/.*'
                '${PROJECT_SOURCE_DIR}/modules/.*'
                '${PROJECT_SOURCE_DIR}/include/qb/vendor/.*'
                '${PROJECT_SOURCE_DIR}/examples/.*'
                '${PROJECT_SOURCE_DIR}/source/.*/tests/benchmark/.*'
                '${PROJECT_SOURCE_DIR}/source/.*/tests/.*'
                '.*benchmark.*'
        )

        set(COVERAGE_GCOVR_ARGS
                --gcov-ignore-parse-errors=all
                --gcov-ignore-errors=source_not_found
        )

        set(QB_COVERAGE_DEPENDENCIES qb-core)
        if (QB_BUILD_TESTS)
            get_property(QB_TEST_TARGETS GLOBAL PROPERTY QB_TEST_TARGETS)
            if (QB_TEST_TARGETS)
                list(APPEND QB_COVERAGE_DEPENDENCIES ${QB_TEST_TARGETS})
                list(REMOVE_DUPLICATES QB_COVERAGE_DEPENDENCIES)
            endif ()
        endif ()

        SETUP_TARGET_FOR_COVERAGE_LCOV(
                NAME qb-coverage
                EXECUTABLE ctest
                DEPENDENCIES ${QB_COVERAGE_DEPENDENCIES}
                LCOV_ARGS
                --ignore-errors mismatch,mismatch
                --ignore-errors unused,unused
                --ignore-errors inconsistent,inconsistent
        )

        SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(
                NAME qb-coverage-xml
                EXECUTABLE ctest
                DEPENDENCIES ${QB_COVERAGE_DEPENDENCIES}
                GCOVR_ARGS ${COVERAGE_GCOVR_ARGS}
        )

        SETUP_TARGET_FOR_COVERAGE_GCOVR_HTML(
                NAME qb-coverage-html
                EXECUTABLE ctest
                DEPENDENCIES ${QB_COVERAGE_DEPENDENCIES}
                GCOVR_ARGS ${COVERAGE_GCOVR_ARGS}
        )
    else ()
        qb_warning_message("Coverage requested but tools not found (lcov, gcov)")
    endif ()
endif ()

# -----------------------------------------------------------------------------
# Documentation
# -----------------------------------------------------------------------------
if (QB_BUILD_DOCS)
    add_subdirectory("${QB_ROOT_DIR}/docs")
endif ()

# -----------------------------------------------------------------------------
# Installation Configuration
# -----------------------------------------------------------------------------
if (QB_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # Collect the library targets that should be installed.
    # Bundled dependencies (qev, stduuid) MUST be in the same export set so
    # CMake rewrites their names with the qb:: namespace in the transitive
    # dependency list of qb::io / qb::core.
    set(_QB_INSTALL_TARGETS)
    if (TARGET qb-io)
        list(APPEND _QB_INSTALL_TARGETS qb-io)
    endif ()
    if (TARGET qb-core)
        list(APPEND _QB_INSTALL_TARGETS qb-core)
    endif ()
    if (TARGET qev)
        list(APPEND _QB_INSTALL_TARGETS qev)
    endif ()
    if (TARGET stduuid)
        list(APPEND _QB_INSTALL_TARGETS stduuid)
    endif ()
    # qb::io links this PUBLIC, so qbTargets.cmake names it in qb::io's INTERFACE_LINK_LIBRARIES.
    # Leaving it out of the export set makes find_package(qb) fail on an unknown target.
    if (TARGET qb-nlohmann)
        list(APPEND _QB_INSTALL_TARGETS qb-nlohmann)
    endif ()

    # Install library targets with an EXPORT set so CMake can generate
    # the *Targets.cmake files required by find_package(qb).
    if (_QB_INSTALL_TARGETS)
        install(TARGETS ${_QB_INSTALL_TARGETS}
                EXPORT qbTargets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif ()

    # Install the export set
    install(EXPORT qbTargets
            FILE qbTargets.cmake
            NAMESPACE qb::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
    )

    # Install public headers.
    #
    # This ONE rule now also ships the vendored forks (include/qb/vendor/{qev,uuid,nanolog,
    # ska_hash}), because they physically live under this directory. That is the point of the
    # layout: the build tree and the installed tree expose qb's headers AND its forks through the
    # same single root at the same relative paths, so there is no second BUILD_INTERFACE /
    # INSTALL_INTERFACE pair to drift apart. The previous layout had exactly that drift and it
    # shipped broken: find_package(qb) configured fine and then failed to compile on
    # "fatal error: 'ev/ev++.h' file not found", because the build-interface root
    # ($<BUILD_INTERFACE:${QB_MODULES_DIR}>) had no installed counterpart and no CI job ever ran
    # cmake --install.
    #
    # Excluded: stduuid's vendored Catch2 (~830 KB) and its test sources. They are build-time
    # material for the fork's own test suite, never part of qb's public surface.
    install(
            DIRECTORY "${QB_INCLUDE_DIR}/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING
            REGEX "/qb/vendor/uuid/(catch|test|cmake)(/|$)" EXCLUDE
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.tpp"
    )

    # Genuine third-party upstream dependencies still living in modules/ (nlohmann only). Installed
    # at their canonical path -- <nlohmann/json.hpp> IS that library's public spelling, and unlike
    # the forks it is a name a consumer may legitimately already own.
    #
    # Only when qb actually fell back to the bundled copy. If qb was built against a system
    # nlohmann_json, installing ours too would put a SECOND copy on the consumer's include path
    # while qbConfig.cmake resolves the real one via find_dependency -- the exact duplicate-type
    # situation the find_package branch exists to avoid.
    set(_qb_bundled_hdr_mods ${QB_HEADER_ONLY_MODULES})
    if (QB_USES_SYSTEM_NLOHMANN)
        set(_qb_bundled_hdr_mods)
    endif ()
    foreach (_hdr_mod ${_qb_bundled_hdr_mods})
        set(_hdr_mod_dir "${QB_MODULES_DIR}/${_hdr_mod}")
        if (IS_DIRECTORY "${_hdr_mod_dir}")
            install(
                    DIRECTORY "${_hdr_mod_dir}/"
                    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${_hdr_mod}
                    FILES_MATCHING
                    PATTERN "*.h"
                    PATTERN "*.hpp"
            )
        endif ()
    endforeach ()

    # Install qb's own license and the third-party notices.
    #
    # This is not decoration: Apache-2.0 s.4 requires a copy of the License to reach every
    # recipient, and the MIT / BSD-2 / BSL-1.0 components qb bundles all require their notice
    # to be *distributed with* the software. Until this rule existed nothing did -- the two
    # install(DIRECTORY) rules above use FILES_MATCHING with *.h/*.hpp/*.tpp patterns, and
    # FILES_MATCHING installs ONLY files matching a pattern, so LICENSE and THIRD-PARTY-NOTICES
    # sitting right next to the vendored headers were silently skipped. A verified install
    # produced zero license files of any kind while shipping five vendored upstreams.
    #
    # share/licenses/<pkg>/ rather than share/doc: it is the layout distro packaging tools
    # already look in, and it keeps qb's notices from colliding with a qbm module's.
    set(_qb_licensedir "${CMAKE_INSTALL_DATAROOTDIR}/licenses/qb")
    install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES"
            DESTINATION "${_qb_licensedir}"
    )

    # Each vendored unit keeps its license text beside its own code, so glob rather than
    # hard-code: a unit added later ships its notice without anyone remembering to edit this
    # list, and scripts/check-vendor-attribution.py fails the guard battery if a unit has no
    # notice to ship. The installed path mirrors THIRD-PARTY-NOTICES' "License at" entries.
    #
    # The glob is deliberately one level deep, which is also what keeps uuid/catch out: Catch2
    # is the stduuid fork's test-only material, the header rule above already excludes it, and
    # shipping its license would advertise a dependency the consumer never received.
    file(GLOB _qb_vendor_dirs LIST_DIRECTORIES true "${QB_INCLUDE_DIR}/qb/vendor/*")
    foreach (_qb_vendor_dir ${_qb_vendor_dirs})
        if (NOT IS_DIRECTORY "${_qb_vendor_dir}")
            continue()
        endif ()
        get_filename_component(_qb_vendor_name "${_qb_vendor_dir}" NAME)
        file(GLOB _qb_vendor_notices
                "${_qb_vendor_dir}/LICENSE" "${_qb_vendor_dir}/LICENSE.*"
                "${_qb_vendor_dir}/LICENSE_*" "${_qb_vendor_dir}/THIRD-PARTY-NOTICES")
        if (_qb_vendor_notices)
            install(FILES ${_qb_vendor_notices}
                    DESTINATION "${_qb_licensedir}/third-party/${_qb_vendor_name}")
        endif ()
    endforeach ()

    # nlohmann's notice follows its header: installed only when qb actually shipped the bundled
    # copy. If qb resolved a system nlohmann_json, neither the header nor its license is ours to
    # install -- and when the bundled copy is eventually deleted in favour of
    # find_package(nlohmann_json), this rule stops firing on its own rather than leaving a
    # dangling license for a header that is no longer there.
    foreach (_hdr_mod ${_qb_bundled_hdr_mods})
        file(GLOB _qb_mod_notices "${QB_MODULES_DIR}/${_hdr_mod}/LICENSE*")
        if (_qb_mod_notices)
            install(FILES ${_qb_mod_notices}
                    DESTINATION "${_qb_licensedir}/third-party/${_hdr_mod}")
        endif ()
    endforeach ()

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
    if (QB_HAS_ARGON2)
        install(FILES "${QB_CMAKE_DIR}/FindArgon2.cmake"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
        )
    endif ()

    # Install FindNgtcp2 so find_package(qb) consumers of a QUIC-enabled build can
    # recreate the Ngtcp2:: imported targets that qb::io links transitively.
    if (QB_HAS_QUIC)
        install(FILES "${QB_CMAKE_DIR}/FindNgtcp2.cmake"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
        )
    endif ()

    qb_status_message("Installation configured:")
    qb_status_message("  Prefix: ${CMAKE_INSTALL_PREFIX}")
    qb_status_message("  Libraries: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
    qb_status_message("  Headers: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
    qb_status_message("  CMake: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/qb")
endif ()

# -----------------------------------------------------------------------------
# Export Framework Configuration
# -----------------------------------------------------------------------------
# Mark framework as successfully loaded
set(QB_FOUND TRUE CACHE BOOL "qb framework found and configured")
set(QB_LIBRARIES ${QB_ALL_LIBRARIES} CACHE STRING "All qb libraries")
# Two entries in the BUILD tree only: QB_INCLUDE_DIR (qb's headers + its vendored forks under
# qb/vendor/) and QB_MODULES_DIR (the bundled nlohmann fallback, which is not copied into the
# source tree). An INSTALLED qb has a single root -- see qbConfig.cmake.in. Legacy variable;
# prefer linking qb::core / qb::io and inheriting the include paths from the target.
set(QB_INCLUDE_DIRS "${QB_INCLUDE_DIR};${QB_MODULES_DIR}" CACHE STRING "qb include directories")

# Set target variables for compatibility
set(QB_CORE_TARGET qb::core)
set(QB_IO_TARGET qb::io)
set(QB_EV_TARGET qev)

# -----------------------------------------------------------------------------
# Parent Scope Export (for add_subdirectory usage)
# -----------------------------------------------------------------------------
get_directory_property(hasParent PARENT_DIRECTORY)
if (hasParent)
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

    # NOT exported: OPENSSL_FOUND / ZLIB_FOUND are the standard result variables of
    # find_package(OpenSSL) / find_package(ZLIB). Writing them into the parent scope overwrote a
    # consumer's own find_package results with qb's internal feature flags -- a project that had
    # already probed for OpenSSL saw its answer replaced. qb's own state is published under the
    # QB_ prefix, which cannot collide.
    set(QB_HAS_SSL_FOUND ${QB_HAS_SSL} PARENT_SCOPE)
    set(QB_HAS_COMPRESSION_FOUND ${QB_HAS_COMPRESSION} PARENT_SCOPE)
    set(QB_IO_WITH_SSL ${QB_HAS_SSL} PARENT_SCOPE)

    qb_status_message("Framework variables exported to parent scope")
endif ()

# Print final summary
qb_print_dependencies()

qb_status_message("========================================")
qb_status_message("qb Framework Successfully Configured!")
qb_status_message("Available libraries: ${QB_ALL_LIBRARIES}")
qb_status_message("Available targets: qb::core, qb::io")
qb_status_message("========================================")
