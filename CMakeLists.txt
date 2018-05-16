cmake_minimum_required(VERSION 3.10)
project(cube)

set(CMAKE_CXX_STANDARD 17)

include_directories(./)
include_directories(cube-linq/include)
include_directories(NanoLog)

if (WIN32)
	if (MSVC)
    		SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /DMSVC /D_SCL_SECURE_NO_WARNINGS")
	endif()
endif()

if (UNIX)
endif()


add_executable(actor-mock
        test/main.cpp system/debug.cpp NanoLog/NanoLog.cpp)