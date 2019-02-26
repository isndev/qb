cmake_minimum_required(VERSION 3.1)
project(cube)

string(TOUPPER ${CMAKE_PROJECT_NAME} PROJECT_PREFIX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_VERBOSE_MAKEFILE ON)
if(NOT CYGWIN)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(USE_TC_MALLOC "TCMalloc is ennabled" ON)
option("${PROJECT_PREFIX}_COMPILE_TESTS" "COMPILE_TESTS" OFF)


set(GPERFTOOLS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/gperftools/src)
get_directory_property(IS_INVOKED PARENT_DIRECTORY)

include(CTest)
enable_testing()

# add google benchmark module
set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GOOGLEBENCHMARK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/googlebenchmark)
add_subdirectory(${GOOGLEBENCHMARK_DIR})
# add google test module
if (MSVC)
    set(gtest_force_shared_crt ON)
endif()
set(GOOGLETEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/googlebenchmark/googletest)
add_subdirectory(${GOOGLETEST_DIR})

# overload with our cmake utils
include(cmake/utils.cmake)
config_compiler_and_linker()  # Defined in internal_utils.cmake.

#include_directories(${GPERFTOOLS_INCLUDE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/modules)
#include_directories(modules/nanolog)

#if (WIN32)
#    if (MSVC)
#        set(${CMAKE_CXX_FLAGS} "${CMAKE_CXX_FLAGS} /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS -MT")
##        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR- /Zc:__cplusplus /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS")
#    elseif(GNU)
#    endif()
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

add_subdirectory(source/engine)
add_subdirectory(source/network)

# Sample Projects
add_subdirectory(sample)

# Build Documentation
add_subdirectory(docs)

if(IS_INVOKED)
    set(CUBE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CUBE_CXX_FLAGS}" PARENT_SCOPE)
endif()