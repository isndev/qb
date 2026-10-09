#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The vendored stduuid fork as a user of the fork alone gets it (Huly QB-375, QB-376): configured on its own in its
# DEFAULT configuration, installed, then found by a consumer that asks for a version. qb's embedded build takes none
# of these paths, which is how both defects stayed invisible to every qb lane:
#   - QB-375: the default configuration installed a gsl/ directory the C++20 migration had deleted -- step 2 failed;
#   - QB-376: the version file was stduuid-version.cmake, a name find_package never reads beside stduuid-config.cmake,
#     so `find_package(stduuid 1.0)` rejected the package it had found -- step 3 failed.
# Configure and install only: the fork is header-only, nothing is compiled.
#
# Run by ctest as qb-io-test-unit-stduuid-standalone (tests/io/unit/CMakeLists.txt):
#   cmake -DUUID_SOURCE_DIR=<qb>/src/qb/vendor/uuid -DWORK_DIR=<scratch> -DGENERATOR=<generator>
#         [-DPLATFORM=<-A>] [-DTOOLSET=<-T>] [-DMAKE_PROGRAM=<path>] [-DCXX_COMPILER=<path>] -P <this file>
#
foreach(_required UUID_SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT ${_required})
        message(FATAL_ERROR "stduuid-standalone: ${_required} is required")
    endif()
endforeach()

set(_generator_args -G "${GENERATOR}")
if(PLATFORM)
    list(APPEND _generator_args -A "${PLATFORM}")
endif()
if(TOOLSET)
    list(APPEND _generator_args -T "${TOOLSET}")
endif()
if(MAKE_PROGRAM)
    list(APPEND _generator_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
set(_compiler_args "")
if(CXX_COMPILER)
    list(APPEND _compiler_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

function(_stduuid_step what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "stduuid-standalone: ${what} FAILED (exit ${_rc})\n${_out}\n${_err}")
    endif()
    message(STATUS "stduuid-standalone: ${what}: ok")
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")

# 1. The fork on its own, no option set: the configuration its README hands a user.
_stduuid_step("configure the fork standalone"
    "${CMAKE_COMMAND}" -S "${UUID_SOURCE_DIR}" -B "${WORK_DIR}/build" ${_generator_args} ${_compiler_args})

# 2. Installed (QB-375). --config names a configuration for a multi-config generator; nothing is built either way.
_stduuid_step("install the fork"
    "${CMAKE_COMMAND}" --install "${WORK_DIR}/build" --prefix "${WORK_DIR}/prefix" --config Release)

# 3. Found WITH a version (QB-376), and the package delivers its target.
file(WRITE "${WORK_DIR}/consumer/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(stduuid_consumer NONE)\n"
    "find_package(stduuid 1.0 CONFIG REQUIRED)\n"
    "if(NOT TARGET stduuid)\n"
    "    message(FATAL_ERROR \"find_package(stduuid) succeeded without creating the stduuid target\")\n"
    "endif()\n")
_stduuid_step("find_package(stduuid 1.0 CONFIG REQUIRED) from a consumer"
    "${CMAKE_COMMAND}" -S "${WORK_DIR}/consumer" -B "${WORK_DIR}/consumer-build" ${_generator_args}
    "-DCMAKE_PREFIX_PATH=${WORK_DIR}/prefix")

message(STATUS "stduuid-standalone: configured, installed and found by version")
