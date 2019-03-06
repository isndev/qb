project(${QB_PREFIX}-core LANGUAGES CXX VERSION 1.0.0.0 DESCRIPTION "A simple actor library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} core.cpp)
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (${QB_PREFIX_UPPER}_BUILD_TEST)
    add_subdirectory(tests)
    if (${QB_PREFIX_UPPER}_COVERAGE)
        set(COVERAGE_LCOV_EXCLUDES
                '/usr/*'
                '*benchmark*'
                '*google*/*'
                '*cube/system/*'
                '*modules/*'
                )
        SETUP_TARGET_FOR_COVERAGE_LCOV(NAME ${PROJECT_NAME}-coverage
                EXECUTABLE ctest
                DEPENDENCIES cube-core)
        SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(NAME ${PROJECT_NAME}-coverage-xml
                EXECUTABLE ctest
                DEPENDENCIES cube-core)
    endif()
endif()