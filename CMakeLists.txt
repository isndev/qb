cmake_minimum_required(VERSION 3.10)
project(cube)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/gperftools.cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS")
endif()

include_directories(./)
include_directories(nanolog)
include_directories(test)

#if (GNU)
#	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
#endif()
#
#if (WIN32)
#	if (MSVC)
#    		SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D_SCL_SECURE_NO_WARNINGS")
#	endif()
#endif()
#
#if (UNIX)
#endif()

add_executable(test-actor
		test/actor/actor.cpp system/debug.cpp NanoLog/nanolog.cpp)
add_executable(test-linked-core
		test/benchmark/linked_core.cpp system/debug.cpp nanolog/nanolog.cpp)
add_executable(test-shared-data
		test/benchmark/shared_data.cpp system/debug.cpp nanolog/nanolog.cpp)
add_executable(test-TComposition
		test/utils/TComposition.cpp)
add_executable(test-timestamp
		test/utils/timstamp.cpp)