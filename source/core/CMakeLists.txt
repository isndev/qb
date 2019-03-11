project(${QB_PREFIX}-core LANGUAGES CXX VERSION 1.0.0.0 DESCRIPTION "A simple actor library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} core.cpp)
message(STATUS "ON LIB QB_DIR=${QB_DIRECTORY}")
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (${QB_PREFIX_UPPER}_BUILD_TEST)
    add_subdirectory(tests)
endif()