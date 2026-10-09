#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# Under a multi-config generator (Visual Studio, Ninja Multi-Config) a qb test executable must sit in
# bin/tests/<CONFIG> -- where qb_ensure_runtime_dll_deployer() puts the DLLs it needs. Before Huly QB-391 it sat in
# bin/: qbConfig.cmake's CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG> initialised the per-configuration property, which
# wins over RUNTIME_OUTPUT_DIRECTORY, so the executable and its DLLs were one directory apart.
#
# Run by ctest as qb-io-test-unit-multiconfig-layout (registered only for a multi-config generator):
#   cmake -DEXE=$<TARGET_FILE:qb-io-test-unit-cpu-topology> -DEXPECT_DIR=<build>/bin/tests/$<CONFIG> -P <this file>
#
if(NOT EXE OR NOT EXPECT_DIR)
    message(FATAL_ERROR "multiconfig-layout: EXE and EXPECT_DIR are required")
endif()
get_filename_component(_got "${EXE}" DIRECTORY)
cmake_path(NORMAL_PATH _got)
set(_want "${EXPECT_DIR}")
cmake_path(NORMAL_PATH _want)
string(REGEX REPLACE "/$" "" _got "${_got}")
string(REGEX REPLACE "/$" "" _want "${_want}")
if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_got}" _got)
    string(TOLOWER "${_want}" _want)
endif()
if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "multiconfig-layout: the test executable is in\n  ${_got}\nbut the runtime DLLs are deployed to\n"
                        "  ${_want}\n(Huly QB-391: a per-configuration output directory must point at <dir>/<CONFIG>)")
endif()
message(STATUS "multiconfig-layout: ${EXE} sits where its DLLs are deployed")
