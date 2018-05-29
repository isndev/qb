cmake_minimum_required(VERSION 3.10)
project(cube)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE OFF)
option(USE_TC_MALLOC "TCMalloc is ennabled" ON)
option(COMPILE_TESTS "COMPILE_TESTS" OFF)
option(NOLOG "NOLOG" OFF)
option(NOCOUT "NOCOUT" ON)


set(GPERFTOOLS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/gperftools/src)
get_directory_property(IS_INVOKED PARENT_DIRECTORY)

include_directories(${GPERFTOOLS_INCLUDE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/nanolog)



if (WIN32)
    if (MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS")
        if (NOLOG)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /DNLOG")
        endif()
    elseif(GNU)
        if (NOLOG)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNLOG")
        endif()
    endif()

    find_library(
            TCMALLOC_RELEASE
            NAMES libtcmalloc_minimal
            PATHS ${CMAKE_CURRENT_SOURCE_DIR}/external/tcmalloc/win32/release
    )

    find_library(
            TCMALLOC_DEBUG
            NAMES libtcmalloc_minimal-debug
            PATHS ${CMAKE_CURRENT_SOURCE_DIR}/external/tcmalloc/win32/debug
    )

    if (TCMALLOC_RELEASE)
        message(STATUS "INFO: will use tcmalloc")
        if (GNU)
            SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
        elseif (MSVC)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /INCLUDE:\"__tcmalloc\"")
        endif()
    elseif (USE_TC_MALLOC)
        message(STATUS "INFO: tcmalloc not found")
        set(USE_TC_MALLOC OFF)
    endif()

elseif(UNIX)

endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEBUGFILES ${CMAKE_CURRENT_SOURCE_DIR}/system/debug.cpp)
    message(STATUS "Added debug source files")
else()
endif()

if (NOLOG)
else()
    set(LOGFILES ${CMAKE_CURRENT_SOURCE_DIR}/nanolog/nanolog.cpp)
    message(STATUS "Added log source files")
endif()

if (COMPILE_TESTS)
    add_subdirectory(test)
endif()

add_library(cube STATIC ${DEBUGFILES} ${LOGFILES})

if(IS_INVOKED)
    set(CUBE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CUBE_CXX_FLAGS}" PARENT_SCOPE)
endif()