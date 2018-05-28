cmake_minimum_required(VERSION 3.10)
project(cube)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

set(GPERFTOOLS_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/modules/gperftools/src)
set(USE_TC_MALLOC ON)

include_directories(${GPERFTOOLS_INCLUDE_DIR})
include_directories(./)
include_directories(nanolog)
include_directories(test)

if (WIN32)
    if (MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS")
    elseif(GNU)
    endif()

    find_library(
            TCMALLOC_RELEASE
            NAMES libtcmalloc_minimal
            PATHS ${CMAKE_CURRENT_LIST_DIR}/external/tcmalloc/win32/release
    )

    find_library(
            TCMALLOC_DEBUG
            NAMES libtcmalloc_minimal-debug
            PATHS ${CMAKE_CURRENT_LIST_DIR}/external/tcmalloc/win32/debug
    )

    if (TCMALLOC_RELEASE-NOTFOUND)
        message(STATUS "INFO: tcmalloc not found")
        set(USE_TC_MALLOC OFF)
    elseif (USE_TC_MALLOC)
        if (GNU)
            SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
        elseif (MSVC)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /INCLUDE:\"__tcmalloc\"")
        endif()
    endif()

elseif(UNIX)

endif()

add_executable(test-actor
		test/actor/actor.cpp system/debug.cpp NanoLog/nanolog.cpp)
add_executable(test-chain-actor
        test/benchmark/chain_event.cpp system/debug.cpp nanolog/nanolog.cpp)
add_executable(test-linked-core
        test/benchmark/linked_core.cpp system/debug.cpp nanolog/nanolog.cpp)
add_executable(test-core-shared_data
        test/benchmark/shared_data.cpp system/debug.cpp nanolog/nanolog.cpp)
add_executable(test-TComposition
        test/utils/TComposition.cpp)
add_executable(test-timestamp
        test/utils/timstamp.cpp)

if (USE_TC_MALLOC)
target_link_libraries(test-actor
        debug ${TCMALLOC_DEBUG}
        optimized ${TCMALLOC_RELEASE})
target_link_libraries(test-chain-actor
        debug ${TCMALLOC_DEBUG}
        optimized ${TCMALLOC_RELEASE})
target_link_libraries(test-linked-core
        debug ${TCMALLOC_DEBUG}
        optimized ${TCMALLOC_RELEASE})
target_link_libraries(test-core-shared_data
        debug ${TCMALLOC_DEBUG}
        optimized ${TCMALLOC_RELEASE})
endif()

