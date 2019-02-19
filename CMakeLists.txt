cmake_minimum_required(VERSION 3.10)
project(cube)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE OFF)
option(USE_TC_MALLOC "TCMalloc is ennabled" ON)
option(COMPILE_TESTS "COMPILE_TESTS")
option(NOLOG "NOLOG")
option(NOCOUT "NOCOUT")


set(GPERFTOOLS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/gperftools/src)
get_directory_property(IS_INVOKED PARENT_DIRECTORY)


include_directories(${GPERFTOOLS_INCLUDE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/nanolog)


if (WIN32)
    if (MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR- /Zc:__cplusplus /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS")
    elseif(GNU)
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
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
        elseif (MSVC)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /INCLUDE:\"__tcmalloc\"")
        endif()
    elseif (USE_TC_MALLOC)
        message(STATUS "INFO: tcmalloc not found")
        set(USE_TC_MALLOC OFF)
    endif()

elseif(UNIX)
    set(USE_TC_MALLOC OFF)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -fno-rtti")
endif()

set(IOFILES ${CMAKE_CURRENT_SOURCE_DIR}/nanolog/nanolog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/system/io.cpp)

if (CUBE_TESTS)
    add_subdirectory(test)
endif()

set(CUBE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/system/actor/ActorId.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/system/actor/Actor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/system/actor/Core.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/system/actor/CoreSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/system/actor/Cube.cpp)

add_library(cube STATIC ${CUBE_SOURCE} ${IOFILES})
add_subdirectory(network)

if(IS_INVOKED)
    set(CUBE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CUBE_CXX_FLAGS}" PARENT_SCOPE)
endif()