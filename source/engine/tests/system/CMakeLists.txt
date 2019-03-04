
cxx_gtest(${PROJECT_NAME}-gtest-system-main "${PROJECT_NAME}" test-main.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-add "${PROJECT_NAME}" test-actor-add.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-callback "${PROJECT_NAME}" test-actor-callback.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-event "${PROJECT_NAME}" test-actor-event.cpp)

if (CUBE_COVERAGE)
    set(COVERAGE_LCOV_EXCLUDES
            '/usr/*'
            '*benchmark*'
            '*google*/*'
            '*cube/system/*'
            '*modules/*'
            )
    SETUP_TARGET_FOR_COVERAGE_LCOV(NAME ${PROJECT_NAME}-coverage
            EXECUTABLE ctest
            DEPENDENCIES cube-engine )
endif()