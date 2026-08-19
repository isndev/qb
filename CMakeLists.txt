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

# The event loop uses its own variable, not the vendor prefix, because it is not vendored and
# because ITS CMakeLists.txt is byte-shared with the standalone isndev/qev repo -- an embedder
# names the directory its include root exposes these headers under, and qb's answer is qb/ev.
set(QB_EV_EMBED_INCLUDE_DIR "qb/ev")

# Load qb's own vendored fork (uuid) and the event loop. Header-only forks (nanolog, ska_hash)
# need no build.
add_subdirectory("${QB_VENDOR_DIR}/uuid")
add_subdirectory("${QB_EV_SRC_DIR}")

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
# These targets exist ONLY when qb is the top-level project.
#
# They report qb alone -- COVERAGE_LCOV_EXCLUDES below drops '*modules*' -- and they
# zero and capture gcov counters with `-directory .` taken from qb's PROJECT_BINARY_DIR.
# A superproject that reaches this file through add_subdirectory() has its own aggregate
# family rooted one level up (qb-dev's CMakeLists.txt does), and that root is a STRICT
# PARENT of this one: `<build>/` contains `<build>/qb/`. Two families over one set of
# `.gcda` files with no ordering edge between them is not a smaller report, it is a
# WRONG one -- `--zerocounters` from the parent erases counters the child is producing,
# and neither target declares the counters as an output so no generator can order them.
#
# The subset here is redundant as well as unsafe: the superproject's `coverage` covers
# qb AND every qbm module, in the same three report kinds. So the top-level project owns
# the report; an embedded qb contributes instrumentation and nothing else.
if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32 AND QB_BUILD_COVERAGE AND NOT PROJECT_IS_TOP_LEVEL)
    qb_status_message(
            "Coverage instrumentation is on, but qb is not the top-level project: the qb-coverage* report targets belong to the top-level project (in qb-dev that is the aggregate `coverage` target, qb + qbm)")
elseif (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32 AND QB_BUILD_COVERAGE)
    include("${QB_CMAKE_DIR}/CodeCoverage.cmake")

    find_program(LCOV_PATH lcov)
    if (NOT GCOV_PATH)
        find_program(GCOV_PATH gcov)
    endif ()

    # These three targets read `.gcno`/`.gcda`, which only gcov-style instrumentation
    # emits. qbCompiler.cmake gives clang `-fprofile-instr-generate -fcoverage-mapping`
    # instead (QB_COVERAGE_KIND "llvm"), so on clang no `.gcno` is ever written and lcov
    # /gcovr have nothing to read -- they run the whole instrumented suite first and only
    # then produce an empty report. Measured on a fully instrumented clang tree: 0 `.gcno`,
    # 0 `.gcda`, 356 `.profraw`.
    #
    # A configure-time warning was the first answer and it is not enough. The warning scrolls
    # past in a build log; the target still builds the whole suite, runs all 356 tests, and
    # then writes an empty report that reads like a coverage collapse. So on a non-gcov
    # toolchain the four gcov targets are REPLACED, name for name, by ones that fail in under
    # a second and say which of the two real paths to take. Nothing is removed from a gcov
    # build -- qb/.github/workflows/coverage.yml configures with gcc-14, so QB_COVERAGE_KIND
    # is "gcov" there and the targets below are created exactly as before -- and nothing is
    # left undiscoverable: `--target qb-coverage` still resolves, it just answers instead of
    # running for ten minutes to say nothing.
    if (LCOV_PATH AND GCOV_PATH AND NOT QB_COVERAGE_KIND STREQUAL "gcov")
        qb_warning_message(
                "QB_COVERAGE_KIND is '${QB_COVERAGE_KIND}': the qb-coverage* targets are gcov-based and this build emits no .gcno/.gcda, so they would report nothing. They are replaced by a fail-fast stub. Use a GCC toolchain for them, or the qb-dev superproject's LLVM `coverage` target")
        foreach (_qb_cov_stub qb-coverage-run qb-coverage qb-coverage-xml qb-coverage-html)
            add_custom_target(${_qb_cov_stub}
                    COMMAND ${CMAKE_COMMAND} -E echo
                        "${_qb_cov_stub}: refusing to run -- this build's instrumentation is '${QB_COVERAGE_KIND}', not gcov."
                    COMMAND ${CMAKE_COMMAND} -E echo
                        "  ${CMAKE_CXX_COMPILER_ID} was given -fprofile-instr-generate -fcoverage-mapping, so it writes .profraw and NO .gcno/.gcda."
                    COMMAND ${CMAKE_COMMAND} -E echo
                        "  lcov and gcovr would run the entire test suite and then emit an EMPTY report."
                    COMMAND ${CMAKE_COMMAND} -E echo
                        "  Either: configure with a GCC toolchain (-DCMAKE_CXX_COMPILER=g++-14), which is what qb's coverage.yml does;"
                    COMMAND ${CMAKE_COMMAND} -E echo
                        "  or:     use the qb-dev superproject's LLVM-native aggregate target (cmake --preset coverage && cmake --build --preset coverage --target coverage)."
                    COMMAND ${CMAKE_COMMAND} -E false
                    COMMENT "gcov coverage target on a non-gcov toolchain"
                    VERBATIM)
            set_target_properties(${_qb_cov_stub} PROPERTIES FOLDER "Coverage")
        endforeach ()
    endif ()

    if (LCOV_PATH AND GCOV_PATH AND QB_COVERAGE_KIND STREQUAL "gcov")
        qb_status_message("Code coverage enabled")

        # '*vendor*' alongside '*modules*': the forks (qev, uuid, nanolog, ska_hash) moved under
        # src/qb/vendor/. Without it they would start counting towards qb's coverage figure --
        # libev's ev.c alone is several thousand lines of third-party-derived C.
        set(COVERAGE_LCOV_EXCLUDES
                '/usr/*'
                '*benchmark*'
                '*modules*'
                '*/src/qb/vendor/*'
                '*examples/*'
                '*test*'
        )

        set(COVERAGE_GCOVR_EXCLUDES
                '/usr/.*'
                '${PROJECT_BINARY_DIR}/_deps/.*'
                '${PROJECT_SOURCE_DIR}/modules/.*'
                '${PROJECT_SOURCE_DIR}/src/qb/vendor/.*'
                '${PROJECT_SOURCE_DIR}/examples/.*'
                '${PROJECT_SOURCE_DIR}/tests/.*/benchmark/.*'
                '${PROJECT_SOURCE_DIR}/tests/.*'
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

        # One writer, three readers (CodeCoverage.cmake, SETUP_COVERAGE_RUN banner).
        #
        # The three report targets used to zero the counters and run the whole suite
        # each, with no edge between them, so `cmake --build . --target qb-coverage
        # qb-coverage-xml` had qb-coverage's `--zerocounters` racing the run whose
        # counters qb-coverage-xml was about to read. coverage.yml only avoided it by
        # invoking two sequential steps -- a convention, enforced by nothing, and the
        # numbers are wrong quietly when it is broken.
        set(QB_COVERAGE_LCOV_ARGS
                --ignore-errors mismatch,mismatch
                --ignore-errors unused,unused
                --ignore-errors inconsistent,inconsistent
        )

        SETUP_COVERAGE_RUN(
                NAME qb-coverage-run
                EXECUTABLE ctest
                DEPENDENCIES ${QB_COVERAGE_DEPENDENCIES}
                LCOV_ARGS ${QB_COVERAGE_LCOV_ARGS}
        )

        SETUP_TARGET_FOR_COVERAGE_LCOV(
                NAME qb-coverage
                RUN_TARGET qb-coverage-run
                DEPENDENCIES ${QB_COVERAGE_DEPENDENCIES}
                LCOV_ARGS ${QB_COVERAGE_LCOV_ARGS}
        )

        SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(
                NAME qb-coverage-xml
                RUN_TARGET qb-coverage-run
                DEPENDENCIES ${QB_COVERAGE_DEPENDENCIES}
                GCOVR_ARGS ${COVERAGE_GCOVR_ARGS}
        )

        SETUP_TARGET_FOR_COVERAGE_GCOVR_HTML(
                NAME qb-coverage-html
                RUN_TARGET qb-coverage-run
                DEPENDENCIES ${QB_COVERAGE_DEPENDENCIES}
                GCOVR_ARGS ${COVERAGE_GCOVR_ARGS}
        )
    elseif (NOT LCOV_PATH OR NOT GCOV_PATH)
        # Narrowed from a bare `else()`: with the QB_COVERAGE_KIND condition added above, a
        # plain else would greet an llvm build that HAS lcov and gcov installed with "tools not
        # found", which is false and sends the reader looking for the wrong thing. The
        # non-gcov case is reported by the branch above, which also installs the stubs.
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

    # No third-party header tree is installed any more. nlohmann was the only one -- it sat outside
    # qb's include root and installed at its own canonical path, because <nlohmann/json.hpp> IS that
    # library's public spelling. 3.0 stopped vendoring it (see QB_USE_SYSTEM_NLOHMANN): an installed
    # qb now names it through find_dependency(nlohmann_json) instead of shipping a second copy into
    # a consumer's include root. The rule that used to do this is gone rather than disabled, which
    # is what makes the installed include root exactly `qb` on every host instead of host-dependent.
    #
    # CMake exports the Argon2:: / Ngtcp2:: ad-hoc IMPORTED targets that qb::io links
    # transitively BY NAME, with no recreation; a find_package(qb) consumer has no way to
    # invent them, so the Find modules qbConfig.cmake.in calls travel with the package.
    set(_qb_find_modules)
    if (QB_HAS_ARGON2)
        list(APPEND _qb_find_modules "${QB_CMAKE_DIR}/FindArgon2.cmake")
    endif ()
    if (QB_HAS_QUIC)
        list(APPEND _qb_find_modules "${QB_CMAKE_DIR}/FindNgtcp2.cmake")
    endif ()

    # THE install/export rule -- qb_install_package() in cmake/qbPackage.cmake, the same one
    # call every qbm module makes. Everything above this line is qb-only because it is about
    # a third-party choice or about which targets exist, not about how a qb-shaped repository
    # is packaged.
    #
    # HEADER_EXCLUDE carries two exclusions, both about material that is not qb's public surface:
    #   * stduuid's vendored Catch2 (~830 KB) and its test sources -- build-time material for
    #     that fork's own test suite.
    #   * the qev fork's libevent compatibility headers, unless QB_EV_LIBEVENT_COMPAT is ON. The
    #     bodies behind them are only compiled into libqev.a in that mode (see the option's
    #     comment: they are 24 unprefixed upstream libevent C symbols on every consumer's link
    #     line), and declarations that name the real libevent's API over an incompatible
    #     `struct event_base` do more harm installed than missing.
    # A module needs no exclusion at all.
    set(_qb_header_exclude "/qb/vendor/uuid/(catch|test|cmake)(/|$)")
    if (NOT QB_EV_LIBEVENT_COMPAT)
        set(_qb_header_exclude "${_qb_header_exclude}|/qb/ev/event(_compat)?\\.h$")
    endif ()

    qb_install_package(
            PACKAGE qb
            NAMESPACE qb::
            VERSION ${QB_FRAMEWORK_VERSION}
            COMPATIBILITY SameMajorVersion
            CONFIG_TEMPLATE "${QB_CMAKE_DIR}/qbConfig.cmake.in"
            SOURCE_DIR "${QB_ROOT_DIR}"
            HEADER_EXCLUDE "${_qb_header_exclude}"
            TARGETS ${_QB_INSTALL_TARGETS}
            CMAKE_FILES ${_qb_find_modules}
            VENDOR_DIRS "${QB_VENDOR_DIR}/*" "${QB_EV_SRC_DIR}"
    )

    # ---- share/qb/abi-fingerprint.txt --------------------------------------------------------
    # What a packager (brew, vcpkg, dpkg) has to be able to answer about a PREBUILT prefix:
    # "what was this compiled with?". Until 3.0 the only answer was
    # `strings lib/libqb-io.a | grep '^qb-abi '`, which needs the archive, the right grep, and
    # prior knowledge that the string exists -- and it covers only the five link-enforced ABI
    # axes, not the feature flags (SSL / compression / QUIC) that silently downgrade at configure
    # time when a dependency is missing in a hermetic build environment.
    #
    # The `qb-abi ...` line is READ BACK OUT OF THE BUILT ARCHIVE rather than re-derived from
    # CMake variables. That is the whole point: a re-derived copy is a second source of truth and
    # would drift from the artefact exactly the way every other hand-maintained version string in
    # this tree already has. If the archive does not contain the string, that is itself the
    # finding, and the file says so instead of quietly emitting a plausible-looking line.
    install(CODE "set(_qb_abi_archive [[$<TARGET_FILE:qb-io>]])")
    # BUILD_SHARED_LIBS is routinely UNDEFINED rather than OFF, and an empty `shared_libs=` reads
    # as "the field is broken" rather than "static". Normalise it, so every field on the line
    # always carries a value a packager can compare.
    if (BUILD_SHARED_LIBS)
        set(_qb_abi_shared ON)
    else ()
        set(_qb_abi_shared OFF)
    endif ()
    # `nlohmann=<version>` REPLACED `system_nlohmann=<bool>` in 3.0, because that boolean stopped
    # being able to vary: this file is written only by an install, and an installable qb is now
    # always built against a system nlohmann (qbDependencies.cmake refuses fetched-nlohmann +
    # QB_INSTALL). A field with one reachable value tells a packager nothing and implies a choice
    # that no longer exists. The VERSION does vary, and it is the actionable fact -- nlohmann
    # encodes it in an inline namespace (nlohmann::json_abi_v3_12_0), so a consumer resolving a
    # different one gets a link error, and this line is how a packager knows which to require.
    # No corresponding axis was added to the link-enforced set in src/qb/utility/abi.h, and none
    # should be: nlohmann's own inline namespace already turns a version mismatch into a link
    # failure, so a qb axis would duplicate a working mechanism. (It duplicated nothing before this
    # change only because the vendored copy MISDECLARED its version, which is what defeated that
    # mechanism -- the defect being removed here, not one to re-implement in qb.)
    install(CODE "set(_qb_abi_cfg [[cxx_standard=${QB_CXX_STANDARD} compiler=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION} build_type=${CMAKE_BUILD_TYPE} shared_libs=${_qb_abi_shared}
ssl=${QB_HAS_SSL} openssl=${OPENSSL_VERSION} compression=${QB_HAS_COMPRESSION} quic=${QB_HAS_QUIC} argon2=${QB_HAS_ARGON2}
unordered_map=${QB_ABI_UNORDERED_MAP} nlohmann=${nlohmann_json_VERSION}]])")
    install(CODE [[
        set(_qb_abi_line "")
        if(EXISTS "${_qb_abi_archive}")
            file(STRINGS "${_qb_abi_archive}" _qb_abi_hits REGEX "^qb-abi ")
            if(_qb_abi_hits)
                list(GET _qb_abi_hits 0 _qb_abi_line)
            endif()
        endif()
        if(NOT _qb_abi_line)
            set(_qb_abi_line "UNKNOWN - no 'qb-abi ' string found in ${_qb_abi_archive}")
            message(WARNING
                "[qb] abi-fingerprint.txt: could not read the fingerprint out of the archive. "
                "The qb_abi_fingerprint symbol should be present in every qb build; if it is "
                "genuinely absent, the link-time ABI check is not armed in this package.")
        endif()
        file(WRITE "${CMAKE_INSTALL_PREFIX}/share/qb/abi-fingerprint.txt"
"# qb ABI + configuration fingerprint of THIS installed prefix.
# The qb-abi line is read out of the installed archive itself, not re-derived. The five axes it
# names are enforced AT LINK TIME: a consumer compiled with a different value references a symbol
# this archive does not define, and the link fails naming the axis (see qb/utility/abi.h).
# The configuration lines below are NOT link-enforced -- they record what configure resolved.
${_qb_abi_line}
${_qb_abi_cfg}
")
        message(STATUS "[qb] wrote ${CMAKE_INSTALL_PREFIX}/share/qb/abi-fingerprint.txt")
    ]])

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
# One entry: QB_INCLUDE_DIR (qb's headers + its vendored forks under qb/vendor/). It used to carry
# a second, QB_MODULES_DIR, for the bundled nlohmann fallback; 3.0 deleted that copy, so the build
# tree and the installed tree now agree on having a single root. nlohmann's own include path
# arrives as a usage requirement of qb-nlohmann, from find_package or FetchContent. Legacy
# variable; prefer linking qb::core / qb::io and inheriting the include paths from the target.
set(QB_INCLUDE_DIRS "${QB_INCLUDE_DIR}" CACHE STRING "qb include directories")

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
    set(QB_INCLUDE_DIRS "${QB_INCLUDE_DIR}" PARENT_SCOPE)

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
