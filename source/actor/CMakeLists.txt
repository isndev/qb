project(${CMAKE_PROJECT_NAME}-actor VERSION 1.0.0.0 DESCRIPTION "A simple actor library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} src/actor-all.cpp)

#if (COMPILE_CUBE_TESTS)
    add_subdirectory(tests)
#endif()