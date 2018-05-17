cmake_minimum_required(VERSION 3.10)
project(cube)

set(CMAKE_CXX_STANDARD 17)

include_directories(./)
include_directories(cube-linq/include)
include_directories(NanoLog)

if (GNU)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
endif()

if (WIN32)
	if (MSVC)
    		SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D_SCL_SECURE_NO_WARNINGS")
	endif()
endif()

if (UNIX)
endif()


add_executable(test-linked-core
		test/benchmark/linked_core.cpp system/debug.cpp NanoLog/nanolog.cpp)
add_executable(test-shared-data
		test/benchmark/shared_data.cpp system/debug.cpp NanoLog/nanolog.cpp)