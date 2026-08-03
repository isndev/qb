cmake_minimum_required(VERSION 3.22)
project(stduuid CXX)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

option(UUID_BUILD_TESTS "Build the unit tests" OFF)
option(UUID_SYSTEM_GENERATOR "Enable operating system uuid generator" OFF)
option(UUID_TIME_GENERATOR "Enable experimental time-based uuid generator" OFF)
option(UUID_USING_CXX20_SPAN "Using span from std instead of gsl" OFF)

# An embedding project sets QB_VENDOR_INCLUDE_PREFIX (qb sets it to "qb/vendor") to declare that it
# reaches this fork's header through ITS OWN include root, as <qb/vendor/uuid/include/uuid.h>. In
# that mode this project must publish neither an include directory nor an install rule for uuid.h:
# both put the bare name `uuid.h` at the consumer's include root, where it shadows (and is shadowed
# by) any other uuid.h on the search path -- including the system libuuid one.
if (DEFINED QB_VENDOR_INCLUDE_PREFIX AND NOT QB_VENDOR_INCLUDE_PREFIX STREQUAL "")
    set(QB_UUID_EMBEDDED ON)
else ()
    set(QB_UUID_EMBEDDED OFF)
endif ()

# Library target
add_library(${PROJECT_NAME} INTERFACE)
if (NOT QB_UUID_EMBEDDED)
    target_include_directories(${PROJECT_NAME} INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
endif ()

# Using system uuid generator
if (UUID_SYSTEM_GENERATOR)
    target_compile_definitions(${PROJECT_NAME} INTERFACE UUID_SYSTEM_GENERATOR)

    if (WIN32)
    elseif (APPLE)
        find_library(CFLIB CoreFoundation REQUIRED)
        target_link_libraries(${PROJECT_NAME} INTERFACE ${CFLIB})
    else ()
        find_package(Libuuid REQUIRED)
        if (Libuuid_FOUND)
            target_include_directories(${PROJECT_NAME} INTERFACE ${Libuuid_INCLUDE_DIRS})
            target_link_libraries(${PROJECT_NAME} INTERFACE ${Libuuid_LIBRARIES})
        endif ()
    endif ()
endif ()

# Using time-based generator
if (UUID_TIME_GENERATOR)
    target_compile_definitions(${PROJECT_NAME} INTERFACE UUID_TIME_GENERATOR)
endif ()

# Using span from std
if (NOT UUID_USING_CXX20_SPAN)
    target_include_directories(${PROJECT_NAME} INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/gsl>
            $<INSTALL_INTERFACE:include/gsl>)
    install(DIRECTORY gsl DESTINATION include)
endif ()

# Install step and imported target.
# Skipped when embedded: the parent installs this target in its own export set and ships uuid.h
# through its own install(DIRECTORY include/), at the namespaced path. Running it here as well
# would re-plant a bare `uuid.h` in the include root and a second lib/cmake/stduuid/ package for a
# library the parent already exports.
if (NOT QB_UUID_EMBEDDED)
install(FILES include/uuid.h DESTINATION include)
install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}-targets)
install(EXPORT ${PROJECT_NAME}-targets
        DESTINATION lib/cmake/${PROJECT_NAME})

# Config files for find_package()
include(CMakePackageConfigHelpers)
configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
        INSTALL_DESTINATION lib/cmake/${PROJECT_NAME})
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-version.cmake"
        VERSION "1.0"
        COMPATIBILITY AnyNewerVersion)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-version.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindLibuuid.cmake"
        DESTINATION lib/cmake/${PROJECT_NAME})
export(EXPORT ${PROJECT_NAME}-targets
        FILE "${CMAKE_CURRENT_BINARY_DIR}/cmake/${PROJECT_NAME}-targets.cmake")
endif () # NOT QB_UUID_EMBEDDED

# Tests
if (UUID_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif ()