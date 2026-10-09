#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The contract of qb/cmake/FindGperftools.cmake (Huly QB-390), checked by CONFIGURING this languageless project
# against fabricated install prefixes: no compiler, no build, a few milliseconds. Run by ctest as
# qb-io-test-unit-find-gperftools (tests/io/unit/CMakeLists.txt); a mismatch is a SEND_ERROR, so the configure -- and
# the test -- fails. Before the fix, cases 1, 2 and 4 failed: headers alone were "found" (Gperftools_FOUND TRUE, qb
# then reported profiling ON and linked nothing), and no Gperftools_<C>_FOUND was ever set, so `COMPONENTS PROFILER`
# was never satisfied and no component reported itself found.
#
#   cmake --fresh -S <this dir> -B <build> -DQB_CMAKE_MODULE_DIR=<qb>/cmake
#
cmake_minimum_required(VERSION 3.24)
project(qb_find_gperftools_contract NONE)

if(NOT QB_CMAKE_MODULE_DIR OR NOT EXISTS "${QB_CMAKE_MODULE_DIR}/FindGperftools.cmake")
    message(FATAL_ERROR "QB_CMAKE_MODULE_DIR must name qb/cmake (got '${QB_CMAKE_MODULE_DIR}')")
endif()
list(PREPEND CMAKE_MODULE_PATH "${QB_CMAKE_MODULE_DIR}")

# Hermetic: the module also searches /usr, /usr/local, Homebrew and the library-path environment, and a gperftools
# really installed on the host must never satisfy (or spoil) a fabricated case.
set(CMAKE_IGNORE_PATH
    /usr/lib /usr/local/lib /usr/local/homebrew/lib /opt/homebrew/lib /opt/local/lib
    /usr/include /usr/local/include /usr/local/homebrew/include /opt/homebrew/include /opt/local/include)
set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH FALSE)
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH FALSE)
set(CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH FALSE)
set(ENV{LD_LIBRARY_PATH} "")
set(ENV{DYLD_LIBRARY_PATH} "")
# A languageless project need not have loaded the platform's library naming: name every spelling the fixture writes.
set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".so" ".dylib" ".lib")

set(_cases 0)
set(_failed 0)

# A fake install prefix: the header when HEADER is given, and every platform spelling of each named library.
function(make_prefix dir)
    cmake_parse_arguments(P "HEADER" "" "LIBS" ${ARGN})
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}/lib" "${dir}/include")
    if(P_HEADER)
        file(WRITE "${dir}/include/gperftools/heap-profiler.h" "/* fixture */\n")
    endif()
    foreach(lib IN LISTS P_LIBS)
        foreach(name "lib${lib}.a" "lib${lib}.so" "lib${lib}.dylib" "${lib}.lib" "lib${lib}.lib")
            file(WRITE "${dir}/lib/${name}" "")
        endforeach()
    endforeach()
endfunction()

# One case: forget every cached result, point the module at the prefix, find_package(Gperftools [COMPONENTS ...]),
# compare Gperftools_FOUND (and a component's flag) with what the contract says.
function(check_case name prefix expect_found)
    cmake_parse_arguments(C "" "COMPONENT_FLAG;EXPECT_FLAG" "COMPONENTS" ${ARGN})
    foreach(v GPERFTOOLS_TCMALLOC GPERFTOOLS_PROFILER GPERFTOOLS_TCMALLOC_MINIMAL GPERFTOOLS_TCMALLOC_AND_PROFILER
              GPERFTOOLS_INCLUDE_DIR)
        unset(${v} CACHE)
    endforeach()
    set(GPERFTOOLS_ROOT_DIR "${prefix}")
    if(C_COMPONENTS)
        find_package(Gperftools QUIET COMPONENTS ${C_COMPONENTS})
    else()
        find_package(Gperftools QUIET)
    endif()
    math(EXPR n "${_cases} + 1")
    set(_cases ${n} PARENT_SCOPE)
    set(ok TRUE)
    if(expect_found AND NOT Gperftools_FOUND)
        set(ok FALSE)
    elseif(NOT expect_found AND Gperftools_FOUND)
        set(ok FALSE)
    endif()
    if(C_COMPONENT_FLAG)
        if(C_EXPECT_FLAG AND NOT ${C_COMPONENT_FLAG})
            set(ok FALSE)
        elseif(NOT C_EXPECT_FLAG AND ${C_COMPONENT_FLAG})
            set(ok FALSE)
        endif()
    endif()
    if(NOT ok)
        math(EXPR f "${_failed} + 1")
        set(_failed ${f} PARENT_SCOPE)
        message(SEND_ERROR "find-gperftools case '${name}': Gperftools_FOUND='${Gperftools_FOUND}' (expected "
                           "${expect_found}); ${C_COMPONENT_FLAG}='${${C_COMPONENT_FLAG}}' (expected ${C_EXPECT_FLAG})")
    else()
        message(STATUS "find-gperftools case '${name}': ok")
    endif()
endfunction()

set(_root "${CMAKE_CURRENT_BINARY_DIR}/prefixes")

# 1. Headers without any library are NOT gperftools: nothing to link, profiling must stay OFF.
make_prefix("${_root}/header-only" HEADER)
check_case("headers alone" "${_root}/header-only" FALSE)

# 2. Headers + libprofiler satisfy `COMPONENTS PROFILER`, and say so per component.
make_prefix("${_root}/profiler" HEADER LIBS profiler)
check_case("profiler requested and present" "${_root}/profiler" TRUE COMPONENTS PROFILER
           COMPONENT_FLAG Gperftools_PROFILER_FOUND EXPECT_FLAG TRUE)

# 3. tcmalloc alone does not satisfy a request for the profiler.
make_prefix("${_root}/tcmalloc" HEADER LIBS tcmalloc)
check_case("profiler requested, only tcmalloc present" "${_root}/tcmalloc" FALSE COMPONENTS PROFILER
           COMPONENT_FLAG Gperftools_PROFILER_FOUND EXPECT_FLAG FALSE)

# 4. tcmalloc alone IS gperftools when no component is requested (the documented "PROFILER or TCMALLOC").
check_case("tcmalloc, no component requested" "${_root}/tcmalloc" TRUE
           COMPONENT_FLAG Gperftools_TCMALLOC_FOUND EXPECT_FLAG TRUE)

if(_failed)
    message(FATAL_ERROR "find-gperftools: ${_failed} of ${_cases} case(s) failed")
endif()
message(STATUS "find-gperftools: ${_cases}/${_cases} cases hold")
