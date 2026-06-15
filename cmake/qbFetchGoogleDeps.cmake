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
# Include after project(), qbCompiler.cmake, qbDependencies.cmake, and qbFunctions.cmake,
# and before add_subdirectory(qb/source/io).
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

        set(_gtest_fp_args "")
        if(QB_DEPS_FETCH_FALLBACK)
            set(_gtest_fp_args FIND_PACKAGE_ARGS NAMES GTest)
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
        # Only present when gtest was built from source.
        if(TARGET gtest)
            target_compile_options(gtest PRIVATE
                $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wno-character-conversion>)
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

        set(_benchmark_fp_args "")
        if(QB_DEPS_FETCH_FALLBACK)
            set(_benchmark_fp_args FIND_PACKAGE_ARGS NAMES benchmark)
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
