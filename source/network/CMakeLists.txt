project(${QB_PREFIX}-network LANGUAGES CXX VERSION 1.0.0.0 DESCRIPTION "A simple network library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} src/network-all.cpp)
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

cxx_shared_library(${PROJECT_NAME}-shared ${cxx_default_lib} src/network-all.cpp)
target_include_directories(${PROJECT_NAME}-shared INTERFACE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (WIN32)
    target_link_libraries(${PROJECT_NAME} ws2_32)
    target_link_libraries(${PROJECT_NAME}-shared ws2_32)
endif()

if (${QB_PREFIX_UPPER}_BUILD_TEST)
    add_subdirectory(tests)
endif()