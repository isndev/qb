#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Resolves GoogleTest and Google Benchmark before any test/benchmark targets are
# defined. Default: CMake FetchContent with pinned tags (reproducible, no git submodules).
# Optional: QB_USE_SYSTEM_GTEST / QB_USE_SYSTEM_BENCHMARK for find_package(CONFIG).
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

if(QB_BUILD_TESTS)
    if(QB_USE_SYSTEM_GTEST)
        find_package(GTest CONFIG REQUIRED)
        set(QB_HAS_GTEST TRUE)
        qb_status_message("Google Test: system package (find_package CONFIG)")
    else()
        if(MSVC)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        endif()
        set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_build_tests OFF CACHE BOOL "" FORCE)
        set(gmock_build_tests OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG ${QB_GOOGLETEST_GIT_TAG}
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(googletest)
        set(QB_HAS_GTEST TRUE)
        qb_status_message("Google Test: FetchContent (tag ${QB_GOOGLETEST_GIT_TAG})")
    endif()

    include(GoogleTest)
endif()

if(QB_BUILD_BENCHMARKS)
    if(QB_USE_SYSTEM_BENCHMARK)
        find_package(benchmark CONFIG REQUIRED)
        set(QB_HAS_BENCHMARK TRUE)
        qb_status_message("Google Benchmark: system package (find_package CONFIG)")
    else()
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
            googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG ${QB_GOOGLEBENCHMARK_GIT_TAG}
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(googlebenchmark)
        set(QB_HAS_BENCHMARK TRUE)
        qb_status_message("Google Benchmark: FetchContent (tag ${QB_GOOGLEBENCHMARK_GIT_TAG})")
    endif()
endif()
