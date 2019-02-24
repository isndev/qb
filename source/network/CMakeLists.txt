project(${CMAKE_PROJECT_NAME}-network VERSION 1.0.0.0 DESCRIPTION "A simple network library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} src/network-all.cpp)
cxx_shared_library(${PROJECT_NAME}-shared ${cxx_default_lib} src/network-all.cpp)

if (WIN32)
    target_link_libraries(${PROJECT_NAME} ws2_32)
    target_link_libraries(${PROJECT_NAME}-shared ws2_32)
endif()

#if (COMPILE_CUBE_TESTS)
    add_subdirectory(tests)
#endif()