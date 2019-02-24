set(TEST_PATH ".")

include_directories(${TEST_PATH})
include_directories(${TEST_PATH}/..)

add_executable(test-actor ${TEST_PATH}/actor/actor.cpp)
target_link_libraries(test-actor cube)

add_executable(test-event ${TEST_PATH}/actor/event.cpp)
target_link_libraries(test-event cube)

add_executable(test-scheduler ${TEST_PATH}/actor/scheduler.cpp)
target_link_libraries(test-scheduler cube)

add_executable(test-chain-actor ${TEST_PATH}/benchmark/chain_event.cpp)
target_link_libraries(test-chain-actor cube)

add_executable(test-linked-core ${TEST_PATH}/benchmark/linked_core.cpp)
target_link_libraries(test-linked-core cube)


add_executable(test-TComposition
        ${TEST_PATH}/utils/TComposition.cpp)
add_executable(test-timestamp
        ${TEST_PATH}/utils/timstamp.cpp)

if (UNIX)
    add_executable(test-poller
            ${TEST_PATH}/actor/poller.cpp)
    target_link_libraries(test-poller cube-network)
endif()

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
    target_link_libraries(test-scheduler
            debug ${TCMALLOC_DEBUG}
            optimized ${TCMALLOC_RELEASE})
endif()