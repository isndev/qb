#cmake_minimum_required(VERSION 3.10)
#project(cube-test)
#
#set(CMAKE_CXX_STANDARD 17)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(TEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
#set(TCMALLOC_RELEASE "" PARENT_SCOPE)
#set(TCMALLOC_DEBUG "" PARENT_SCOPE)

include_directories(${TEST_PATH})
include_directories(${TEST_PATH}/..)

add_executable(test-actor
        ${TEST_PATH}/actor/actor.cpp ${DEBUGFILES} ${LOGFILES})
add_executable(test-event
        ${TEST_PATH}/actor/event.cpp ${DEBUGFILES} ${LOGFILES})
add_executable(test-chain-actor
        ${TEST_PATH}/benchmark/chain_event.cpp ${DEBUGFILES} ${LOGFILES})
add_executable(test-linked-core
        ${TEST_PATH}/benchmark/linked_core.cpp ${DEBUGFILES} ${LOGFILES})
add_executable(test-core-shared_data
        ${TEST_PATH}/benchmark/shared_data.cpp ${DEBUGFILES} ${LOGFILES})
add_executable(test-TComposition
        ${TEST_PATH}/utils/TComposition.cpp)
add_executable(test-timestamp
        ${TEST_PATH}/utils/timstamp.cpp)

if (USE_TC_MALLOC)
    target_link_libraries(test-actor
            debug ${TCMALLOC_DEBUG}
            optimized ${TCMALLOC_RELEASE})
    target_link_libraries(test-event
            debug ${TCMALLOC_DEBUG}
            optimized ${TCMALLOC_RELEASE})
    target_link_libraries(test-chain-actor
            debug ${TCMALLOC_DEBUG}
            optimized ${TCMALLOC_RELEASE})
    target_link_libraries(test-linked-core
            debug ${TCMALLOC_DEBUG}
            optimized ${TCMALLOC_RELEASE})
    target_link_libraries(test-core-shared_data
            debug ${TCMALLOC_DEBUG}
            optimized ${TCMALLOC_RELEASE})
endif()