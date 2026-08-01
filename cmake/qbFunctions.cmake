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
# qb Framework - User Functions Module
#
# This file provides simple and powerful functions for users of the qb framework.
# It includes functions for creating executables, tests, modules, and more.
# -----------------------------------------------------------------------------

if(QB_FUNCTIONS_INCLUDED)
    return()
endif()
set(QB_FUNCTIONS_INCLUDED TRUE)

# -----------------------------------------------------------------------------
# Test strictness toggle
# -----------------------------------------------------------------------------
# Promote warnings to errors on test/benchmark targets. Default ON in CI, OFF for
# casual local builds, so the per-phase "0-warning" exit criterion is enforced in CI
# without making every local rebuild fail on a stray warning.
if(NOT DEFINED QB_CI)
    if(DEFINED ENV{CI})
        set(QB_CI ON)
    else()
        set(QB_CI OFF)
    endif()
endif()
option(QB_TESTS_WERROR "Treat warnings as errors in test/benchmark targets" ${QB_CI})

# Apply -Werror / /WX to a test or benchmark target, only when QB_TESTS_WERROR is ON.
# The repo strict-warning *set* is applied separately by qb_apply_compiler_flags(); this
# only flips the promote-to-error switch.
function(qb_apply_test_werror target)
    if(NOT QB_TESTS_WERROR)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX)
    else()
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Internal Helper Functions
# -----------------------------------------------------------------------------

# Internal function to choose the correct usage scope for a target.
function(_qb_target_usage_scope target out_var)
    get_target_property(_qb_target_type ${target} TYPE)
    if(_qb_target_type STREQUAL "INTERFACE_LIBRARY")
        set(${out_var} INTERFACE PARENT_SCOPE)
    else()
        set(${out_var} PUBLIC PARENT_SCOPE)
    endif()
endfunction()

# Internal function to apply qb's transitive usage requirements.
function(_qb_apply_target_usage_properties target)
    _qb_target_usage_scope(${target} _qb_usage_scope)

    # Propagate qb's C++ standard requirement as a usage requirement so consumers
    # linking qb/qbm targets compile at the framework-required language level. The
    # CXX_STANDARD target property below is not transitive.
    target_compile_features(${target} ${_qb_usage_scope} cxx_std_${QB_CXX_STANDARD})

    # Add include directories with the same scope. For INTERFACE modules this is
    # their whole build contract; for compiled targets it is also inherited by
    # downstream consumers.
    target_include_directories(${target}
        ${_qb_usage_scope}
            "$<BUILD_INTERFACE:${QB_INCLUDE_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    target_include_directories(${target}
        SYSTEM ${_qb_usage_scope}
            "$<BUILD_INTERFACE:${QB_MODULES_DIR}>"
    )

    if(QB_COMPILE_DEFINITIONS)
        target_compile_definitions(${target} ${_qb_usage_scope} ${QB_COMPILE_DEFINITIONS})
    endif()
endfunction()

# Internal function to apply common target properties
function(_qb_apply_target_properties target)
    # Set C++ standard
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${QB_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    _qb_apply_target_usage_properties(${target})

    # Apply compiler flags
    qb_apply_compiler_flags(${target})
    
    # Apply linker flags
    qb_apply_linker_flags(${target})

    # Output directories come from the global CMAKE_*_OUTPUT_DIRECTORY set politely in
    # qbConfig (tests/benchmarks override to their own bin/ subdir). No per-target
    # override here, so an embedding parent's chosen layout is respected.
endfunction()

# Internal function to parse common arguments
#
# TIER/MODULE/LABELS/REQUIRES/TIMEOUT/RESOURCE_LOCK/WINDOWS_EXCLUDE are the test-suite
# convention args (see docs/tests-audit/_CONVENTIONS.md §4). They are parsed here for all
# target kinds but only consumed by qb_add_test / qb_register_module_test / qb_add_benchmark;
# qb_add_library / qb_add_executable ignore them harmlessly.
function(_qb_parse_common_args prefix)
    set(options PRIVATE_LINKAGE WINDOWS_EXCLUDE)
    set(oneValueArgs NAME VERSION DESCRIPTION OUTPUT_NAME TIER MODULE TIMEOUT)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES COMPILE_OPTIONS LINK_OPTIONS
        LABELS REQUIRES RESOURCE_LOCK)

    cmake_parse_arguments(${prefix} "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    # Set parsed arguments in parent scope
    foreach(arg IN LISTS options oneValueArgs multiValueArgs)
        set(${prefix}_${arg} ${${prefix}_${arg}} PARENT_SCOPE)
    endforeach()
    
    # Set unparsed arguments
    set(${prefix}_UNPARSED_ARGUMENTS ${${prefix}_UNPARSED_ARGUMENTS} PARENT_SCOPE)
endfunction()

# Internal function to apply target dependencies
function(_qb_apply_dependencies target dependencies)
    if(NOT dependencies)
        return()
    endif()
    
    # Resolve qb library dependencies
    set(resolved_deps)
    foreach(dep ${dependencies})
        if(dep STREQUAL "qb-io")
            list(APPEND resolved_deps qb::io)
        elseif(dep STREQUAL "qb-core")
            list(APPEND resolved_deps qb::core)
        elseif(dep MATCHES "^qbm-")
            # Module dependency
            string(REPLACE "qbm-" "" module_name ${dep})
            list(APPEND resolved_deps qbm::${module_name})
        else()
            # External dependency
            list(APPEND resolved_deps ${dep})
        endif()
    endforeach()
    
    # Apply resolved dependencies. INTERFACE libraries cannot consume PUBLIC or
    # PRIVATE items, so header-only qb modules receive the same dependencies as
    # usage requirements.
    if(resolved_deps)
        _qb_target_usage_scope(${target} _qb_usage_scope)
        target_link_libraries(${target} ${_qb_usage_scope} ${resolved_deps})
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Library Functions
# -----------------------------------------------------------------------------

# qb_add_library - Create a library with qb framework integration
function(qb_add_library)
    _qb_parse_common_args(LIB ${ARGN})
    
    if(NOT LIB_NAME)
        qb_error_message("qb_add_library: NAME is required")
    endif()
    
    if(NOT LIB_SOURCES)
        qb_error_message("qb_add_library: SOURCES is required")
    endif()
    
    # Determine library type
    if(QB_BUILD_SHARED_LIBS)
        set(lib_type SHARED)
    else()
        set(lib_type STATIC)
    endif()
    
    # Create library
    add_library(${LIB_NAME} ${lib_type} ${LIB_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${LIB_NAME})
    
    # Apply dependencies
    _qb_apply_dependencies(${LIB_NAME} "${LIB_DEPENDS}")
    
    # Apply additional includes
    if(LIB_INCLUDES)
        target_include_directories(${LIB_NAME} PRIVATE ${LIB_INCLUDES})
    endif()
    
    # Apply definitions
    if(LIB_DEFINES)
        target_compile_definitions(${LIB_NAME} PRIVATE ${LIB_DEFINES})
    endif()
    
    # Apply compile options
    if(LIB_COMPILE_OPTIONS)
        target_compile_options(${LIB_NAME} PRIVATE ${LIB_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(LIB_LINK_OPTIONS)
        target_link_options(${LIB_NAME} PRIVATE ${LIB_LINK_OPTIONS})
    endif()
    
    # Set output name if specified
    if(LIB_OUTPUT_NAME)
        set_target_properties(${LIB_NAME} PROPERTIES OUTPUT_NAME ${LIB_OUTPUT_NAME})
    endif()
    
    # Set version if specified
    if(LIB_VERSION)
        set_target_properties(${LIB_NAME} PROPERTIES VERSION ${LIB_VERSION})
    endif()
    
    qb_debug_message("Created library: ${LIB_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Executable Functions
# -----------------------------------------------------------------------------

# qb_add_executable - Create an executable with qb framework integration
function(qb_add_executable)
    _qb_parse_common_args(EXE ${ARGN})
    
    if(NOT EXE_NAME)
        qb_error_message("qb_add_executable: NAME is required")
    endif()
    
    if(NOT EXE_SOURCES)
        qb_error_message("qb_add_executable: SOURCES is required")
    endif()
    
    # Create executable
    add_executable(${EXE_NAME} ${EXE_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${EXE_NAME})
    
    # Apply dependencies
    _qb_apply_dependencies(${EXE_NAME} "${EXE_DEPENDS}")
    
    # Apply additional includes
    if(EXE_INCLUDES)
        target_include_directories(${EXE_NAME} PRIVATE ${EXE_INCLUDES})
    endif()
    
    # Apply definitions
    if(EXE_DEFINES)
        target_compile_definitions(${EXE_NAME} PRIVATE ${EXE_DEFINES})
    endif()
    
    # Apply compile options
    if(EXE_COMPILE_OPTIONS)
        target_compile_options(${EXE_NAME} PRIVATE ${EXE_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(EXE_LINK_OPTIONS)
        target_link_options(${EXE_NAME} PRIVATE ${EXE_LINK_OPTIONS})
    endif()
    
    # Set output name if specified
    if(EXE_OUTPUT_NAME)
        set_target_properties(${EXE_NAME} PROPERTIES OUTPUT_NAME ${EXE_OUTPUT_NAME})
    endif()
    
    qb_debug_message("Created executable: ${EXE_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Test Functions
# -----------------------------------------------------------------------------

# Internal: translate the convention args (TIER/MODULE/LABELS/REQUIRES) into the concrete
# CTest properties (labels, timeout, resource locks, skip-regex) and a skip-registration
# decision. Implements docs/tests-audit/_CONVENTIONS.md §4.3/§4.5.
#
#   out_prefix  - results are returned as ${out_prefix}_LABELS / _TIMEOUT / _LOCKS /
#                 _SKIP_REGEX / _SKIP_REGISTER (TRUE if a compile-gated feature is absent).
# Reads (one/multi-value): TIER MODULE LABELS REQUIRES TIMEOUT RESOURCE_LOCK.
function(_qb_test_conventions out_prefix)
    set(oneValueArgs TIER MODULE TIMEOUT)
    set(multiValueArgs LABELS REQUIRES RESOURCE_LOCK)
    cmake_parse_arguments(C "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(_labels "")
    set(_locks "${C_RESOURCE_LOCK}")
    set(_skip_register FALSE)
    set(_skip_regex "")

    # Per-tier default timeout (overridable via TIMEOUT).
    set(_timeout "")
    if(C_TIER)
        list(APPEND _labels "tier:${C_TIER}")
        if(C_TIER STREQUAL "unit")
            set(_timeout 60)
        elseif(C_TIER STREQUAL "system")
            set(_timeout 120)
        elseif(C_TIER STREQUAL "integration")
            set(_timeout 300)
        endif()
    endif()
    if(C_MODULE)
        list(APPEND _labels "module:${C_MODULE}")
    endif()
    if(C_LABELS)
        list(APPEND _labels ${C_LABELS})
    endif()

    # REQUIRES tokens: each adds its label; ssl/quic/compression compile-gate on the
    # matching QB_HAS_* feature (absent → skip registration, never a build failure);
    # live wires the skip-not-fail contract (label + integration lock + skip-regex).
    foreach(_req IN LISTS C_REQUIRES)
        if(_req STREQUAL "ssl")
            list(APPEND _labels "ssl")
            if(NOT QB_HAS_SSL)
                set(_skip_register TRUE)
            endif()
        elseif(_req STREQUAL "quic")
            list(APPEND _labels "quic")
            if(NOT QB_HAS_QUIC)
                set(_skip_register TRUE)
            endif()
        elseif(_req STREQUAL "compression")
            list(APPEND _labels "compression")
            if(NOT QB_HAS_COMPRESSION)
                set(_skip_register TRUE)
            endif()
        elseif(_req STREQUAL "network")
            list(APPEND _labels "network")
        elseif(_req STREQUAL "live")
            list(APPEND _labels "live")
            if(C_MODULE)
                list(APPEND _locks "${C_MODULE}-integration")
            endif()
            # A REQUIRES-live binary skips every case when its daemon is unreachable. gtest
            # still exits 0, so make CTest report *Skipped* (not Passed/Failed) when the shared
            # integration fixture prints this exact sentinel. It is intentionally specific (NOT
            # the generic "[ SKIPPED ]") so a per-case capability skip does not mark the whole
            # binary Skipped — only a daemon-down full-skip does.
            set(_skip_regex "QBM_INTEGRATION_SKIP_DAEMON_UNREACHABLE")
        else()
            # Unknown REQUIRES token: treat as a plain capability label.
            list(APPEND _labels "${_req}")
        endif()
    endforeach()

    # Backward-compat: a call that passes none of the convention args keeps the legacy
    # flat label so the pre-migration tree stays green.
    if(NOT _labels)
        set(_labels "qb-tests")
    endif()

    if(C_TIMEOUT)
        set(_timeout ${C_TIMEOUT})
    endif()
    if(NOT _timeout)
        set(_timeout 300)  # safe default for un-tiered/legacy calls
    endif()

    list(REMOVE_DUPLICATES _labels)
    set(${out_prefix}_LABELS "${_labels}" PARENT_SCOPE)
    set(${out_prefix}_TIMEOUT "${_timeout}" PARENT_SCOPE)
    set(${out_prefix}_LOCKS "${_locks}" PARENT_SCOPE)
    set(${out_prefix}_SKIP_REGEX "${_skip_regex}" PARENT_SCOPE)
    set(${out_prefix}_SKIP_REGISTER "${_skip_register}" PARENT_SCOPE)
endfunction()

# qb_add_test - Create a test with qb framework integration
function(qb_add_test)
    _qb_parse_common_args(TEST ${ARGN})
    
    if(NOT TEST_NAME)
        qb_error_message("qb_add_test: NAME is required")
    endif()
    
    if(NOT TEST_SOURCES)
        qb_error_message("qb_add_test: SOURCES is required")
    endif()

    # Build the conventional target name <module>-test-<tier>-<name> when MODULE (+TIER) are
    # supplied with a short NAME; fall back to <module>-test-<name> (legacy qbm, no tier) or to
    # NAME verbatim (legacy direct callers that already pass a full target name).
    if(TEST_MODULE AND TEST_TIER)
        set(TEST_NAME "${TEST_MODULE}-test-${TEST_TIER}-${TEST_NAME}")
    elseif(TEST_MODULE)
        set(TEST_NAME "${TEST_MODULE}-test-${TEST_NAME}")
    endif()

    # Only create tests if testing is enabled
    if(NOT QB_BUILD_TESTS)
        return()
    endif()

    # Resolve convention args (labels / timeout / locks / skip-regex / feature gating).
    _qb_test_conventions(_TC
        TIER "${TEST_TIER}" MODULE "${TEST_MODULE}"
        LABELS ${TEST_LABELS} REQUIRES ${TEST_REQUIRES}
        TIMEOUT "${TEST_TIMEOUT}" RESOURCE_LOCK ${TEST_RESOURCE_LOCK})

    # Portability / feature gating: a POSIX-only test on Windows, or a test whose
    # REQUIRES feature (ssl/quic/compression) is absent, is silently NOT registered
    # (no build failure) — exactly mirroring the old list(REMOVE_ITEM ...) exclusions.
    if(TEST_WINDOWS_EXCLUDE AND WIN32)
        qb_debug_message("qb_add_test: ${TEST_NAME} excluded on Windows")
        return()
    endif()
    if(_TC_SKIP_REGISTER)
        qb_debug_message("qb_add_test: ${TEST_NAME} skipped (REQUIRES feature unavailable)")
        return()
    endif()

    # Create test executable
    add_executable(${TEST_NAME} ${TEST_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${TEST_NAME})
    
    if(TARGET GTest::gtest_main)
        target_link_libraries(${TEST_NAME} PRIVATE GTest::gtest_main)
    elseif(TARGET gtest_main)
        target_link_libraries(${TEST_NAME} PRIVATE gtest_main)
    else()
        qb_error_message("qb_add_test: no GTest target (enable QB_BUILD_TESTS and FetchContent/system GTest)")
    endif()

    # qb_add_test already links gtest_main; strip it from DEPENDS to avoid duplicate -lgtest_main (ld warning).
    set(_test_depends_filtered ${TEST_DEPENDS})
    if(_test_depends_filtered)
        list(REMOVE_ITEM _test_depends_filtered gtest_main)
    endif()

    # Apply dependencies
    _qb_apply_dependencies(${TEST_NAME} "${_test_depends_filtered}")
    
    # Apply additional includes
    if(TEST_INCLUDES)
        target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDES})
    endif()
    
    # Apply definitions
    if(TEST_DEFINES)
        target_compile_definitions(${TEST_NAME} PRIVATE ${TEST_DEFINES})
    endif()
    
    # Apply compile options
    if(TEST_COMPILE_OPTIONS)
        target_compile_options(${TEST_NAME} PRIVATE ${TEST_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(TEST_LINK_OPTIONS)
        target_link_options(${TEST_NAME} PRIVATE ${TEST_LINK_OPTIONS})
    endif()
    
    # Set test output directory
    set(TEST_BINARY_DIR "${CMAKE_BINARY_DIR}/bin/tests")
    set_target_properties(${TEST_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_BINARY_DIR}"
    )

    # On Windows, copy all runtime DLLs (gtest, gmock, OpenSSL, etc.) next to the
    # test executable so it can be launched directly without touching PATH.
    # We delegate to a cmake -P script instead of calling copy_if_different directly:
    # when $<TARGET_RUNTIME_DLLS:…> is empty (all deps are static), copy_if_different
    # receives no source files and exits with code 1. The script is a safe no-op.
    #
    # Use CMAKE_CURRENT_FUNCTION_LIST_DIR (CMake 3.17+) which resolves to the
    # directory of THIS file at function-definition time, not the calling directory.
    # This avoids the scope issue where QB_CMAKE_DIR is undefined when qb_add_test()
    # is called from a qbm subdirectory that was added after qb's own scope closed.
    if(WIN32)
        add_custom_command(TARGET ${TEST_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DDLL_LIST=$<JOIN:$<TARGET_RUNTIME_DLLS:${TEST_NAME}>,;>"
                "-DDEST_DIR=${TEST_BINARY_DIR}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_runtime_dlls.cmake"
            COMMAND_EXPAND_LISTS
        )
    endif()

    # Make test depend on SSL resources if they exist
    if(QB_HAS_SSL AND TARGET qb_copy_test_ssl_resources)
        add_dependencies(${TEST_NAME} qb_copy_test_ssl_resources)
    endif()
    
    # Register test with CTest
    # Set working directory to where the binary is located so tests can find resources
    add_test(NAME ${TEST_NAME} 
             COMMAND ${TEST_NAME}
             WORKING_DIRECTORY "${TEST_BINARY_DIR}")

    set_property(GLOBAL APPEND PROPERTY QB_TEST_TARGETS ${TEST_NAME})

    # CTest integration: labels (tier:/module:/feature), per-tier timeout, resource locks,
    # and — for REQUIRES live — the skip-regex that makes a daemon-down run report Skipped.
    set_tests_properties(${TEST_NAME} PROPERTIES
        TIMEOUT "${_TC_TIMEOUT}"
        LABELS "${_TC_LABELS}"
    )
    if(_TC_LOCKS)
        set_tests_properties(${TEST_NAME} PROPERTIES RESOURCE_LOCK "${_TC_LOCKS}")
    endif()
    if(_TC_SKIP_REGEX)
        set_tests_properties(${TEST_NAME} PROPERTIES SKIP_REGULAR_EXPRESSION "${_TC_SKIP_REGEX}")
    endif()

    # IDE folder grouping mirrors the on-disk tier tree when MODULE/TIER are supplied.
    if(TEST_MODULE AND TEST_TIER)
        set_target_properties(${TEST_NAME} PROPERTIES FOLDER "Tests/${TEST_MODULE}/${TEST_TIER}")
    endif()

    # Strict warnings → errors only when QB_TESTS_WERROR is ON (default = CI).
    qb_apply_test_werror(${TEST_NAME})

    qb_debug_message("Created test: ${TEST_NAME} (labels: ${_TC_LABELS})")
endfunction()

# -----------------------------------------------------------------------------
# Benchmark Functions
# -----------------------------------------------------------------------------

# qb_add_benchmark - Create a benchmark with qb framework integration
function(qb_add_benchmark)
    _qb_parse_common_args(BENCH ${ARGN})
    
    if(NOT BENCH_NAME)
        qb_error_message("qb_add_benchmark: NAME is required")
    endif()
    
    if(NOT BENCH_SOURCES)
        qb_error_message("qb_add_benchmark: SOURCES is required")
    endif()
    
    # Only create benchmarks if benchmarking is enabled
    if(NOT QB_BUILD_BENCHMARKS)
        return()
    endif()

    # Feature / portability gating (benchmarks are daemon-free by convention; REQUIRES
    # ssl/quic/compression compile-gate, WINDOWS_EXCLUDE skips POSIX-only benches).
    _qb_test_conventions(_BC
        TIER benchmark MODULE "${BENCH_MODULE}"
        LABELS ${BENCH_LABELS} REQUIRES ${BENCH_REQUIRES})
    if(BENCH_WINDOWS_EXCLUDE AND WIN32)
        return()
    endif()
    if(_BC_SKIP_REGISTER)
        qb_debug_message("qb_add_benchmark: ${BENCH_NAME} skipped (REQUIRES feature unavailable)")
        return()
    endif()

    # Create benchmark executable
    add_executable(${BENCH_NAME} ${BENCH_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${BENCH_NAME})
    # Deprecations warn but do not fail HERE only. Benchmark targets are the one place that
    # compiles against google-benchmark's headers, and its API name for the registration object
    # differs by version: the pinned FetchContent build (QB_GOOGLEBENCHMARK_GIT_TAG, v1.9.2) knows
    # only `benchmark::internal::Benchmark`, while newer system packages
    # (-DQB_USE_SYSTEM_BENCHMARK=ON, which CI uses) deprecate that spelling in favour of
    # `benchmark::Benchmark` — an alias v1.9.2 does not have. No single spelling compiles warning-
    # free under both, so promoting a THIRD-PARTY deprecation to an error just makes the build
    # depend on which benchmark package the machine happens to provide. Our own -Werror still
    # applies to every other warning in these targets, and to all test targets unchanged.
    target_compile_options(${BENCH_NAME} PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-c2y-extensions>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-error=deprecated-declarations>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/wd4996>
    )
    
    if(TARGET benchmark::benchmark)
        target_link_libraries(${BENCH_NAME} PRIVATE benchmark::benchmark)
    elseif(TARGET benchmark)
        target_link_libraries(${BENCH_NAME} PRIVATE benchmark)
    else()
        qb_error_message("qb_add_benchmark: no benchmark target (enable QB_BUILD_BENCHMARKS and FetchContent/system benchmark)")
    endif()
    
    # Apply dependencies
    _qb_apply_dependencies(${BENCH_NAME} "${BENCH_DEPENDS}")
    
    # Apply additional includes
    if(BENCH_INCLUDES)
        target_include_directories(${BENCH_NAME} PRIVATE ${BENCH_INCLUDES})
    endif()
    
    # Apply definitions
    if(BENCH_DEFINES)
        target_compile_definitions(${BENCH_NAME} PRIVATE ${BENCH_DEFINES})
    endif()
    
    # Apply compile options
    if(BENCH_COMPILE_OPTIONS)
        target_compile_options(${BENCH_NAME} PRIVATE ${BENCH_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(BENCH_LINK_OPTIONS)
        target_link_options(${BENCH_NAME} PRIVATE ${BENCH_LINK_OPTIONS})
    endif()
    
    # Set benchmark output directory
    set(BENCH_BINARY_DIR "${CMAKE_BINARY_DIR}/bin/benchmarks")
    set_target_properties(${BENCH_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BENCH_BINARY_DIR}"
    )

    # On Windows, copy all runtime DLLs (benchmark, OpenSSL, etc.) next to the
    # benchmark executable so it can be launched directly without touching PATH.
    # Same rationale as for tests: use a -P script to handle the empty-list case safely.
    # CMAKE_CURRENT_FUNCTION_LIST_DIR resolves to this file's directory regardless
    # of which scope calls qb_add_benchmark().
    if(WIN32)
        add_custom_command(TARGET ${BENCH_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DDLL_LIST=$<JOIN:$<TARGET_RUNTIME_DLLS:${BENCH_NAME}>,;>"
                "-DDEST_DIR=${BENCH_BINARY_DIR}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_runtime_dlls.cmake"
            COMMAND_EXPAND_LISTS
        )
    endif()

    if(BENCH_MODULE)
        set_target_properties(${BENCH_NAME} PROPERTIES FOLDER "Tests/${BENCH_MODULE}/benchmark")
    endif()
    qb_apply_test_werror(${BENCH_NAME})

    qb_debug_message("Created benchmark: ${BENCH_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Module Functions
# -----------------------------------------------------------------------------

# qb_register_module - Register a qb module
function(qb_register_module)
    set(options HEADER_ONLY)
    set(oneValueArgs NAME VERSION DESCRIPTION)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES)
    
    cmake_parse_arguments(MOD "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT MOD_NAME)
        qb_error_message("qb_register_module: NAME is required")
    endif()
    
    # Set module names
    set(module_target "qbm-${MOD_NAME}")
    set(module_alias "qbm::${MOD_NAME}")
    
    # Create module library
    if(MOD_HEADER_ONLY)
        # Header-only module
        add_library(${module_target} INTERFACE)

        # Header-only modules do not compile sources, but they still expose qb's
        # full usage contract to consumers.
        _qb_apply_target_usage_properties(${module_target})

        # Apply interface properties
        if(MOD_INCLUDES)
            target_include_directories(${module_target} INTERFACE ${MOD_INCLUDES})
        endif()
        
        if(MOD_DEFINES)
            target_compile_definitions(${module_target} INTERFACE ${MOD_DEFINES})
        endif()
        
        # Apply dependencies
        if(MOD_DEPENDS)
            _qb_apply_dependencies(${module_target} "${MOD_DEPENDS}")
        endif()
        
    else()
        # Regular module with sources
        if(NOT MOD_SOURCES)
            qb_error_message("qb_register_module: SOURCES is required for non-header-only modules")
        endif()
        
        # Create library
        if(QB_BUILD_SHARED_LIBS)
            add_library(${module_target} SHARED ${MOD_SOURCES})
        else()
            add_library(${module_target} STATIC ${MOD_SOURCES})
        endif()
        
        # Apply common properties
        _qb_apply_target_properties(${module_target})
        
        # Apply dependencies
        _qb_apply_dependencies(${module_target} "${MOD_DEPENDS}")
        
        # Module include directories are PUBLIC so consumers can
        # reach the module's headers through target_link_libraries.
        if(MOD_INCLUDES)
            target_include_directories(${module_target} PUBLIC ${MOD_INCLUDES})
        endif()
        
        # Apply definitions
        if(MOD_DEFINES)
            target_compile_definitions(${module_target} PRIVATE ${MOD_DEFINES})
        endif()
        
        # Set version if specified
        if(MOD_VERSION)
            set_target_properties(${module_target} PROPERTIES VERSION ${MOD_VERSION})
        endif()
    endif()
    
    # Expose the module's parent directory as an include root so consumers reach
    # the module umbrella header by its prefix (e.g. <http/http.h>, <http/ws.h>,
    # <redis/redis.h>, <pgsql/pgsql.h>). The module's own sources use relative
    # includes ("../http.h"), but external consumers (examples, downstream apps)
    # include by module prefix. Marked SYSTEM to match QB_MODULES_DIR handling.
    get_filename_component(_qb_module_include_root "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
    _qb_target_usage_scope(${module_target} _qb_module_scope)
    target_include_directories(${module_target}
        SYSTEM ${_qb_module_scope}
            "$<BUILD_INTERFACE:${_qb_module_include_root}>"
    )

    # Create alias
    add_library(${module_alias} ALIAS ${module_target})
    
    # Add to global module list
    list(APPEND QB_MODULE_LIBRARIES ${module_target})
    set(QB_MODULE_LIBRARIES ${QB_MODULE_LIBRARIES} PARENT_SCOPE)
    
    qb_status_message("Registered module: ${MOD_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Module Loading Functions
# -----------------------------------------------------------------------------

# qb_load_modules - Load all modules from a directory
function(qb_load_modules modules_dir)
    if(NOT IS_DIRECTORY "${modules_dir}")
        qb_error_message("qb_load_modules: Directory does not exist: ${modules_dir}")
    endif()
    
    qb_status_message("Loading modules from: ${modules_dir}")
    
    # Get all subdirectories. CONFIGURE_DEPENDS re-runs the glob on rebuild so a
    # newly added/removed module triggers a reconfigure; sorting makes the module
    # load order deterministic across machines.
    file(GLOB module_dirs RELATIVE "${modules_dir}" CONFIGURE_DEPENDS "${modules_dir}/*")
    list(SORT module_dirs)

    foreach(module_dir ${module_dirs})
        set(full_module_path "${modules_dir}/${module_dir}")
        
        # Check if it's a directory and has a CMakeLists.txt
        if(IS_DIRECTORY "${full_module_path}")
            set(cmake_file "${full_module_path}/CMakeLists.txt")
            if(EXISTS "${cmake_file}")
                qb_debug_message("Loading module: ${module_dir}")
                add_subdirectory("${full_module_path}")
            else()
                qb_debug_message("Skipping module ${module_dir}: no CMakeLists.txt found")
            endif()
        endif()
    endforeach()
    
endfunction()

# -----------------------------------------------------------------------------
# Test Module Functions
# -----------------------------------------------------------------------------

# qb_register_module_test - Register a test for a module
function(qb_register_module_test)
    set(options WINDOWS_EXCLUDE)
    set(oneValueArgs MODULE_NAME TEST_NAME TIER TIMEOUT)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES LABELS REQUIRES RESOURCE_LOCK)

    cmake_parse_arguments(MTEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT MTEST_MODULE_NAME)
        qb_error_message("qb_register_module_test: MODULE_NAME is required")
    endif()

    if(NOT MTEST_TEST_NAME)
        qb_error_message("qb_register_module_test: TEST_NAME is required")
    endif()

    if(NOT MTEST_SOURCES)
        qb_error_message("qb_register_module_test: SOURCES is required")
    endif()

    # Only create tests if testing is enabled
    if(NOT QB_BUILD_TESTS)
        return()
    endif()

    # Target name: <module>-test-<tier>-<name> when a TIER is given (the new convention),
    # else the legacy <module>-test-<name> so un-migrated call sites keep their names.
    if(MTEST_TIER)
        set(test_target "qbm-${MTEST_MODULE_NAME}-test-${MTEST_TIER}-${MTEST_TEST_NAME}")
    else()
        set(test_target "qbm-${MTEST_MODULE_NAME}-test-${MTEST_TEST_NAME}")
    endif()

    set(_we "")
    if(MTEST_WINDOWS_EXCLUDE)
        set(_we WINDOWS_EXCLUDE)
    endif()

    # Create test, forwarding the convention args (MODULE → module:qbm-<name> label).
    # NAME is the SHORT subject; qb_add_test rebuilds the same <module>-test-<tier>-<name>.
    qb_add_test(
        NAME ${MTEST_TEST_NAME}
        SOURCES ${MTEST_SOURCES}
        DEPENDS qbm-${MTEST_MODULE_NAME} ${MTEST_DEPENDS}
        INCLUDES ${MTEST_INCLUDES}
        DEFINES ${MTEST_DEFINES}
        MODULE "qbm-${MTEST_MODULE_NAME}"
        TIER "${MTEST_TIER}"
        LABELS ${MTEST_LABELS}
        REQUIRES ${MTEST_REQUIRES}
        TIMEOUT "${MTEST_TIMEOUT}"
        RESOURCE_LOCK ${MTEST_RESOURCE_LOCK}
        ${_we}
    )

    qb_debug_message("Created module test: ${test_target}")
endfunction()

# qb_register_module_benchmark - Register a google-benchmark target for a module
# Target name: qbm-<module>-bench-<name>. Daemon-free by convention (REQUIRES is for
# ssl/quic compile-gating only).
function(qb_register_module_benchmark)
    set(options WINDOWS_EXCLUDE)
    set(oneValueArgs MODULE_NAME BENCH_NAME)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES LABELS REQUIRES)

    cmake_parse_arguments(MBENCH "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT MBENCH_MODULE_NAME)
        qb_error_message("qb_register_module_benchmark: MODULE_NAME is required")
    endif()
    if(NOT MBENCH_BENCH_NAME)
        qb_error_message("qb_register_module_benchmark: BENCH_NAME is required")
    endif()
    if(NOT MBENCH_SOURCES)
        qb_error_message("qb_register_module_benchmark: SOURCES is required")
    endif()
    if(NOT QB_BUILD_BENCHMARKS)
        return()
    endif()

    set(bench_target "qbm-${MBENCH_MODULE_NAME}-bench-${MBENCH_BENCH_NAME}")
    set(_we "")
    if(MBENCH_WINDOWS_EXCLUDE)
        set(_we WINDOWS_EXCLUDE)
    endif()

    qb_add_benchmark(
        NAME ${bench_target}
        SOURCES ${MBENCH_SOURCES}
        DEPENDS qbm-${MBENCH_MODULE_NAME} ${MBENCH_DEPENDS}
        INCLUDES ${MBENCH_INCLUDES}
        DEFINES ${MBENCH_DEFINES}
        MODULE "qbm-${MBENCH_MODULE_NAME}"
        LABELS ${MBENCH_LABELS}
        REQUIRES ${MBENCH_REQUIRES}
        ${_we}
    )

    qb_debug_message("Created module benchmark: ${bench_target}")
endfunction()

# -----------------------------------------------------------------------------
# Test Resource Functions
# -----------------------------------------------------------------------------

# qb_setup_test_resources - Setup SSL resources for tests (centralized)
# This function creates a global target that copies SSL resources to the test directory
# Call this once in your root CMakeLists.txt or test configuration
function(qb_setup_test_resources)
    if(NOT QB_BUILD_TESTS)
        return()
    endif()
    
    set(TEST_RESOURCES_DIR "${CMAKE_BINARY_DIR}/bin/tests")
    
    # Create a global target for copying SSL resources
    if(QB_HAS_SSL AND QB_SSL_RESOURCES)
        if(NOT TARGET qb_copy_test_ssl_resources)
            add_custom_target(qb_copy_test_ssl_resources ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_RESOURCES_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QB_SSL_RESOURCES}" "${TEST_RESOURCES_DIR}/ssl"
                # Tests load certificates by bare name ("cert.pem"/"key.pem") and run
                # with WORKING_DIRECTORY = the test bin dir (see qb_add_test), so also
                # stage the resources directly there. Without this the SSL tests can
                # only find the certs from a hand-copied bin/tests, otherwise they
                # silently skip (or crash, for tests that don't guard a null context).
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QB_SSL_RESOURCES}" "${TEST_RESOURCES_DIR}"
                COMMENT "Copying SSL resources to test directory: ${TEST_RESOURCES_DIR} (+/ssl)"
            )
            
            # Set folder for IDE organization
            set_target_properties(qb_copy_test_ssl_resources PROPERTIES
                FOLDER "Tests/Resources"
            )
            
            qb_status_message("SSL test resources will be copied to: ${TEST_RESOURCES_DIR}/ssl")
        endif()
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Utility Functions
# -----------------------------------------------------------------------------

# qb_copy_resources - Copy resources to output directory
function(qb_copy_resources)
    set(options)
    set(oneValueArgs TARGET DESTINATION)
    set(multiValueArgs RESOURCES)
    
    cmake_parse_arguments(RES "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT RES_TARGET)
        qb_error_message("qb_copy_resources: TARGET is required")
    endif()
    
    if(NOT RES_RESOURCES)
        qb_error_message("qb_copy_resources: RESOURCES is required")
    endif()
    
    if(NOT RES_DESTINATION)
        set(RES_DESTINATION "${CMAKE_BINARY_DIR}/bin/resources")
    endif()
    
    # Create custom target for copying resources
    set(copy_target "${RES_TARGET}_copy_resources")
    
    add_custom_target(${copy_target}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${RES_RESOURCES} ${RES_DESTINATION}
        COMMENT "Copying resources for ${RES_TARGET}"
    )
    
    # Make the main target depend on the copy target
    add_dependencies(${RES_TARGET} ${copy_target})
    
    qb_debug_message("Added resource copy for: ${RES_TARGET}")
endfunction()

# qb_install_target - Install a target with proper configuration
function(qb_install_target)
    set(options)
    set(oneValueArgs TARGET)
    set(multiValueArgs)
    
    cmake_parse_arguments(INST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT INST_TARGET)
        qb_error_message("qb_install_target: TARGET is required")
    endif()
    
    if(NOT QB_INSTALL)
        return()
    endif()
    
    # Install target
    install(TARGETS ${INST_TARGET}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    
    qb_debug_message("Configured installation for: ${INST_TARGET}")
endfunction()

# Mark functions as loaded
set(QB_FUNCTIONS_LOADED TRUE CACHE INTERNAL "qb functions loaded")
