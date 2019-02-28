project(${CUBE_PREFIX}-network LANGUAGES CXX VERSION 1.0.0.0 DESCRIPTION "A simple network library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} src/network-all.cpp)
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${CUBE_DIRECTORY}/include;${CUBE_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

cxx_shared_library(${PROJECT_NAME}-shared ${cxx_default_lib} src/network-all.cpp)
target_include_directories(${PROJECT_NAME}-shared INTERFACE
        "$<BUILD_INTERFACE:${CUBE_DIRECTORY}/include;${CUBE_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (WIN32)
    target_link_libraries(${PROJECT_NAME} ws2_32)
    target_link_libraries(${PROJECT_NAME}-shared ws2_32)
endif()

#if (COMPILE_CUBE_TESTS)
    add_subdirectory(tests)
#endif()