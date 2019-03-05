cmake_minimum_required(VERSION 3.1)
project(cube)

set(CUBE_PREFIX ${PROJECT_NAME})
string(TOUPPER ${CUBE_PREFIX} CUBE_PREFIX_UPPER)
set(CUBE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_VERBOSE_MAKEFILE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

if(NOT CYGWIN)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(USE_TC_MALLOC "TCMalloc is ennabled" ON)
option("${CUBE_PREFIX_UPPER}_BUILD_TEST" "Build Cube Tests" OFF)
option("${CUBE_PREFIX_UPPER}_BUILD_BENCHMARK" "Build Cube Benchmarks" OFF)
option("${CUBE_PREFIX_UPPER}_BUILD_SAMPLE" "Build Cube Samples" OFF)
option("${CUBE_PREFIX_UPPER}_BUILD_DOC" "Build Cube Documentation" OFF)
option("${CUBE_PREFIX_UPPER}_WITH_RTTI" "Build with RTTI" ON)
option("${CUBE_PREFIX_UPPER}_USE_TCMALLOC" "Build with RTTI" ON)

#default build type to Release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

set(GPERFTOOLS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/gperftools/src)
get_directory_property(IS_INVOKED PARENT_DIRECTORY)

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32)
    include(cmake/CodeCoverage.cmake)
    APPEND_COVERAGE_COMPILER_FLAGS()
    set(${CUBE_PREFIX_UPPER}_COVERAGE ON)
endif()

if (${CUBE_PREFIX_UPPER}_BUILD_TEST)
    include(CTest)
    enable_testing()
    if (${CUBE_PREFIX_UPPER}_BUILD_BENCHMARK)
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

#if (WIN32)
#
#    find_library(
#            TCMALLOC_RELEASE
#            NAMES libtcmalloc_minimal
#            PATHS ${CMAKE_CURRENT_SOURCE_DIR}/external/tcmalloc/win32/release
#    )
#
#    find_library(
#            TCMALLOC_DEBUG
#            NAMES libtcmalloc_minimal-debug
#            PATHS ${CMAKE_CURRENT_SOURCE_DIR}/external/tcmalloc/win32/debug
#    )
#
#    if (TCMALLOC_RELEASE)
#        message(STATUS "INFO: will use tcmalloc")
#        if (GNU)
#            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
#        elseif (MSVC)
#            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /INCLUDE:\"__tcmalloc\"")
#        endif()
#    elseif (USE_TC_MALLOC)
#        message(STATUS "INFO: tcmalloc not found")
#        set(USE_TC_MALLOC OFF)
#    endif()
#
#elseif(UNIX)
#    set(USE_TC_MALLOC OFF)
#endif()

add_subdirectory(source/network)

#TCMALLOC only available on linux for the moment
if (NOT WIN32 AND ${CUBE_PREFIX_UPPER}_USE_TCMALLOC)
    message(STATUS "INFO: will use tcmalloc library")
    set(GPERFTOOLS_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    find_package(Gperftools)
    if (GPERFTOOLS_FOUND)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -lunwind")
        link_libraries(${GPERFTOOLS_TCMALLOC})
    endif()
endif()

add_subdirectory(source/engine)

# Sample Projects
if (${CUBE_PREFIX_UPPER}_BUILD_SAMPLE)
    add_subdirectory(sample)
endif()

# Generate Documentation
add_subdirectory(docs)

if(IS_INVOKED)
    if(${CUBE_PREFIX_UPPER}_WITH_RTTI)
        message("Will build ${CUBE_PREFIX} libraries with rtti")
    else()
        message("Will build ${CUBE_PREFIX} libraries without rtti")
        set(CMAKE_CXX_FLAGS "${cxx_no_rtti_flags}" PARENT_SCOPE)
    endif()
endif()