#
# qb - C++ Actor Framework
# Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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
set(QB_VERSION_MAJOR 1)
set(QB_VERSION_MINOR 8)
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

set(QB_PATH ${CMAKE_CURRENT_SOURCE_DIR})
option(${QB_PREFIX_UPPER}_BUILD_TEST "Build qb Tests" OFF)
option(${QB_PREFIX_UPPER}_BUILD_BENCHMARK "Build qb Benchmarks" OFF)
option(${QB_PREFIX_UPPER}_BUILD_DOC "Build qb Documentation" OFF)
option(${QB_PREFIX_UPPER}_WITH_LOG "Enable nanolog" ON)
option(${QB_PREFIX_UPPER}_WITH_TCMALLOC "TCMalloc is ennabled" OFF)

if (NOT ${QB_PREFIX_UPPER}_BUILD_ARCH)
    set(${QB_PREFIX_UPPER}_BUILD_ARCH native)
endif()

#default build type to Release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

# check if has module googletest
if (${QB_PREFIX_UPPER}_BUILD_TEST AND NOT EXISTS "${QB_PATH}/modules/googletest/CMakeLists.txt")
    set(${QB_PREFIX_UPPER}_BUILD_TEST OFF)
    message(WARNING: Cannot build tests googletest module not loaded, try -> git submodule update --init --recursive)
endif()
# check if has module googlebenchmark
if (${QB_PREFIX_UPPER}_BUILD_BENCHMARK AND NOT EXISTS "${QB_PATH}/modules/googlebenchmark/CMakeLists.txt")
    set(${QB_PREFIX_UPPER}_BUILD_TEST OFF)
    message(WARNING: Cannot build benchmarks googlebenchmark module not loaded, try -> git submodule update --init --recursive)
endif()

set(BUILD_TESTING OFF)
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

# (optional) some modules require openssl
find_package(OpenSSL)
if(OPENSSL_FOUND)
    #used for tests
    set(${QB_PREFIX_UPPER}_SSL_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ressources/ssl/key.pem ${CMAKE_CURRENT_SOURCE_DIR}/ressources/ssl/cert.pem)
else()
    message(INFO: OPENSSL not found, some modules require libssl, they will be skipped from compilation)
endif()

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

# qb io libraries
add_subdirectory(modules/ev)
add_subdirectory(source/io)
# qb core library
add_subdirectory(source/core)
# Generate Documentation
add_subdirectory(docs)

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

# share some variable to parent
get_directory_property(hasParent PARENT_DIRECTORY)
if(hasParent)
    set(CMAKE_CXX_FLAGS "${${QB_PREFIX_UPPER}_CXX_FLAGS}" PARENT_SCOPE)
    set(QB_PREFIX "${QB_PREFIX}" PARENT_SCOPE)
    set(QB_DIRECTORY "${QB_DIRECTORY}" PARENT_SCOPE)
    set(OPENSSL_FOUND "${OPENSSL_FOUND}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_BUILD_TEST "${${QB_PREFIX_UPPER}_BUILD_TEST}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_BUILD_BENCHMARK "${${QB_PREFIX_UPPER}_BUILD_BENCHMARK}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_BUILD_COVERAGE "${${QB_PREFIX_UPPER}_BUILD_COVERAGE}" PARENT_SCOPE)
    set(${QB_PREFIX_UPPER}_SSL_RESOURCES "${${QB_PREFIX_UPPER}_SSL_RESOURCES}" PARENT_SCOPE)
endif()