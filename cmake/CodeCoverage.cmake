# Copyright (c) 2012 - 2017, Lars Bilke
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this
#    list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors
#    may be used to endorse or promote products derived from this software without
#    specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
# ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
# ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# CHANGES:
#
# 2012-01-31, Lars Bilke
# - Enable Code Coverage
#
# 2013-09-17, Joakim Söderberg
# - Added support for Clang.
# - Some additional usage instructions.
#
# 2016-02-03, Lars Bilke
# - Refactored functions to use named parameters
#
# 2017-06-02, Lars Bilke
# - Merged with modified version from github.com/ufz/ogs
#
# qb, 2026-08: gcov-compatible tool detection for clang (`llvm-cov gcov`), and the
# - one-writer/N-readers split: SETUP_COVERAGE_RUN() plus the optional RUN_TARGET
# - argument on each report function. See the banner above SETUP_COVERAGE_RUN.
#
#
# USAGE:
#
# 1. Copy this file into your cmake modules path.
#
# 2. Add the following line to your CMakeLists.txt:
#      include(CodeCoverage)
#
# 3. Append necessary compiler flags:
#      APPEND_COVERAGE_COMPILER_FLAGS()
#
# 3.a (OPTIONAL) Set appropriate optimization flags, e.g. -O0, -O1 or -Og
#
# 4. If you need to exclude additional directories from the report, specify them
#    using the COVERAGE_LCOV_EXCLUDES variable before calling SETUP_TARGET_FOR_COVERAGE_LCOV.
#    Example:
#      set(COVERAGE_LCOV_EXCLUDES 'dir1/*' 'dir2/*')
#
# 5. Use the functions described below to create a custom make target which
#    runs your test executable and produces a code coverage report.
#
# 6. Build a Debug build:
#      cmake -DCMAKE_BUILD_TYPE=Debug ..
#      make
#      make my_coverage_target
#

include(CMakeParseArguments)

# Check prereqs
if(NOT GCOV_PATH AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    string(REGEX MATCH "^[0-9]+" _gcov_major "${CMAKE_CXX_COMPILER_VERSION}")
    find_program(GCOV_PATH NAMES "gcov-${_gcov_major}" gcov)
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "(Apple)?[Cc]lang")
    find_program(LLVM_COV_PATH
        NAMES llvm-cov
        HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    )
    if(LLVM_COV_PATH AND (NOT GCOV_PATH OR GCOV_PATH MATCHES "/gcov(-[0-9]+)?$"))
        set(GCOV_PATH "${LLVM_COV_PATH} gcov" CACHE STRING "gcov-compatible coverage command" FORCE)
    elseif(NOT GCOV_PATH)
        find_program(GCOV_PATH gcov)
    endif()
else()
    find_program(GCOV_PATH gcov)
endif()
find_program( LCOV_PATH  NAMES lcov lcov.bat lcov.exe lcov.perl)
find_program( GENHTML_PATH NAMES genhtml genhtml.perl genhtml.bat )
find_program( GCOVR_PATH gcovr PATHS ${CMAKE_SOURCE_DIR}/scripts/test)
find_package(Python COMPONENTS Interpreter)

if(NOT GCOV_PATH)
    message(FATAL_ERROR "gcov not found! Aborting...")
endif() # NOT GCOV_PATH

if("${CMAKE_CXX_COMPILER_ID}" MATCHES "(Apple)?[Cc]lang")
    if("${CMAKE_CXX_COMPILER_VERSION}" VERSION_LESS 3)
        message(FATAL_ERROR "Clang version must be 3.0.0 or greater! Aborting...")
    endif()
elseif(NOT CMAKE_COMPILER_IS_GNUCXX)
    message(FATAL_ERROR "Compiler is not GNU gcc! Aborting...")
endif()

set(COVERAGE_COMPILER_FLAGS "-g -fprofile-arcs -ftest-coverage"
        CACHE INTERNAL "")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(COVERAGE_COMPILER_FLAGS "${COVERAGE_COMPILER_FLAGS} --coverage")
endif()

set(CMAKE_CXX_FLAGS_COVERAGE
        ${COVERAGE_COMPILER_FLAGS}
        CACHE STRING "Flags used by the C++ compiler during coverage builds."
        FORCE )
set(CMAKE_C_FLAGS_COVERAGE
        ${COVERAGE_COMPILER_FLAGS}
        CACHE STRING "Flags used by the C compiler during coverage builds."
        FORCE )
set(CMAKE_EXE_LINKER_FLAGS_COVERAGE
        ""
        CACHE STRING "Flags used for linking binaries during coverage builds."
        FORCE )
set(CMAKE_SHARED_LINKER_FLAGS_COVERAGE
        ""
        CACHE STRING "Flags used by the shared libraries linker during coverage builds."
        FORCE )
mark_as_advanced(
        CMAKE_CXX_FLAGS_COVERAGE
        CMAKE_C_FLAGS_COVERAGE
        CMAKE_EXE_LINKER_FLAGS_COVERAGE
        CMAKE_SHARED_LINKER_FLAGS_COVERAGE )

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "Code coverage results with an optimised (non-Debug) build may be misleading")
endif() # NOT CMAKE_BUILD_TYPE STREQUAL "Debug"

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    link_libraries(gcov)
else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

# -----------------------------------------------------------------------------
# qb ADDITION -- one writer for the counters, N readers ordered after it
# -----------------------------------------------------------------------------
# Everything below this line up to the next banner is a qb addition to Lars Bilke's
# module; the report functions keep their original behaviour when RUN_TARGET is not
# passed, so an existing caller is unaffected.
#
# The three report functions below were each self-contained: zero the counters, run
# the suite, capture. That is safe for exactly one target. It is NOT safe for a
# project that defines several of them, because they do not describe what they touch:
#
#   * `.gcda` counters are SHARED build-tree state. They live next to the objects, so
#     every report target that passes `-directory .` from a directory ABOVE them is
#     talking about the same files -- and `--zerocounters` deletes them.
#   * `add_custom_target()` creates NO ordering edge to a sibling. `cmake --build .
#     --target a b` may run both commands at once under any parallel generator.
#
# So `--target coverage coverage-lcov` had one target erasing the counters the other
# was in the middle of producing, and the report that came out was silently short --
# no error, no exit code, just smaller numbers. Coverage output is precisely the kind
# of artefact nobody re-derives by hand to notice.
#
# The fix is structural, not a timing tweak. SETUP_COVERAGE_RUN() creates the single
# target permitted to MUTATE the counter set (zero -> baseline -> run the suite once).
# Passing RUN_TARGET to a report function turns it into a pure READER: it drops the
# zero/baseline/run steps and takes an add_dependencies() edge on the run target. Any
# subset of the readers, requested in any order, then gets: counters zeroed once, the
# suite run once, and every report generated from that one set of counters.
#
# SETUP_COVERAGE_RUN(
#     NAME coverage-run                           # New target name
#     EXECUTABLE ctest                            # Executable in PROJECT_BINARY_DIR
#     DEPENDENCIES testrunner                     # Dependencies to build first
# )
function(SETUP_COVERAGE_RUN)

    set(options NONE)
    set(oneValueArgs NAME BASELINE)
    set(multiValueArgs EXECUTABLE EXECUTABLE_ARGS DEPENDENCIES LCOV_ARGS)
    cmake_parse_arguments(Coverage "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT LCOV_PATH)
        message(FATAL_ERROR "lcov not found! Aborting...")
    endif() # NOT LCOV_PATH

    if(NOT Coverage_BASELINE)
        set(Coverage_BASELINE "${PROJECT_BINARY_DIR}/${Coverage_NAME}.base")
    endif()

    add_custom_target(${Coverage_NAME}
            # Cleanup lcov -- the ONE place the counters are erased.
            COMMAND ${LCOV_PATH} ${Coverage_LCOV_ARGS} --gcov-tool ${GCOV_PATH} -directory . --zerocounters
            # Create baseline to make sure untouched files show up in the report
            COMMAND ${LCOV_PATH} ${Coverage_LCOV_ARGS} --gcov-tool ${GCOV_PATH} -c -i -d . -o ${Coverage_BASELINE}

            # Run tests
            COMMAND ${Coverage_EXECUTABLE} ${Coverage_EXECUTABLE_ARGS}

            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
            DEPENDS ${Coverage_DEPENDENCIES}
            COMMENT "Resetting code coverage counters to zero and running the suite once."
            USES_TERMINAL
            )

endfunction() # SETUP_COVERAGE_RUN

# Defines a target for running and collection code coverage information
# Builds dependencies, runs the given executable and outputs reports.
# NOTE! The executable should always have a ZERO as exit code otherwise
# the coverage generation will not complete.
#
# SETUP_TARGET_FOR_COVERAGE_LCOV(
#     NAME testrunner_coverage                    # New target name
#     EXECUTABLE testrunner -j ${PROCESSOR_COUNT} # Executable in PROJECT_BINARY_DIR
#     DEPENDENCIES testrunner                     # Dependencies to build first
#     [RUN_TARGET coverage-run]                   # qb: reader mode, see banner above
#     [BASELINE <file>]                           # qb: baseline written by RUN_TARGET
# )
function(SETUP_TARGET_FOR_COVERAGE_LCOV)

    set(options NONE)
    set(oneValueArgs NAME RUN_TARGET BASELINE)
    set(multiValueArgs EXECUTABLE EXECUTABLE_ARGS DEPENDENCIES LCOV_ARGS GENHTML_ARGS)
    cmake_parse_arguments(Coverage "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT LCOV_PATH)
        message(FATAL_ERROR "lcov not found! Aborting...")
    endif() # NOT LCOV_PATH

    if(NOT GENHTML_PATH)
        message(FATAL_ERROR "genhtml not found! Aborting...")
    endif() # NOT GENHTML_PATH

    if(Coverage_RUN_TARGET)
        # Reader: RUN_TARGET already zeroed, captured the baseline and ran the suite.
        # It still owns the baseline file, so this target must not delete it.
        if(NOT Coverage_BASELINE)
            set(Coverage_BASELINE "${PROJECT_BINARY_DIR}/${Coverage_RUN_TARGET}.base")
        endif()
        set(_cov_prologue "")
        set(_cov_cleanup ${Coverage_NAME}.total ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned)
        set(_cov_comment "Processing code coverage counters and generating report.")
    else()
        set(Coverage_BASELINE ${Coverage_NAME}.base)
        set(_cov_prologue
            # Cleanup lcov
            COMMAND ${LCOV_PATH} ${Coverage_LCOV_ARGS} --gcov-tool ${GCOV_PATH} -directory . --zerocounters
            # Create baseline to make sure untouched files show up in the report
            COMMAND ${LCOV_PATH} ${Coverage_LCOV_ARGS} --gcov-tool ${GCOV_PATH} -c -i -d . -o ${Coverage_BASELINE}

            # Run tests
            COMMAND ${Coverage_EXECUTABLE} ${Coverage_EXECUTABLE_ARGS}
        )
        set(_cov_cleanup ${Coverage_BASELINE} ${Coverage_NAME}.total ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned)
        set(_cov_comment "Resetting code coverage counters to zero.\nProcessing code coverage counters and generating report.")
    endif()

    # Setup target
    add_custom_target(${Coverage_NAME}
            ${_cov_prologue}

            # Capturing lcov counters and generating report
            COMMAND ${LCOV_PATH} ${Coverage_LCOV_ARGS} --gcov-tool ${GCOV_PATH} --directory . --capture --output-file ${Coverage_NAME}.info
            # add baseline counters
            COMMAND ${LCOV_PATH} ${Coverage_LCOV_ARGS} --gcov-tool ${GCOV_PATH} -a ${Coverage_BASELINE} -a ${Coverage_NAME}.info --output-file ${Coverage_NAME}.total
            COMMAND ${LCOV_PATH} ${Coverage_LCOV_ARGS} --gcov-tool ${GCOV_PATH} --remove ${Coverage_NAME}.total ${COVERAGE_LCOV_EXCLUDES} --output-file ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned
            COMMAND ${GENHTML_PATH} ${Coverage_GENHTML_ARGS} -o ${Coverage_NAME} ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned
            COMMAND ${CMAKE_COMMAND} -E remove ${_cov_cleanup}

            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
            DEPENDS ${Coverage_DEPENDENCIES}
            COMMENT "${_cov_comment}"
            )

    if(Coverage_RUN_TARGET)
        add_dependencies(${Coverage_NAME} ${Coverage_RUN_TARGET})
    endif()

endfunction() # SETUP_TARGET_FOR_COVERAGE_LCOV

# Defines a target for running and collection code coverage information
# Builds dependencies, runs the given executable and outputs reports.
# NOTE! The executable should always have a ZERO as exit code otherwise
# the coverage generation will not complete.
#
# SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(
#     NAME ctest_coverage                    # New target name
#     EXECUTABLE ctest -j ${PROCESSOR_COUNT} # Executable in PROJECT_BINARY_DIR
#     DEPENDENCIES executable_target         # Dependencies to build first
#     [RUN_TARGET coverage-run]              # qb: reader mode, see the banner above
# )
function(SETUP_TARGET_FOR_COVERAGE_GCOVR_XML)

    set(options NONE)
    set(oneValueArgs NAME RUN_TARGET)
    set(multiValueArgs EXECUTABLE EXECUTABLE_ARGS DEPENDENCIES GCOVR_ARGS)
    cmake_parse_arguments(Coverage "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT Python_FOUND)
        message(FATAL_ERROR "python not found! Aborting...")
    endif()

    if(NOT GCOVR_PATH)
        message(FATAL_ERROR "gcovr not found! Aborting...")
    endif() # NOT GCOVR_PATH

    # Combine excludes to several -e arguments
    set(GCOVR_EXCLUDES "")
    foreach(EXCLUDE ${COVERAGE_GCOVR_EXCLUDES})
        list(APPEND GCOVR_EXCLUDES "-e")
        list(APPEND GCOVR_EXCLUDES "${EXCLUDE}")
    endforeach()

    # Reader mode: RUN_TARGET ran the suite, this target only reads the counters.
    if(Coverage_RUN_TARGET)
        set(_cov_run "")
    else()
        set(_cov_run ${Coverage_EXECUTABLE} ${Coverage_EXECUTABLE_ARGS})
    endif()

    add_custom_target(${Coverage_NAME}
            # Run tests
            ${_cov_run}

            # Running gcovr
            COMMAND ${GCOVR_PATH} ${Coverage_GCOVR_ARGS} --xml
            -r ${PROJECT_SOURCE_DIR} ${GCOVR_EXCLUDES}
            --gcov-executable=${GCOV_PATH}
            --object-directory=${PROJECT_BINARY_DIR}
            -o ${Coverage_NAME}.xml
            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
            DEPENDS ${Coverage_DEPENDENCIES}
            COMMENT "Running gcovr to produce Cobertura code coverage report."
            )

    if(Coverage_RUN_TARGET)
        add_dependencies(${Coverage_NAME} ${Coverage_RUN_TARGET})
    endif()

endfunction() # SETUP_TARGET_FOR_COVERAGE_GCOVR_XML

# Defines a target for running and collection code coverage information
# Builds dependencies, runs the given executable and outputs reports.
# NOTE! The executable should always have a ZERO as exit code otherwise
# the coverage generation will not complete.
#
# SETUP_TARGET_FOR_COVERAGE_GCOVR_HTML(
#     NAME ctest_coverage                    # New target name
#     EXECUTABLE ctest -j ${PROCESSOR_COUNT} # Executable in PROJECT_BINARY_DIR
#     DEPENDENCIES executable_target         # Dependencies to build first
#     [RUN_TARGET coverage-run]              # qb: reader mode, see the banner above
# )
function(SETUP_TARGET_FOR_COVERAGE_GCOVR_HTML)

    set(options NONE)
    set(oneValueArgs NAME RUN_TARGET)
    set(multiValueArgs EXECUTABLE EXECUTABLE_ARGS DEPENDENCIES GCOVR_ARGS)
    cmake_parse_arguments(Coverage "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT Python_FOUND)
        message(FATAL_ERROR "python not found! Aborting...")
    endif()

    if(NOT GCOVR_PATH)
        message(FATAL_ERROR "gcovr not found! Aborting...")
    endif() # NOT GCOVR_PATH

    # Combine excludes to several -e arguments
    set(GCOVR_EXCLUDES "")
    foreach(EXCLUDE ${COVERAGE_GCOVR_EXCLUDES})
        list(APPEND GCOVR_EXCLUDES "-e")
        list(APPEND GCOVR_EXCLUDES "${EXCLUDE}")
    endforeach()

    # Reader mode: RUN_TARGET ran the suite, this target only reads the counters.
    if(Coverage_RUN_TARGET)
        set(_cov_run "")
    else()
        set(_cov_run ${Coverage_EXECUTABLE} ${Coverage_EXECUTABLE_ARGS})
    endif()

    add_custom_target(${Coverage_NAME}
            # Run tests
            ${_cov_run}

            # Create folder
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/${Coverage_NAME}

            # Running gcovr
            COMMAND ${GCOVR_PATH} ${Coverage_GCOVR_ARGS} --html --html-details
            -r ${PROJECT_SOURCE_DIR} ${GCOVR_EXCLUDES}
            --gcov-executable=${GCOV_PATH}
            --object-directory=${PROJECT_BINARY_DIR}
            -o ${Coverage_NAME}/index.html
            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
            DEPENDS ${Coverage_DEPENDENCIES}
            COMMENT "Running gcovr to produce HTML code coverage report."
            )

    if(Coverage_RUN_TARGET)
        add_dependencies(${Coverage_NAME} ${Coverage_RUN_TARGET})
    endif()

endfunction() # SETUP_TARGET_FOR_COVERAGE_GCOVR_HTML

function(APPEND_COVERAGE_COMPILER_FLAGS)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COVERAGE_COMPILER_FLAGS}" PARENT_SCOPE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COVERAGE_COMPILER_FLAGS}" PARENT_SCOPE)
    message(STATUS "Appending code coverage compiler flags: ${COVERAGE_COMPILER_FLAGS}")
endfunction() # APPEND_COVERAGE_COMPILER_FLAGS
