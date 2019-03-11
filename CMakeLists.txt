cmake_minimum_required(VERSION 3.1)
project(qb)

set(QB_PREFIX ${PROJECT_NAME})
string(TOUPPER ${QB_PREFIX} QB_PREFIX_UPPER)
set(QB_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(${QB_PREFIX_UPPER}_CXX_FLAGS "")
#set(CMAKE_VERBOSE_MAKEFILE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

if(NOT CYGWIN)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(USE_TC_MALLOC "TCMalloc is ennabled" ON)
option("${QB_PREFIX_UPPER}_BUILD_TEST" "Build Cube Tests" OFF)
option("${QB_PREFIX_UPPER}_BUILD_BENCHMARK" "Build Cube Benchmarks" OFF)
option("${QB_PREFIX_UPPER}_BUILD_SAMPLE" "Build Cube Samples" OFF)
option("${QB_PREFIX_UPPER}_BUILD_DOC" "Build Cube Documentation" OFF)
option("${QB_PREFIX_UPPER}_WITH_RTTI" "Build with RTTI" ON)
option("${QB_PREFIX_UPPER}_USE_TCMALLOC" "Build with RTTI" ON)

#default build type to Release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32)
    include(cmake/CodeCoverage.cmake)
    APPEND_COVERAGE_COMPILER_FLAGS()
    set(${QB_PREFIX_UPPER}_COVERAGE ON)
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

# overload with our cmake utils
include(cmake/utils.cmake)
config_compiler_and_linker()  # Defined in internal_utils.cmake.

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/modules)

# Network Projects
add_subdirectory(source/network)

#TCMALLOC only available on linux for the moment
if (NOT WIN32 AND ${QB_PREFIX_UPPER}_USE_TCMALLOC)
    message(STATUS "INFO: will use tcmalloc library")
    set(GPERFTOOLS_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    find_package(Gperftools)
    if (GPERFTOOLS_FOUND)
        set(${QB_PREFIX_UPPER}_CXX_FLAGS "-flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -lunwind")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${${QB_PREFIX_UPPER}_CXX_FLAGS}")
        link_libraries(${GPERFTOOLS_TCMALLOC})
    endif()
endif()

# Cube Main Library
add_subdirectory(source/core)

# Sample Projects
if (${QB_PREFIX_UPPER}_BUILD_SAMPLE)
    add_subdirectory(sample)
endif()

# Generate Documentation
add_subdirectory(docs)

if(${QB_PREFIX_UPPER}_WITH_RTTI)
    message(STATUS "Will build ${QB_PREFIX} libraries with rtti")
else()
    message(STATUS "Will build ${QB_PREFIX} libraries without rtti")
    set(${QB_PREFIX_UPPER}_CXX_FLAGS "${${QB_PREFIX_UPPER}_CXX_FLAGS} ${cxx_no_rtti_flags}")
endif()

if(QB_MODULE_PATH)
    qb_load_modules(${QB_MODULE_PATH})
endif()

if (${QB_PREFIX_UPPER}_COVERAGE)
    set(COVERAGE_LCOV_EXCLUDES
            '/usr/*'
            '*benchmark*'
            '*google*'
            '*qb/system/*'
            '*qb/network/*'
            '*sample*'
            '*modules*'
            )
    SETUP_TARGET_FOR_COVERAGE_LCOV(NAME ${PROJECT_NAME}-coverage
            EXECUTABLE ctest
            DEPENDENCIES qb-core)
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
endif()
