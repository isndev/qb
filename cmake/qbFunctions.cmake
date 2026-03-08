#
# qb - C++ Actor Framework
# Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# -----------------------------------------------------------------------------
# qb Framework - User Functions Module
#
# This file provides simple and powerful functions for users of the qb framework.
# It includes functions for creating executables, tests, modules, and more.
# -----------------------------------------------------------------------------

if(QB_FUNCTIONS_INCLUDED)
    return()
endif()
set(QB_FUNCTIONS_INCLUDED TRUE)

# -----------------------------------------------------------------------------
# Internal Helper Functions
# -----------------------------------------------------------------------------

# Internal function to apply common target properties
function(_qb_apply_target_properties target)
    # Set C++ standard
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    
    # Apply compiler flags
    qb_apply_compiler_flags(${target})
    
    # Apply linker flags
    qb_apply_linker_flags(${target})
    
    # Set output directories
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
    
    # Add include directories (PUBLIC so they're inherited by dependents)
    target_include_directories(${target} 
        PUBLIC 
            "$<BUILD_INTERFACE:${QB_INCLUDE_DIR}>"
            "$<BUILD_INTERFACE:${QB_MODULES_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
endfunction()

# Internal function to parse common arguments
function(_qb_parse_common_args prefix)
    set(options PRIVATE_LINKAGE)
    set(oneValueArgs NAME VERSION DESCRIPTION OUTPUT_NAME)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES COMPILE_OPTIONS LINK_OPTIONS)
    
    cmake_parse_arguments(${prefix} "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    # Set parsed arguments in parent scope
    foreach(arg IN LISTS options oneValueArgs multiValueArgs)
        set(${prefix}_${arg} ${${prefix}_${arg}} PARENT_SCOPE)
    endforeach()
    
    # Set unparsed arguments
    set(${prefix}_UNPARSED_ARGUMENTS ${${prefix}_UNPARSED_ARGUMENTS} PARENT_SCOPE)
endfunction()

# Internal function to apply target dependencies
function(_qb_apply_dependencies target dependencies)
    if(NOT dependencies)
        return()
    endif()
    
    # Resolve qb library dependencies
    set(resolved_deps)
    foreach(dep ${dependencies})
        if(dep STREQUAL "qb-io")
            list(APPEND resolved_deps qb::io)
        elseif(dep STREQUAL "qb-core")
            list(APPEND resolved_deps qb::core)
        elseif(dep MATCHES "^qbm-")
            # Module dependency
            string(REPLACE "qbm-" "" module_name ${dep})
            list(APPEND resolved_deps qbm::${module_name})
        else()
            # External dependency
            list(APPEND resolved_deps ${dep})
        endif()
    endforeach()
    
    # Apply resolved dependencies
    if(resolved_deps)
        target_link_libraries(${target} PUBLIC ${resolved_deps})
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Library Functions
# -----------------------------------------------------------------------------

# qb_add_library - Create a library with qb framework integration
function(qb_add_library)
    _qb_parse_common_args(LIB ${ARGN})
    
    if(NOT LIB_NAME)
        qb_error_message("qb_add_library: NAME is required")
    endif()
    
    if(NOT LIB_SOURCES)
        qb_error_message("qb_add_library: SOURCES is required")
    endif()
    
    # Determine library type
    if(QB_BUILD_SHARED_LIBS)
        set(lib_type SHARED)
    else()
        set(lib_type STATIC)
    endif()
    
    # Create library
    add_library(${LIB_NAME} ${lib_type} ${LIB_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${LIB_NAME})
    
    # Apply dependencies
    _qb_apply_dependencies(${LIB_NAME} "${LIB_DEPENDS}")
    
    # Apply additional includes
    if(LIB_INCLUDES)
        target_include_directories(${LIB_NAME} PRIVATE ${LIB_INCLUDES})
    endif()
    
    # Apply definitions
    if(LIB_DEFINES)
        target_compile_definitions(${LIB_NAME} PRIVATE ${LIB_DEFINES})
    endif()
    
    # Apply compile options
    if(LIB_COMPILE_OPTIONS)
        target_compile_options(${LIB_NAME} PRIVATE ${LIB_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(LIB_LINK_OPTIONS)
        target_link_options(${LIB_NAME} PRIVATE ${LIB_LINK_OPTIONS})
    endif()
    
    # Set output name if specified
    if(LIB_OUTPUT_NAME)
        set_target_properties(${LIB_NAME} PROPERTIES OUTPUT_NAME ${LIB_OUTPUT_NAME})
    endif()
    
    # Set version if specified
    if(LIB_VERSION)
        set_target_properties(${LIB_NAME} PROPERTIES VERSION ${LIB_VERSION})
    endif()
    
    qb_debug_message("Created library: ${LIB_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Executable Functions
# -----------------------------------------------------------------------------

# qb_add_executable - Create an executable with qb framework integration
function(qb_add_executable)
    _qb_parse_common_args(EXE ${ARGN})
    
    if(NOT EXE_NAME)
        qb_error_message("qb_add_executable: NAME is required")
    endif()
    
    if(NOT EXE_SOURCES)
        qb_error_message("qb_add_executable: SOURCES is required")
    endif()
    
    # Create executable
    add_executable(${EXE_NAME} ${EXE_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${EXE_NAME})
    
    # Apply dependencies
    _qb_apply_dependencies(${EXE_NAME} "${EXE_DEPENDS}")
    
    # Apply additional includes
    if(EXE_INCLUDES)
        target_include_directories(${EXE_NAME} PRIVATE ${EXE_INCLUDES})
    endif()
    
    # Apply definitions
    if(EXE_DEFINES)
        target_compile_definitions(${EXE_NAME} PRIVATE ${EXE_DEFINES})
    endif()
    
    # Apply compile options
    if(EXE_COMPILE_OPTIONS)
        target_compile_options(${EXE_NAME} PRIVATE ${EXE_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(EXE_LINK_OPTIONS)
        target_link_options(${EXE_NAME} PRIVATE ${EXE_LINK_OPTIONS})
    endif()
    
    # Set output name if specified
    if(EXE_OUTPUT_NAME)
        set_target_properties(${EXE_NAME} PROPERTIES OUTPUT_NAME ${EXE_OUTPUT_NAME})
    endif()
    
    qb_debug_message("Created executable: ${EXE_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Test Functions
# -----------------------------------------------------------------------------

# qb_add_test - Create a test with qb framework integration
function(qb_add_test)
    _qb_parse_common_args(TEST ${ARGN})
    
    if(NOT TEST_NAME)
        qb_error_message("qb_add_test: NAME is required")
    endif()
    
    if(NOT TEST_SOURCES)
        qb_error_message("qb_add_test: SOURCES is required")
    endif()
    
    # Only create tests if testing is enabled
    if(NOT QB_BUILD_TESTS)
        return()
    endif()
    
    # Create test executable
    add_executable(${TEST_NAME} ${TEST_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${TEST_NAME})
    
    # Add Google Test dependency
    if(QB_HAS_GTEST)
        target_link_libraries(${TEST_NAME} PRIVATE GTest::gtest_main)
    else()
        # Use internal Google Test
        target_link_libraries(${TEST_NAME} PRIVATE gtest_main)
    endif()
    
    # Apply dependencies
    _qb_apply_dependencies(${TEST_NAME} "${TEST_DEPENDS}")
    
    # Apply additional includes
    if(TEST_INCLUDES)
        target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDES})
    endif()
    
    # Apply definitions
    if(TEST_DEFINES)
        target_compile_definitions(${TEST_NAME} PRIVATE ${TEST_DEFINES})
    endif()
    
    # Apply compile options
    if(TEST_COMPILE_OPTIONS)
        target_compile_options(${TEST_NAME} PRIVATE ${TEST_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(TEST_LINK_OPTIONS)
        target_link_options(${TEST_NAME} PRIVATE ${TEST_LINK_OPTIONS})
    endif()
    
    # Set test output directory
    set(TEST_BINARY_DIR "${CMAKE_BINARY_DIR}/bin/tests")
    set_target_properties(${TEST_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_BINARY_DIR}"
    )

    # On Windows, copy all runtime DLLs (gtest, gmock, OpenSSL, etc.) next to the
    # test executable so it can be launched directly without touching PATH.
    # We delegate to a cmake -P script instead of calling copy_if_different directly:
    # when $<TARGET_RUNTIME_DLLS:…> is empty (all deps are static), copy_if_different
    # receives no source files and exits with code 1. The script is a safe no-op.
    #
    # Use CMAKE_CURRENT_FUNCTION_LIST_DIR (CMake 3.17+) which resolves to the
    # directory of THIS file at function-definition time, not the calling directory.
    # This avoids the scope issue where QB_CMAKE_DIR is undefined when qb_add_test()
    # is called from a qbm subdirectory that was added after qb's own scope closed.
    if(WIN32)
        add_custom_command(TARGET ${TEST_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DDLL_LIST=$<JOIN:$<TARGET_RUNTIME_DLLS:${TEST_NAME}>,;>"
                "-DDEST_DIR=${TEST_BINARY_DIR}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_runtime_dlls.cmake"
            COMMAND_EXPAND_LISTS
        )
    endif()

    # Make test depend on SSL resources if they exist
    if(QB_HAS_SSL AND TARGET qb_copy_test_ssl_resources)
        add_dependencies(${TEST_NAME} qb_copy_test_ssl_resources)
    endif()
    
    # Register test with CTest
    # Set working directory to where the binary is located so tests can find resources
    add_test(NAME ${TEST_NAME} 
             COMMAND ${TEST_NAME}
             WORKING_DIRECTORY "${TEST_BINARY_DIR}")
    
    # Set test properties for better CTest integration
    set_tests_properties(${TEST_NAME} PROPERTIES
        TIMEOUT 300
        LABELS "qb-tests"
    )
    
    qb_debug_message("Created test: ${TEST_NAME} (working directory: ${TEST_BINARY_DIR})")
endfunction()

# -----------------------------------------------------------------------------
# Benchmark Functions
# -----------------------------------------------------------------------------

# qb_add_benchmark - Create a benchmark with qb framework integration
function(qb_add_benchmark)
    _qb_parse_common_args(BENCH ${ARGN})
    
    if(NOT BENCH_NAME)
        qb_error_message("qb_add_benchmark: NAME is required")
    endif()
    
    if(NOT BENCH_SOURCES)
        qb_error_message("qb_add_benchmark: SOURCES is required")
    endif()
    
    # Only create benchmarks if benchmarking is enabled
    if(NOT QB_BUILD_BENCHMARKS)
        return()
    endif()
    
    # Create benchmark executable
    add_executable(${BENCH_NAME} ${BENCH_SOURCES})
    
    # Apply common properties
    _qb_apply_target_properties(${BENCH_NAME})
    
    # Add Google Benchmark dependency
    if(QB_HAS_BENCHMARK)
        target_link_libraries(${BENCH_NAME} PRIVATE benchmark::benchmark)
    else()
        # Use internal Google Benchmark
        target_link_libraries(${BENCH_NAME} PRIVATE benchmark)
    endif()
    
    # Apply dependencies
    _qb_apply_dependencies(${BENCH_NAME} "${BENCH_DEPENDS}")
    
    # Apply additional includes
    if(BENCH_INCLUDES)
        target_include_directories(${BENCH_NAME} PRIVATE ${BENCH_INCLUDES})
    endif()
    
    # Apply definitions
    if(BENCH_DEFINES)
        target_compile_definitions(${BENCH_NAME} PRIVATE ${BENCH_DEFINES})
    endif()
    
    # Apply compile options
    if(BENCH_COMPILE_OPTIONS)
        target_compile_options(${BENCH_NAME} PRIVATE ${BENCH_COMPILE_OPTIONS})
    endif()
    
    # Apply link options
    if(BENCH_LINK_OPTIONS)
        target_link_options(${BENCH_NAME} PRIVATE ${BENCH_LINK_OPTIONS})
    endif()
    
    # Set benchmark output directory
    set(BENCH_BINARY_DIR "${CMAKE_BINARY_DIR}/bin/benchmarks")
    set_target_properties(${BENCH_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BENCH_BINARY_DIR}"
    )

    # On Windows, copy all runtime DLLs (benchmark, OpenSSL, etc.) next to the
    # benchmark executable so it can be launched directly without touching PATH.
    # Same rationale as for tests: use a -P script to handle the empty-list case safely.
    # CMAKE_CURRENT_FUNCTION_LIST_DIR resolves to this file's directory regardless
    # of which scope calls qb_add_benchmark().
    if(WIN32)
        add_custom_command(TARGET ${BENCH_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DDLL_LIST=$<JOIN:$<TARGET_RUNTIME_DLLS:${BENCH_NAME}>,;>"
                "-DDEST_DIR=${BENCH_BINARY_DIR}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_runtime_dlls.cmake"
            COMMAND_EXPAND_LISTS
        )
    endif()

    qb_debug_message("Created benchmark: ${BENCH_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Module Functions
# -----------------------------------------------------------------------------

# qb_register_module - Register a qb module
function(qb_register_module)
    set(options HEADER_ONLY)
    set(oneValueArgs NAME VERSION DESCRIPTION)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES)
    
    cmake_parse_arguments(MOD "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT MOD_NAME)
        qb_error_message("qb_register_module: NAME is required")
    endif()
    
    # Set module names
    set(module_target "qbm-${MOD_NAME}")
    set(module_alias "qbm::${MOD_NAME}")
    
    # Create module library
    if(MOD_HEADER_ONLY)
        # Header-only module
        add_library(${module_target} INTERFACE)
        
        # Apply interface properties
        if(MOD_INCLUDES)
            target_include_directories(${module_target} INTERFACE ${MOD_INCLUDES})
        endif()
        
        if(MOD_DEFINES)
            target_compile_definitions(${module_target} INTERFACE ${MOD_DEFINES})
        endif()
        
        # Apply dependencies
        if(MOD_DEPENDS)
            _qb_apply_dependencies(${module_target} "${MOD_DEPENDS}")
        endif()
        
    else()
        # Regular module with sources
        if(NOT MOD_SOURCES)
            qb_error_message("qb_register_module: SOURCES is required for non-header-only modules")
        endif()
        
        # Create library
        if(QB_BUILD_SHARED_LIBS)
            add_library(${module_target} SHARED ${MOD_SOURCES})
        else()
            add_library(${module_target} STATIC ${MOD_SOURCES})
        endif()
        
        # Apply common properties
        _qb_apply_target_properties(${module_target})
        
        # Apply dependencies
        _qb_apply_dependencies(${module_target} "${MOD_DEPENDS}")
        
        # Apply additional includes
        if(MOD_INCLUDES)
            target_include_directories(${module_target} PRIVATE ${MOD_INCLUDES})
        endif()
        
        # Apply definitions
        if(MOD_DEFINES)
            target_compile_definitions(${module_target} PRIVATE ${MOD_DEFINES})
        endif()
        
        # Set version if specified
        if(MOD_VERSION)
            set_target_properties(${module_target} PROPERTIES VERSION ${MOD_VERSION})
        endif()
    endif()
    
    # Create alias
    add_library(${module_alias} ALIAS ${module_target})
    
    # Add to global module list
    list(APPEND QB_MODULE_LIBRARIES ${module_target})
    set(QB_MODULE_LIBRARIES ${QB_MODULE_LIBRARIES} PARENT_SCOPE)
    
    qb_status_message("Registered module: ${MOD_NAME}")
endfunction()

# -----------------------------------------------------------------------------
# Module Loading Functions
# -----------------------------------------------------------------------------

# qb_load_modules - Load all modules from a directory
function(qb_load_modules modules_dir)
    if(NOT IS_DIRECTORY "${modules_dir}")
        qb_error_message("qb_load_modules: Directory does not exist: ${modules_dir}")
    endif()
    
    qb_status_message("Loading modules from: ${modules_dir}")
    
    # Get all subdirectories
    file(GLOB module_dirs RELATIVE "${modules_dir}" "${modules_dir}/*")
    
    foreach(module_dir ${module_dirs})
        set(full_module_path "${modules_dir}/${module_dir}")
        
        # Check if it's a directory and has a CMakeLists.txt
        if(IS_DIRECTORY "${full_module_path}")
            set(cmake_file "${full_module_path}/CMakeLists.txt")
            if(EXISTS "${cmake_file}")
                qb_debug_message("Loading module: ${module_dir}")
                add_subdirectory("${full_module_path}")
            else()
                qb_debug_message("Skipping module ${module_dir}: no CMakeLists.txt found")
            endif()
        endif()
    endforeach()
    
    # Add modules directory to include path
    include_directories("${modules_dir}")
endfunction()

# -----------------------------------------------------------------------------
# Test Module Functions
# -----------------------------------------------------------------------------

# qb_register_module_test - Register a test for a module
function(qb_register_module_test)
    set(options)
    set(oneValueArgs MODULE_NAME TEST_NAME)
    set(multiValueArgs SOURCES DEPENDS INCLUDES DEFINES)
    
    cmake_parse_arguments(MTEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT MTEST_MODULE_NAME)
        qb_error_message("qb_register_module_test: MODULE_NAME is required")
    endif()
    
    if(NOT MTEST_TEST_NAME)
        qb_error_message("qb_register_module_test: TEST_NAME is required")
    endif()
    
    if(NOT MTEST_SOURCES)
        qb_error_message("qb_register_module_test: SOURCES is required")
    endif()
    
    # Only create tests if testing is enabled
    if(NOT QB_BUILD_TESTS)
        return()
    endif()
    
    # Create test name
    set(test_target "qbm-${MTEST_MODULE_NAME}-test-${MTEST_TEST_NAME}")
    
    # Create test
    qb_add_test(
        NAME ${test_target}
        SOURCES ${MTEST_SOURCES}
        DEPENDS qbm-${MTEST_MODULE_NAME} ${MTEST_DEPENDS}
        INCLUDES ${MTEST_INCLUDES}
        DEFINES ${MTEST_DEFINES}
    )
    
    qb_debug_message("Created module test: ${test_target}")
endfunction()

# -----------------------------------------------------------------------------
# Test Resource Functions
# -----------------------------------------------------------------------------

# qb_setup_test_resources - Setup SSL resources for tests (centralized)
# This function creates a global target that copies SSL resources to the test directory
# Call this once in your root CMakeLists.txt or test configuration
function(qb_setup_test_resources)
    if(NOT QB_BUILD_TESTS)
        return()
    endif()
    
    set(TEST_RESOURCES_DIR "${CMAKE_BINARY_DIR}/bin/tests")
    
    # Create a global target for copying SSL resources
    if(QB_HAS_SSL AND QB_SSL_RESOURCES)
        if(NOT TARGET qb_copy_test_ssl_resources)
            add_custom_target(qb_copy_test_ssl_resources ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_RESOURCES_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QB_SSL_RESOURCES}" "${TEST_RESOURCES_DIR}/ssl"
                COMMENT "Copying SSL resources to test directory: ${TEST_RESOURCES_DIR}/ssl"
            )
            
            # Set folder for IDE organization
            set_target_properties(qb_copy_test_ssl_resources PROPERTIES
                FOLDER "Tests/Resources"
            )
            
            qb_status_message("SSL test resources will be copied to: ${TEST_RESOURCES_DIR}/ssl")
        endif()
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Utility Functions
# -----------------------------------------------------------------------------

# qb_copy_resources - Copy resources to output directory
function(qb_copy_resources)
    set(options)
    set(oneValueArgs TARGET DESTINATION)
    set(multiValueArgs RESOURCES)
    
    cmake_parse_arguments(RES "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT RES_TARGET)
        qb_error_message("qb_copy_resources: TARGET is required")
    endif()
    
    if(NOT RES_RESOURCES)
        qb_error_message("qb_copy_resources: RESOURCES is required")
    endif()
    
    if(NOT RES_DESTINATION)
        set(RES_DESTINATION "${CMAKE_BINARY_DIR}/bin/resources")
    endif()
    
    # Create custom target for copying resources
    set(copy_target "${RES_TARGET}_copy_resources")
    
    add_custom_target(${copy_target}
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        ${RES_RESOURCES} ${RES_DESTINATION}
        COMMENT "Copying resources for ${RES_TARGET}"
    )
    
    # Make the main target depend on the copy target
    add_dependencies(${RES_TARGET} ${copy_target})
    
    qb_debug_message("Added resource copy for: ${RES_TARGET}")
endfunction()

# qb_install_target - Install a target with proper configuration
function(qb_install_target)
    set(options)
    set(oneValueArgs TARGET)
    set(multiValueArgs)
    
    cmake_parse_arguments(INST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT INST_TARGET)
        qb_error_message("qb_install_target: TARGET is required")
    endif()
    
    if(NOT QB_INSTALL)
        return()
    endif()
    
    # Install target
    install(TARGETS ${INST_TARGET}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    
    qb_debug_message("Configured installation for: ${INST_TARGET}")
endfunction()

# Mark functions as loaded
set(QB_FUNCTIONS_LOADED TRUE CACHE INTERNAL "qb functions loaded") 