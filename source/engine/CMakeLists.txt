project(${CUBE_PREFIX}-engine LANGUAGES CXX VERSION 1.0.0.0 DESCRIPTION "A simple actor library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} src/engine-all.cpp)
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${CUBE_DIRECTORY}/include;${CUBE_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (${CUBE_PREFIX_UPPER}_BUILD_TEST)
    add_subdirectory(tests)
endif()