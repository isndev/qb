project(cube-network VERSION 1.0.0 DESCRIPTION "A simple network helper library")

set(CUBE_NETWORK_PATH "${CMAKE_CURRENT_SOURCE_DIR}")

include_directories(${CUBE_NETWORK_PATH})

set(NET_SOURCE
        ${CUBE_NETWORK_PATH}/src/ip.cpp
        ${CUBE_NETWORK_PATH}/src/Socket.cpp
        ${CUBE_NETWORK_PATH}/src/SocketTCP.cpp
        ${CUBE_NETWORK_PATH}/src/SocketUDP.cpp
        ${CUBE_NETWORK_PATH}/src/Listener.cpp)

add_library(cube-network-shared SHARED ${NET_SOURCE})
add_library(cube-network STATIC ${NET_SOURCE})

if (WIN32)
    target_compile_definitions(cube-network-shared PRIVATE CUBE_DYNAMIC=1)
    target_compile_definitions(cube-network-shared PRIVATE CUBE_EXPORTS=1)
    target_link_libraries(cube-network-shared ws2_32)
endif()

