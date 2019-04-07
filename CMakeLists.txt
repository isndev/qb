#
# qb - C++ Actor Framework
# Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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
#         limitations under the License.
#

cmake_minimum_required(VERSION 3.1)
project(qb LANGUAGES CXX DESCRIPTION "C++17 Actor Framework")

#set global variable
set(QB_PREFIX ${PROJECT_NAME})
string(TOUPPER ${QB_PREFIX} QB_PREFIX_UPPER)
set(QB_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set(QB_VERSION_MAJOR 0)
set(QB_VERSION_MINOR 9)
set(QB_VERSION_PATCH 0)
set(QB_VERSION_TWEAK 0)
set(QB_VERSION "${QB_VERSION_MAJOR}.${QB_VERSION_MINOR}.${QB_VERSION_PATCH}.${QB_VERSION_TWEAK}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(${QB_PREFIX_UPPER}_CXX_FLAGS "")
#set(CMAKE_VERBOSE_MAKEFILE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

if(NOT CYGWIN)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(QB_PATH "root" ${CMAKE_CURRENT_SOURCE_DIR})
option("${QB_PREFIX_UPPER}_BUILD_TEST" "Build Cube Tests" OFF)
option("${QB_PREFIX_UPPER}_BUILD_BENCHMARK" "Build Cube Benchmarks" OFF)
option("${QB_PREFIX_UPPER}_BUILD_SAMPLE" "Build Cube Samples" OFF)
option("${QB_PREFIX_UPPER}_BUILD_DOC" "Build Cube Documentation" OFF)
option("${QB_PREFIX_UPPER}_WITH_LOG" "Enable nanolog" ON)
option("${QB_PREFIX_UPPER}_WITH_RTTI" "Build with RTTI" ON)
option("${QB_PREFIX_UPPER}_WITH_TCMALLOC" "TCMalloc is ennabled" OFF)

#default build type to Release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

if (${QB_PREFIX_UPPER}_BUILD_TEST)
    include(CTest)
    enable_testing()
    if (${QB_PREFIX_UPPER}_BUILD_BENCHMARK)
        # add google benchmark module
        set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(GOOGLEBENCHMARK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/googlebenchmark)
        add_subdirectory(${GOOGLEBENCHMARK_DIR})
    endif()
    if (MSVC)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    endif()
    # add google test module
    set(GOOGLETEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/googletest)
    add_subdirectory(${GOOGLETEST_DIR})
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32)
    message(STATUS "INFO: Added coverage target to the solution")
    if (${QB_PREFIX_UPPER}_BUILD_COVERAGE)
        include(cmake/CodeCoverage.cmake)
        APPEND_COVERAGE_COMPILER_FLAGS()
        if (LCOV_PATH AND GCOV_PATH AND Python_FOUND)
            set(${QB_PREFIX_UPPER}_COVERAGE ON)
        endif()
    endif()
endif()

# overload with our cmake utils
include(cmake/utils.cmake)
config_compiler_and_linker()  # Defined in internal_utils.cmake.

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/modules)

# Network Projects
add_subdirectory(source/network)

#TCMALLOC only available on linux for the moment
if (NOT WIN32 AND ${QB_PREFIX_UPPER}_USE_TCMALLOC)
    set(GPERFTOOLS_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    find_package(Gperftools)
    if (GPERFTOOLS_FOUND)
        message(STATUS "INFO: Will build ${QB_PREFIX} libraries with tcmalloc")
            if (GNU)
                set(${QB_PREFIX_UPPER}_CXX_FLAGS "-flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
            endif ()
        link_libraries(${name} unwind)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${${QB_PREFIX_UPPER}_CXX_FLAGS}")
        link_libraries(${GPERFTOOLS_TCMALLOC})
    endif()
endif()

# Cube Main Library
add_subdirectory(source/core)

# Generate Documentation
add_subdirectory(docs)

if(${QB_PREFIX_UPPER}_WITH_RTTI)
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries with RTTI")
else()
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries without RTTI")
    set(${QB_PREFIX_UPPER}_CXX_FLAGS "${${QB_PREFIX_UPPER}_CXX_FLAGS} ${cxx_no_rtti_flags}")
endif()

if(QB_MODULE_PATH)
    qb_load_modules(${QB_MODULE_PATH})
endif()

if (${QB_PREFIX_UPPER}_COVERAGE)
    set(COVERAGE_LCOV_EXCLUDES
            '/usr/*'
            '*benchmark*'
            '*modules*'
            '*sample/*'
            )
    SETUP_TARGET_FOR_COVERAGE_LCOV(NAME ${PROJECT_NAME}-coverage
            EXECUTABLE ctest
            DEPENDENCIES qb-core)
    set(COVERAGE_GCOVR_EXCLUDES
            ".*benchmark.*"
            ".*modules.*"
            ".*sample.*"
            )
    SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(NAME ${PROJECT_NAME}-coverage-xml
            EXECUTABLE ctest
            DEPENDENCIES qb-core)
endif()

# Sample Projects
if (${QB_PREFIX_UPPER}_BUILD_SAMPLE)
    add_subdirectory(sample)
endif()

# share some variable to parent
get_directory_property(hasParent PARENT_DIRECTORY)
if(hasParent)
    set(CMAKE_CXX_FLAGS "${${QB_PREFIX_UPPER}_CXX_FLAGS}" PARENT_SCOPE)
    set(QB_PREFIX "${QB_PREFIX}" PARENT_SCOPE)
    set(QB_DIRECTORY "${QB_DIRECTORY}" PARENT_SCOPE)
endif()