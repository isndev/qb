#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Resolves GoogleTest and Google Benchmark before any test/benchmark targets are
# defined.
#
# Resolution policy (per dependency):
#   QB_USE_SYSTEM_GTEST / QB_USE_SYSTEM_BENCHMARK = ON
#       -> find_package(... CONFIG REQUIRED): require a system package, never fetch.
#   else, QB_DEPS_FETCH_FALLBACK = ON (default)
#       -> FetchContent with FIND_PACKAGE_ARGS: use the system package if present,
#          otherwise build the pinned tag from source ("system if present, else git").
#   else, QB_DEPS_FETCH_FALLBACK = OFF
#       -> always build the pinned tag from source (reproducible, ignores the system).
#
# FIND_PACKAGE_ARGS requires CMake >= 3.24 (the framework's minimum).
#
# Both FIND_PACKAGE_ARGS lists below spell QUIET and GLOBAL out explicitly. That is not
# style -- it is what keeps this file configurable on CMake 3.24 through 3.28, i.e. on
# every CMake older than 3.29, which includes the stock cmake of Ubuntu 24.04 LTS (3.28.3),
# Debian 12 (3.25.1) and RHEL 9 (3.26.5). See the note above each list; do not remove them.
#
# Include after project(), qbCompiler.cmake, qbDependencies.cmake, and qbFunctions.cmake,
# and before add_subdirectory(qb/src/qb/io).
# -----------------------------------------------------------------------------

if(QB_FETCH_GOOGLE_DEPS_INCLUDED)
    return()
endif()
set(QB_FETCH_GOOGLE_DEPS_INCLUDED TRUE)

if(NOT QB_BUILD_TESTS AND NOT QB_BUILD_BENCHMARKS)
    set(QB_HAS_GTEST FALSE)
    set(QB_HAS_BENCHMARK FALSE)
    return()
endif()

include(FetchContent)

# When GTest/benchmark resolve to a *system* package (find_package, e.g. a
# locally-installed GoogleTest), the imported targets (GTest::gtest_main, ...)
# are created in THIS directory scope and are NOT visible to sibling module
# subdirectories (qbm/*), which are added from the project root. That makes
# qb_add_test's `if(TARGET GTest::gtest_main)` false inside modules and fails
# with "no GTest target". Promote find_package imported targets to GLOBAL so
# module tests can see and link them. (CMAKE_FIND_PACKAGE_TARGETS_GLOBAL: CMake >= 3.24)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

# -----------------------------------------------------------------------------
# GoogleTest
# -----------------------------------------------------------------------------
if(QB_BUILD_TESTS)
    if(QB_USE_SYSTEM_GTEST)
        find_package(GTest CONFIG REQUIRED)
        qb_status_message("Google Test: system package (QB_USE_SYSTEM_GTEST=ON)")
    else()
        if(MSVC)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        endif()
        set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_build_tests OFF CACHE BOOL "" FORCE)
        set(gmock_build_tests OFF CACHE BOOL "" FORCE)

        # QUIET and GLOBAL are spelled out on purpose -- omitting them makes CMake < 3.29
        # emit a SYNTAX ERROR from inside its own FetchContent module, before any download:
        #
        #   CMake Error at .../Modules/FetchContent.cmake:1202:EVAL:1:
        #     Syntax Error in cmake code at column 107
        #     Argument not separated from preceding token by whitespace.
        #
        # Mechanism (FetchContent.cmake, 3.24.0 - 3.28.x). __FetchContent_declareDetails
        # accumulates the post-FIND_PACKAGE_ARGS items into __findPackageArgs as a
        # SPACE-separated string of bracket-quoted tokens (" [==[NAMES]==] [==[GTest]==]"),
        # then re-serialises it through cmake_language(EVAL). But it adds the two implicit
        # keywords with LIST commands, which join with ';' and not with a space:
        #     if(NOT __sawQuietKeyword)
        #       list(INSERT __findPackageArgs 0 QUIET)
        #     if(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL AND NOT __sawGlobalKeyword)
        #       list(APPEND __findPackageArgs GLOBAL)     <-- produces "...]==];GLOBAL"
        # so the evaluated line ends `[==[GTest]==];GLOBAL)`, and a bracket argument that is
        # not followed by whitespace is a parse error. Column 107 is exactly where the ';'
        # lands for the name `googletest` (116 for `googlebenchmark`). CMake 3.29.0 fixed it
        # by switching both to string(PREPEND/APPEND) -- see its FetchContent.cmake.
        #
        # It needs BOTH ingredients, which is why it stayed invisible: FIND_PACKAGE_ARGS with
        # at least one trailing argument, AND CMAKE_FIND_PACKAGE_TARGETS_GLOBAL true -- which
        # this file sets a few lines above, for a reason unrelated to any of this.
        #
        # Passing the keywords ourselves sets __sawQuietKeyword/__sawGlobalKeyword, so neither
        # list() call runs and the string stays well-formed. The resulting find_package() call
        # is byte-for-byte the one CMake would have built anyway, so this changes no behaviour
        # on 3.29+; it only stops 3.24-3.28 from corrupting it. Verified configuring on
        # 3.24.4 / 3.25.3 / 3.26.6 / 3.27.9 / 3.28.6 / 3.29.0 / 3.30.9 / 3.31.9 / 4.0.3 / 4.1.2.
        set(_gtest_fp_args "")
        if(QB_DEPS_FETCH_FALLBACK)
            set(_gtest_fp_args FIND_PACKAGE_ARGS QUIET GLOBAL NAMES GTest)
        endif()

        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG ${QB_GOOGLETEST_GIT_TAG}
            GIT_SHALLOW TRUE
            ${_gtest_fp_args}
        )
        FetchContent_MakeAvailable(googletest)

        if(DEFINED googletest_SOURCE_DIR)
            qb_status_message("Google Test: built from source (tag ${QB_GOOGLETEST_GIT_TAG})")
        else()
            qb_status_message("Google Test: system package (found via FIND_PACKAGE_ARGS)")
        endif()

        # Clang -Wcharacter-conversion on char8_t printing in gtest-printers.h (third-party).
        # Only present when gtest was built from source. The flag itself only exists on
        # newer Clang/AppleClang, so pair it with -Wno-unknown-warning-option (scoped to
        # this third-party target) to stay silent on Clang versions that lack it.
        if(TARGET gtest)
            target_compile_options(gtest PRIVATE
                $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wno-unknown-warning-option;-Wno-character-conversion>)
        endif()
    endif()

    set(QB_HAS_GTEST TRUE)
    include(GoogleTest)
endif()

# -----------------------------------------------------------------------------
# Google Benchmark
# -----------------------------------------------------------------------------
if(QB_BUILD_BENCHMARKS)
    if(QB_USE_SYSTEM_BENCHMARK)
        find_package(benchmark CONFIG REQUIRED)
        qb_status_message("Google Benchmark: system package (QB_USE_SYSTEM_BENCHMARK=ON)")
    else()
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

        # QUIET and GLOBAL spelled out for the same reason as GoogleTest above -- without them
        # CMake 3.24-3.28 fails to parse its own generated code (column 116 here, because the
        # offending ';' lands after the longer content name `googlebenchmark`). Full mechanism
        # in the GoogleTest block; do not "simplify" this back.
        set(_benchmark_fp_args "")
        if(QB_DEPS_FETCH_FALLBACK)
            set(_benchmark_fp_args FIND_PACKAGE_ARGS QUIET GLOBAL NAMES benchmark)
        endif()

        FetchContent_Declare(
            googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG ${QB_GOOGLEBENCHMARK_GIT_TAG}
            GIT_SHALLOW TRUE
            ${_benchmark_fp_args}
        )
        FetchContent_MakeAvailable(googlebenchmark)

        if(DEFINED googlebenchmark_SOURCE_DIR)
            qb_status_message("Google Benchmark: built from source (tag ${QB_GOOGLEBENCHMARK_GIT_TAG})")
        else()
            qb_status_message("Google Benchmark: system package (found via FIND_PACKAGE_ARGS)")
        endif()
    endif()

    set(QB_HAS_BENCHMARK TRUE)
endif()
