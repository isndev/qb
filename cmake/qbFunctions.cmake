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

# THE install/export rule, shared verbatim by qb/CMakeLists.txt and qb_register_module().
include("${CMAKE_CURRENT_LIST_DIR}/qbPackage.cmake")

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

# Run every GoogleTest binary in a randomised order (`--gtest_shuffle`).
#
# This is on by default because a suite whose result depends on execution order is not measuring
# what it claims to. Measured instance, 3.0.0: `qb-core-test-unit-type-id-identity` reported
# `9 tests PASSED` in declaration order and `rc=139` (SIGSEGV) on 3 of 6 shuffle seeds — one of
# its tests published a stack address into a process-wide registry, and the suite only survived
# because that test happened to be declared last. ASan did not see it either (the reader lives in
# the un-instrumented archive), so ordering was the *only* thing standing between the tree and a
# corrupt registry.
#
# Shuffling is per-binary — ctest already runs the binaries themselves in an arbitrary,
# parallel order — so the blast radius of a newly-exposed dependency is one executable.
#
# QB_TESTS_SHUFFLE_SEED: 0 (the default, and GoogleTest's own) seeds from the clock and prints
# `Note: Randomizing tests' orders with a seed of NNNN.` on every run, so any failure is
# reproducible with `--gtest_random_seed=NNNN`. Set it to a non-zero value to pin one order — use
# that to bisect a failure, not to make a flaky suite quiet.
option(QB_TESTS_SHUFFLE "Run GoogleTest binaries in a randomised order (--gtest_shuffle)" ON)
set(QB_TESTS_SHUFFLE_SEED "0" CACHE STRING
    "Seed for QB_TESTS_SHUFFLE; 0 = seed from the clock (GoogleTest prints the seed it used)")

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
    # No second include root here any more. This used to add $<BUILD_INTERFACE:${QB_MODULES_DIR}>
    # to EVERY qb and qbm target (485 compile statements in a superproject release build), which is
    # how <ev/...>, <nanolog/...>, <ska_hash/...> and <nlohmann/...> all became reachable by bare
    # prefix -- and, having no $<INSTALL_INTERFACE:> counterpart, is why the installed tree needed
    # bespoke mirror rules that silently drifted. The forks are now under ${QB_INCLUDE_DIR} above;
    # nlohmann arrives as a usage requirement of the qb-nlohmann target linked by qb-io.

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
        # Per target, not via the global CMAKE_POSITION_INDEPENDENT_CODE. qb needs PIC so its
        # bundled static dependencies (qev, llhttp) can be linked into a shared qb-io / qbm-* or into
        # a consumer's shared library without "recompile with -fPIC" on Linux -- but that is qb's
        # requirement, not its parent's. Setting the global forced -fPIC onto every target of any
        # project that embeds qb, including ones that deliberately build non-PIC.
        POSITION_INDEPENDENT_CODE ON
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
# convention args (see dev/tests-audit/_CONVENTIONS.md §4). They are parsed here for all
# target kinds. TIER/MODULE/LABELS/TIMEOUT/RESOURCE_LOCK are consumed only by qb_add_test /
# qb_register_module_test / qb_add_benchmark; REQUIRES is consumed by those AND by
# qb_add_executable (capability gating). qb_add_library ignores them all harmlessly.
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
#
# REQUIRES <token>... — optional capability gate, identical in vocabulary and in
# meaning to qb_add_test / qb_add_benchmark: a target whose ssl/quic/compression
# requirement is unmet is silently NOT created, rather than left to fail the build.
# It is resolved by the SAME helper (_qb_test_conventions) so there is exactly one
# definition of "REQUIRES ssl" in the tree; only its SKIP decision is used here —
# labels, timeouts and resource locks are CTest concepts an executable has none of.
function(qb_add_executable)
    _qb_parse_common_args(EXE ${ARGN})

    if(NOT EXE_NAME)
        qb_error_message("qb_add_executable: NAME is required")
    endif()

    if(NOT EXE_SOURCES)
        qb_error_message("qb_add_executable: SOURCES is required")
    endif()

    # Capability gating — must precede add_executable() so a gated-out target never
    # exists (callers test with if(TARGET ...), as the example resource staging does).
    if(EXE_REQUIRES)
        _qb_test_conventions(_EXE_GATE REQUIRES ${EXE_REQUIRES})
        if(_EXE_GATE_SKIP_REGISTER)
            qb_status_message("Skipping executable ${EXE_NAME} (REQUIRES ${EXE_REQUIRES} unavailable)")
            return()
        endif()
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
# decision. Implements dev/tests-audit/_CONVENTIONS.md §4.3/§4.5.
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

    # `requires-multicore` was a LABEL and nothing else -- it described the test without
    # scheduling it. qb PINS each VirtualCore to a CPU (SetThreadAffinityMask on Windows,
    # pthread_setaffinity_np elsewhere, VirtualCore.cpp:431,444) and always to the LOW core
    # indices, so N concurrent multicore tests do not spread over the machine: they land on the
    # same handful of CPUs. SIXTEEN tests carry the label -- fifteen in a QB_WITH_LOGGING=OFF
    # build, since engine-io-smoke is registered conditionally -- and the test presets run
    # `jobs: 4`, so four of them oversubscribe cores 0..k however many the host really has --
    # measured on a 24-core Windows box, `messaging-api` and `ask-roundtrip` each blew their
    # 120s tier timeout in a full parallel run and passed alone.
    #
    # The pinning above is what the rationale rests on, and it is NOT universal: on Apple
    # Silicon `thread_policy_set(THREAD_AFFINITY_POLICY)` returns KERN_NOT_SUPPORTED for every
    # core and VirtualCore.cpp maps that to success, so nothing is pinned there and neither test
    # came near its timeout. The lock still pays for itself on macOS -- `messaging-api` runs
    # 6.1-6.4s serialized against 7.3-9.5s with three concurrent multicore tests -- but by
    # cutting contention, not by undoing an affinity that was never applied.
    #
    # One shared lock, so ctest never schedules two of them at once. The other 340 of macOS
    # `release`'s 356 keep running in parallel: measured cost of the lock on a warm full suite
    # is +0.73s (30.78s -> 31.51s, n=5 and n=8), inside run-to-run spread. Derived from the
    # label rather than written at each of the sixteen call sites, because a lock that has to
    # be remembered per test is a lock the next multicore test will not have. Same mechanism
    # the live-daemon tiers already use above, where a REQUIRES live token appends its own
    # ${C_MODULE}-integration lock.
    if("requires-multicore" IN_LIST _labels)
        list(APPEND _locks "qb-multicore")
    endif()

    list(REMOVE_DUPLICATES _labels)
    list(REMOVE_DUPLICATES _locks)
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
    # No whole-program optimization on test binaries. `/GL` belongs in QB_CXX_FLAGS_RELEASE for the
    # SHIPPED library, where users benefit from it, but it is applied globally and so also lands on
    # every test executable. MSVC then finds /GL objects at link time, ABORTS the link and restarts
    # it with /LTCG -- "restarting link with /LTCG" appears 211 times in one CI run, each restart
    # re-running whole-program codegen for a binary nobody ships. That is the bulk of the ~70 minute
    # Windows job. `/GL-` negates it per target and leaves the library untouched.
    if(MSVC)
        target_compile_options(${TEST_NAME} PRIVATE $<$<CONFIG:Release>:/GL->)
    endif()

    # This avoids the scope issue where QB_CMAKE_DIR is undefined when qb_add_test()
    # is called from a qbm subdirectory that was added after qb's own scope closed.
    #
    # ONE writer, not ~340. The shared deployer is asked for FIRST and the per-target
    # POST_BUILD is its fallback, not its companion. Both wrote the same DLLs into the same
    # ${TEST_BINARY_DIR}, and cmake/qbRuntimeDlls.cmake says in as many words that ~300 test
    # targets doing that under `ninja -j` IS the ERROR_SHARING_VIOLATION race that got
    # VCPKG_APPLOCAL_DEPS turned off -- so the deployer was added BESIDE the defect it was
    # written to replace. It is inert today only because $<TARGET_RUNTIME_DLLS> is empty by
    # construction for every vcpkg dep reached through a Find module (UNKNOWN_LIBRARY), and
    # the script it calls is itself TOCTOU (deploy_runtime_dlls.cmake: EXISTS/IS_NEWER_THAN
    # then file(COPY), with nothing holding the destination between the two).
    #
    # The POST_BUILD is kept, gated, for the Windows build with no vcpkg tree: there
    # qb_ensure_runtime_dll_deployer() has no directory to deploy FROM and returns empty, and
    # $<TARGET_RUNTIME_DLLS> is the only mechanism left. That configuration can still race
    # itself if a dependency does arrive as a genuine imported SHARED_LIBRARY; it is not the
    # configuration CMakePresets' windows-base builds, and closing it needs a real
    # single-writer union of every target's runtime DLLs, which is not this change.
    #
    # KNOWN GAP, measured not assumed: the deployer copies *.dll out of the vcpkg bin dir
    # only, so it does NOT carry qb-core.dll / qb-io.dll, which land in their own target
    # binary dirs (no library here sets RUNTIME_OUTPUT_DIRECTORY). With QB_BUILD_SHARED_LIBS=ON
    # those enter $<TARGET_RUNTIME_DLLS> for all ~340 targets at once -- which is what "armed"
    # meant -- and gating the POST_BUILD off means nothing deploys them. That is a hole, but a
    # hole in a configuration that cannot link on MSVC anyway: QB_API is #ifdef QB_DYNAMIC
    # (src/qb/utility/build_macros.h:63-73), nothing in this build system ever defines
    # QB_DYNAMIC, and the macro annotates 11 classes, all of them in qb-io and none in
    # qb-core -- so a shared qb-core.dll exports nothing at all and no test can link it.
    # Do not "fix" the deployment without first giving the shared build real exports.
    if(WIN32)
        qb_ensure_runtime_dll_deployer("${TEST_BINARY_DIR}" _qb_dll_deployer)
        if(_qb_dll_deployer)
            add_dependencies(${TEST_NAME} ${_qb_dll_deployer})
        else()
            add_custom_command(TARGET ${TEST_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    "-DDLL_LIST=$<JOIN:$<TARGET_RUNTIME_DLLS:${TEST_NAME}>,;>"
                    "-DDEST_DIR=${TEST_BINARY_DIR}"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_runtime_dlls.cmake"
                COMMAND_EXPAND_LISTS
            )
        endif()
    endif()

    # Make test depend on SSL resources if they exist.
    #
    # This fires only for tests registered AFTER qb_setup_test_resources() has run -- in
    # practice the qbm module suites, since qb/CMakeLists.txt calls it at :140 and qb's own
    # tests are registered at :108/:111. The ones registered earlier are wired retroactively
    # from that function, off the QB_TEST_TARGETS global; see the comment there.
    #
    # generate_ssl_certs is named alongside because with the single-writer rule it, not the
    # copy target, is what puts cert.pem/key.pem in bin/tests on any host with openssl.
    if(QB_HAS_SSL AND TARGET qb_copy_test_ssl_resources)
        add_dependencies(${TEST_NAME} qb_copy_test_ssl_resources)
        if(TARGET generate_ssl_certs)
            add_dependencies(${TEST_NAME} generate_ssl_certs)
        endif()
    endif()
    
    # Register test with CTest
    # Set working directory to where the binary is located so tests can find resources
    # --gtest_shuffle (QB_TESTS_SHUFFLE, ON by default): see the option's rationale above.
    set(_TC_RUN_ARGS "")
    if(QB_TESTS_SHUFFLE)
        list(APPEND _TC_RUN_ARGS --gtest_shuffle
                                 "--gtest_random_seed=${QB_TESTS_SHUFFLE_SEED}")
    endif()
    add_test(NAME ${TEST_NAME}
             COMMAND ${TEST_NAME} ${_TC_RUN_ARGS}
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
    # No whole-program optimization on benchmark binaries -- see the same note on test targets.
    if(MSVC)
        target_compile_options(${BENCH_NAME} PRIVATE $<$<CONFIG:Release>:/GL->)
    endif()

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
    #
    # Same single-writer rule as qb_add_test(), for the same reason and with the same known
    # gap -- see the long comment there. 62 benchmark targets sharing ${BENCH_BINARY_DIR} is
    # a smaller crowd than the ~340 tests, but it is the same defect, so it gets the same
    # shape: ask for the shared deployer first, fall back to the per-target POST_BUILD only
    # when there is no vcpkg tree to deploy from.
    if(WIN32)
        qb_ensure_runtime_dll_deployer("${BENCH_BINARY_DIR}" _qb_dll_deployer)
        if(_qb_dll_deployer)
            add_dependencies(${BENCH_NAME} ${_qb_dll_deployer})
        else()
            add_custom_command(TARGET ${BENCH_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    "-DDLL_LIST=$<JOIN:$<TARGET_RUNTIME_DLLS:${BENCH_NAME}>,;>"
                    "-DDEST_DIR=${BENCH_BINARY_DIR}"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_runtime_dlls.cmake"
                COMMAND_EXPAND_LISTS
            )
        endif()
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
#
# Installation (QBM_INSTALL=ON) additionally produces a find_package()-able package per
# module: qbm-<NAME>Config.cmake + qbm-<NAME>Targets.cmake + qbm-<NAME>ConfigVersion.cmake
# under <libdir>/cmake/qbm-<NAME>, and the module's public headers under
# <includedir>/qbm/<NAME>. The rules themselves are qb_install_package() in qbPackage.cmake,
# byte-for-byte the same call qb makes for itself; only the arguments differ.
#
# ONE PACKAGE PER MODULE (qbm-http, qbm-pgsql, qbm-redis), not
# find_package(qbm COMPONENTS http). The unit of shipping here is the repository: the modules
# are independent submodules with their own LICENSE, CHANGELOG and version, and a
# component-style package would need a top-level qbmConfig.cmake that NO module can own (each
# would overwrite the others'). Per-module packages also mean installing http does not drag in
# pgsql and redis. qb itself uses COMPONENTS for the opposite reason: core and io are one repo,
# one version, one install unit.
#
#   EXPORT_EXTRA_TARGETS   extra non-imported targets the module links PUBLIC and that must
#                          therefore travel in the SAME export set (e.g. the bundled llhttp
#                          static library). install(EXPORT) is a hard error otherwise.
#   CONFIG_DEPENDENCIES    path (module-relative) to a *.cmake.in configured into
#                          qbm-<NAME>Dependencies.cmake and included by the generated Config
#                          BEFORE the Targets file. This is where a module re-creates the
#                          ad-hoc IMPORTED targets its export set names by string (e.g.
#                          Nghttp3::nghttp3) -- CMake exports such names verbatim and a
#                          consumer has no way to invent them.
#   INSTALL_CMAKE_FILES    extra module-relative files copied next to the package config
#                          (typically the Find<Pkg>.cmake modules CONFIG_DEPENDENCIES uses).
#   HEADER_EXCLUDE         optional regex forwarded verbatim to qb_install_package(), which
#                          hands it to install(DIRECTORY) as REGEX ... EXCLUDE. Reaching for
#                          it should feel wrong: under the src/ layout every file beneath
#                          src/qbm/<name>/ ships, which is exactly what makes the layout
#                          checkable, so an exclusion asserts that a file sitting in the
#                          public tree is not part of the public surface. Use it only for a
#                          file that is DEAD -- included by nothing in its own module -- and
#                          say so at the call site. A file that is merely awkward to compile
#                          alone is a self-containment bug, not an exclusion.
#
#                          It exists because qbm-pgsql shipped field_handler.h to every
#                          consumer for years: 371 lines that no TU in the tree includes and
#                          that cannot compile at all (it redefines resultset::row::to, which
#                          the merged tail of resultset.h already defines, and calls a
#                          ParamUnserializer::deserialize that does not exist). It was carried
#                          as a permanent named exclusion in scripts/check-installed-headers.sh
#                          instead -- which kept the gate green by taking the file OUT of the
#                          gate's scope, on the one surface where scope is the whole point.
#                          Not installing it is the smaller lie: the file stays in the tree
#                          for the maintainer decision it is waiting on, and the
#                          self-containment gate goes back to covering everything shipped.
function(qb_register_module)
    set(options HEADER_ONLY)
    set(oneValueArgs NAME VERSION DESCRIPTION CONFIG_DEPENDENCIES HEADER_EXCLUDE)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES EXPORT_EXTRA_TARGETS INSTALL_CMAKE_FILES)

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
        #
        # Pass $<BUILD_INTERFACE:...> genexes, never raw paths. A bare source- or build-tree
        # path here lands in INTERFACE_INCLUDE_DIRECTORIES verbatim, and install(TARGETS ...
        # EXPORT) then fails to GENERATE with "property contains path ... which is prefixed in
        # the source directory" -- a whole-project configure error, not a warning. Most modules
        # need no INCLUDES at all: the qbm/ root added below already reaches every module
        # header by its <name>/... prefix.
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
    
    # THE RULE, module edition: the module's public headers live at src/qbm/<name>/, so
    # <qbm/<name>/...> is the same string in this tree and in an installed prefix, and exactly
    # one top-level name -- `qbm` -- lands on a consumer's include path, owned by no single
    # module. Before 3.0.0 the include root was the module's PARENT directory (the
    # superproject's qbm/, which does not exist in the module's own git repository), which is
    # what made the consume spelling <http/...>, <pgsql/...>, <redis/...> and claimed those
    # three maximally generic names in every consumer's include namespace.
    #
    # A hard error rather than a fallback: a differently-shaped module would install into the
    # wrong place and fail only at a downstream consumer's first #include. ONE argument, not
    # four, because qb_error_message forwards ${ARGN} and CMake joins a list with semicolons.
    if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/qbm/${MOD_NAME}")
        string(CONCAT _qbm_layout_err
            "qbm-${MOD_NAME}: expected the module's public headers under "
            "${CMAKE_CURRENT_SOURCE_DIR}/src/qbm/${MOD_NAME}/ -- src/ IS the include root, so "
            "<qbm/${MOD_NAME}/...> is the same string in this tree and in an installed prefix. "
            "See dev/analysis/SOURCE-LAYOUT-3.0.md.")
        qb_error_message("${_qbm_layout_err}")
    endif()

    # Expose that root so consumers reach the umbrella header by the shipped prefix
    # (<qbm/http/http.h>, <qbm/http/ws.h>, <qbm/redis/redis.h>, <qbm/pgsql/pgsql.h>). The
    # module's own sources include their siblings relatively, but external consumers (examples,
    # downstream apps) and the module's own test suite include by that prefix. Marked SYSTEM so
    # a consumer's -Werror does not fire on a qbm header.
    #
    # The INSTALL_INTERFACE entry is the installed mirror of the SAME root, and it comes from
    # qb_package_include_root() -- the one place either is decided, and the same call
    # qb_install_package() uses to copy one onto the other. Without the pair the build interface
    # has no installed counterpart, and find_package() consumers configure fine and then fail to
    # compile on "'qbm/http/http.h' file not found" -- the drift class that shipped once already
    # in qb (the missing <qb/vendor/qev/qev++.h> root) and is invisible to an in-tree test suite.
    qb_package_include_root("${CMAKE_CURRENT_SOURCE_DIR}"
        _qb_module_include_root _qb_module_install_root)
    _qb_target_usage_scope(${module_target} _qb_module_scope)
    target_include_directories(${module_target}
        SYSTEM ${_qb_module_scope}
            "$<BUILD_INTERFACE:${_qb_module_include_root}>"
            "$<INSTALL_INTERFACE:${_qb_module_install_root}>"
    )

    # Create alias
    add_library(${module_alias} ALIAS ${module_target})

    # Install / export rules (find_package(qbm-<name>) support)
    if(QBM_INSTALL)
        # install(EXPORT NAMESPACE qbm::) prefixes the EXPORT_NAME, which defaults to the target
        # name -- so without this the package would define `qbm::qbm-http` while the build tree
        # defines `qbm::http`, and every consumer's target_link_libraries(qbm::http) would break
        # the moment it switched from add_subdirectory to find_package.
        set_target_properties(${module_target} PROPERTIES EXPORT_NAME ${MOD_NAME})

        # Substituted into qbmModuleConfig.cmake.in by qb_install_package(). Set here because
        # configure_package_config_file() expands @VARS@ from the calling scope.
        set(_QBM_CFG_PKG "qbm-${MOD_NAME}")
        set(_QBM_CFG_NAME "${MOD_NAME}")
        set(_QBM_CFG_INCDIR "${_qb_module_install_root}")
        set(_QBM_CFG_VERSION "${MOD_VERSION}")
        if(NOT _QBM_CFG_VERSION)
            set(_QBM_CFG_VERSION "${QB_FRAMEWORK_VERSION}")
        endif()
        string(TOUPPER "${MOD_NAME}" _QBM_CFG_NAME_UPPER)

        set(_qbm_cmake_files)
        if(MOD_CONFIG_DEPENDENCIES)
            configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${MOD_CONFIG_DEPENDENCIES}"
                "${CMAKE_CURRENT_BINARY_DIR}/qbm-${MOD_NAME}Dependencies.cmake" @ONLY)
            list(APPEND _qbm_cmake_files
                "${CMAKE_CURRENT_BINARY_DIR}/qbm-${MOD_NAME}Dependencies.cmake")
        endif()
        foreach(_qbm_extra IN LISTS MOD_INSTALL_CMAKE_FILES)
            list(APPEND _qbm_cmake_files "${CMAKE_CURRENT_SOURCE_DIR}/${_qbm_extra}")
        endforeach()

        # SameMinorVersion, not qb's SameMajorVersion: a prebuilt static archive compiled
        # against header-heavy, inline-heavy qb does not honestly satisfy "any 3.x". The
        # generated config additionally hard-fails on qb feature skew, which a version range
        # cannot see at all -- see qbmModuleConfig.cmake.in.
        qb_install_package(
            PACKAGE "qbm-${MOD_NAME}"
            NAMESPACE qbm::
            VERSION ${_QBM_CFG_VERSION}
            COMPATIBILITY SameMinorVersion
            CONFIG_TEMPLATE "${QB_CMAKE_DIR}/qbmModuleConfig.cmake.in"
            TARGETS ${module_target} ${MOD_EXPORT_EXTRA_TARGETS}
            CMAKE_FILES ${_qbm_cmake_files}
            HEADER_EXCLUDE "${MOD_HEADER_EXCLUDE}"
            # not-qb/<unit>/ is the convention for a vendored upstream inside a qbm module
            # (today only qbm-http's llhttp). Globbing rather than naming it keeps this
            # generic: a module that vendors something later ships its notice automatically.
            VENDOR_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/not-qb/*"
        )
    endif()

    # Add to global module list.
    #
    # A GLOBAL property, because PARENT_SCOPE cannot work from here. qb_load_modules() calls
    # add_subdirectory() from INSIDE A FUNCTION, so this function's "parent scope" is that
    # function's frame -- which evaporates when qb_load_modules() returns. The variable therefore
    # never reached the caller: with all three qbm modules loaded, the configuration summary still
    # printed "Available libraries: qb-io;qb-core". The PARENT_SCOPE write below is kept for a
    # direct add_subdirectory() caller (where it does propagate one level); the property is what
    # actually survives.
    list(APPEND QB_MODULE_LIBRARIES ${module_target})
    set(QB_MODULE_LIBRARIES ${QB_MODULE_LIBRARIES} PARENT_SCOPE)
    set_property(GLOBAL APPEND PROPERTY QB_MODULE_LIBRARIES ${module_target})

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

    # Hand the accumulated list back to the CALLER (the superproject root), and report it here --
    # this is the only scope that knows which modules were actually registered. qb's own
    # configuration summary cannot: it runs during add_subdirectory(qb), which finishes before
    # qb_load_modules() is called, so "Available libraries" there is qb-core/qb-io by construction.
    get_property(_qb_loaded GLOBAL PROPERTY QB_MODULE_LIBRARIES)
    if(_qb_loaded)
        set(QB_MODULE_LIBRARIES ${_qb_loaded} PARENT_SCOPE)
        qb_status_message("Registered modules: ${_qb_loaded}")
    endif()
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
        # ONE writer per file, so nothing has to be ordered.
        #
        # bin/tests/ssl/ is only ever written here, so it is unconditional. The FLAT
        # bin/tests/{cert,key}.pem pair is different: generate_ssl_certs' POST_BUILD
        # (tests/io/system/CMakeLists.txt) writes those exact two names into that exact
        # directory whenever a host has openssl. Staging the committed pair there too made
        # them two producers of one path, and the file's content then depended on which edge
        # ninja happened to finish last -- per FILE, so a committed cert could land beside a
        # generated key, create_server_context() returns NULL, and nine SSL/TLS/QUIC tests
        # fail. That was measured once in this tree: `release` got a matching pair while
        # `relwithdebinfo` got a mismatched one.
        #
        # An add_dependencies() edge (kept below) orders the two, and MEASURING it says the
        # order holds: 20/20 clean-shape and 20/20 incremental-shape samples on macOS/ninja
        # produced the generated pair, matched, with .ninja_log showing a strict
        # copy -> openssl -> POST_BUILD chain and no overlap. But ordering only decides who
        # wins a race that still exists, and it decides it only while BOTH targets are in the
        # graph -- generate_ssl_certs is pulled in by nine SSL test targets, so a build that
        # names none of them has the committed pair as the sole writer and a later build flips
        # the file back. Conditioning the flat copy on the absence of generate_ssl_certs
        # removes the second writer instead of sequencing it: in every graph exactly one
        # target writes cert.pem and key.pem, and which one is decided at configure time.
        #
        # The committed pair remains the fallback for a host with no openssl -- which is the
        # only reason the branch exists, and why nothing here is deleted.
        set(_qb_ssl_stage_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_RESOURCES_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${QB_SSL_RESOURCES}" "${TEST_RESOURCES_DIR}/ssl")
        if(TARGET generate_ssl_certs)
            set(_qb_ssl_stage_what "ssl/ only; generate_ssl_certs owns the flat cert.pem/key.pem")
        else()
            # Tests load certificates by bare name ("cert.pem"/"key.pem") and run
            # with WORKING_DIRECTORY = the test bin dir (see qb_add_test), so also
            # stage the resources directly there. Without this the SSL tests can
            # only find the certs from a hand-copied bin/tests, otherwise they
            # silently skip (or crash, for tests that don't guard a null context).
            list(APPEND _qb_ssl_stage_commands
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QB_SSL_RESOURCES}" "${TEST_RESOURCES_DIR}")
            set(_qb_ssl_stage_what "ssl/ + the flat cert.pem/key.pem (no openssl on this host)")
        endif()

        if(NOT TARGET qb_copy_test_ssl_resources)
            add_custom_target(qb_copy_test_ssl_resources ALL
                ${_qb_ssl_stage_commands}
                COMMENT "Copying SSL resources to test directory: ${TEST_RESOURCES_DIR} (${_qb_ssl_stage_what})"
            )

            # Set folder for IDE organization
            set_target_properties(qb_copy_test_ssl_resources PROPERTIES
                FOLDER "Tests/Resources"
            )

            qb_status_message("SSL test resources will be copied to: ${TEST_RESOURCES_DIR}/ssl -- ${_qb_ssl_stage_what}")
        endif()

        # Belt to the braces above: order this copy BEFORE generate_ssl_certs' POST_BUILD.
        # With the flat copy conditioned out this edge no longer decides the CONTENT of any
        # file -- it only guarantees bin/tests/ssl/ is populated before the generated pair
        # lands beside it, so a reader that falls through to the ssl/ candidate never sees a
        # half-written directory. Do not read it as the fix for the race; the fix is that
        # there is no second writer.
        #
        # HISTORY, because the numbers here have been misquoted once already and the wrong
        # ones are load-bearing for anyone deciding whether this still matters:
        #
        #   The original defect: two unordered copy invocations, each FILE won independently
        #   under `ninja -j`. A committed cert beside a generated key is not a key pair;
        #   create_server_context() returns NULL and every test that loads them fails. Measured
        #   in a single run of this tree: `release` came out with a matching pair while
        #   `relwithdebinfo` got the committed cert next to the generated key, and nine
        #   SSL/TLS/QUIC tests failed reproducibly on "Failed to load QUIC server private key".
        #
        #   A 38-sample macOS measurement (13 clean / 25 incremental) then reported the clean
        #   build winning for the generated pair only by an RSA-keygen head start of
        #   -3ms..+60ms, and EVERY incremental rebuild producing the committed pair, 25/25.
        #   That measurement predates this add_dependencies() line and does not describe the
        #   tree it was written into. Re-measured after it, in an isolated checkout so a
        #   concurrent build could not contaminate the sample: CLEAN 20/20 generated,
        #   INCREMENTAL 20/20 generated, 0 mixed, `.ninja_log` showing a strict
        #   copy -> openssl -> POST_BUILD chain with no overlap in any of the 40. The openssl
        #   OUTPUT rule indeed never re-runs on an incremental build, but the POST_BUILD hangs
        #   off the custom TARGET, which is unconditionally dirty and IS ordered -- so the
        #   "both edges degrade to plain copies" conclusion does not follow.
        #
        #   What the ordering never fixed, and the single-writer rule does: a build whose graph
        #   contains none of the nine SSL test targets does not contain generate_ssl_certs at
        #   all, so the committed pair was the sole writer and the file flipped with build
        #   history.
        #
        # The GENERATED pair is the survivor because tests/io/shared/ssl_fixtures.h names
        # generate_ssl_certs as the source it expects -- so that is the pair the suite should
        # deterministically get. NOT because any test needs the `subjectAltName = DNS:localhost`
        # that `openssl req -addext` writes: an earlier version of this comment said
        # tls-peer-verification needs it, and that was measured false. That test asserts the
        # OPPOSITE -- that a verifying client REJECTS the self-signed server cert -- and passes
        # against the committed pair, which carries no subjectAltName at all. All nine
        # SSL/TLS/QUIC tests pass against it. Determinism is the whole justification; do not
        # re-derive a stronger one from a property no test reads.
        # The committed pair stays as the fallback for a host with no openssl.
        #
        # Placed here, not in qb/CMakeLists.txt: generate_ssl_certs is created during
        # add_subdirectory(io) and this function runs after it, so both targets exist -- and
        # the ordering lives next to the target it orders.
        if(TARGET generate_ssl_certs AND TARGET qb_copy_test_ssl_resources)
            add_dependencies(generate_ssl_certs qb_copy_test_ssl_resources)
        endif()

        # The edge qb_add_test() thinks it adds does not exist for any of qb's own tests.
        # Its guard is `TARGET qb_copy_test_ssl_resources`, and qb/CMakeLists.txt calls this
        # function at :140 -- AFTER add_subdirectory(io) at :108 and add_subdirectory(core)
        # at :111 have already registered every qb-io and qb-core test. The target does not
        # exist yet at that point, so the guard is false 324 times and only the qbm module
        # tests, registered later from the superproject root, ever got the dependency.
        # Measured, not inferred: `ninja qb-core-test-system-actor-add` into an emptied
        # bin/tests staged NOTHING, 10/10 samples -- no cert.pem, no key.pem, no ssl/. A full
        # build hides it completely because the copy target is ALL.
        # QB_TEST_TARGETS is the list qb_add_test() has been appending to all along.
        get_property(_qb_pre_registered_tests GLOBAL PROPERTY QB_TEST_TARGETS)
        foreach(_qb_pre_test IN LISTS _qb_pre_registered_tests)
            if(TARGET ${_qb_pre_test})
                add_dependencies(${_qb_pre_test} qb_copy_test_ssl_resources)
                if(TARGET generate_ssl_certs)
                    add_dependencies(${_qb_pre_test} generate_ssl_certs)
                endif()
            endif()
        endforeach()
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
    
    # PER-TARGET by default, not a directory every caller shares.
    #
    # The default used to be ${CMAKE_BINARY_DIR}/bin/resources, one path for the whole build
    # tree, and this function creates an unordered custom target per caller -- so two callers
    # taking the default are two concurrent copy_directory invocations into one directory with
    # nothing sequencing them. Both in-tree callers already had to work around it by hand and
    # said so: examples/all/taskmanager/CMakeLists.txt and .../auction_house/CMakeLists.txt
    # each pass DESTINATION "${CMAKE_BINARY_DIR}/bin/<example>/resources" with the comment
    # "so parallel builds never race (or clobber each other) on a shared bin/resources
    # directory". A default that every real caller has to override is the wrong default;
    # this makes the safe layout the one you get for free.
    if(NOT RES_DESTINATION)
        set(RES_DESTINATION "${CMAKE_BINARY_DIR}/bin/${RES_TARGET}/resources")
    endif()

    # Create custom target for copying resources
    set(copy_target "${RES_TARGET}_copy_resources")

    add_custom_target(${copy_target}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${RES_RESOURCES} ${RES_DESTINATION}
        COMMENT "Copying resources for ${RES_TARGET}"
    )

    # An explicit DESTINATION can still collide -- two callers may legitimately want to
    # populate one directory from different source trees. Serialise instead of refusing:
    # each new copy target for an already-claimed destination is ordered after the previous
    # one, so N callers become a chain rather than N racing writers. Costs nothing when the
    # destination is unique, which is now the default.
    string(MAKE_C_IDENTIFIER "QB_RESOURCE_DEST_${RES_DESTINATION}" _res_dest_key)
    get_property(_res_prev GLOBAL PROPERTY ${_res_dest_key})
    if(_res_prev)
        add_dependencies(${copy_target} ${_res_prev})
        qb_status_message(
            "qb_copy_resources: ${RES_TARGET} shares ${RES_DESTINATION} with ${_res_prev}; ordered after it")
    endif()
    set_property(GLOBAL PROPERTY ${_res_dest_key} "${copy_target}")

    # Make the main target depend on the copy target
    add_dependencies(${RES_TARGET} ${copy_target})

    qb_debug_message("Added resource copy for: ${RES_TARGET} -> ${RES_DESTINATION}")
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

# Windows runtime-DLL deployment for test/benchmark executables. Included LAST, and
# deliberately: CMake resolves a function call at invocation time, so defining it here is
# enough for qb_add_test()/qb_add_benchmark() above, and appending keeps every line number
# in this file -- which the docs cite by range -- exactly where it was.
include(${CMAKE_CURRENT_LIST_DIR}/qbRuntimeDlls.cmake)
