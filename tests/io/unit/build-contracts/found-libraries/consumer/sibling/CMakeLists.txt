#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# One directory of ../CMakeLists.txt's consumer (SIBLINGS=...), added beside the others: it finds qb, or <_sibling> as a
# package of the project's own, through qb's Find module when CMake ships none. An imported target is seen only in the
# directory that made it and below, so qb's searches run again in each directory that finds qb. What find_package(qb)
# left of its own in this directory's scope goes to the parent's _siblings_leaked, which its LEAKED line reads.
#
if(_sibling MATCHES "^qb(\\+(.*))?$") # qb+<target>,<target>: this directory made those imported targets first
    string(REPLACE "," ";" _own "${CMAKE_MATCH_2}")
    foreach(_target IN LISTS _own)
        add_library(${_target} INTERFACE IMPORTED)
    endforeach()
    find_package(qb CONFIG REQUIRED PATHS "${_prefix}/lib/cmake/qb" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    set(qb_FOUND "${qb_FOUND}" PARENT_SCOPE)
    get_cmake_property(_leaked VARIABLES)
    list(FILTER _leaked INCLUDE REGEX "^_qb_")
    list(FILTER _leaked EXCLUDE REGEX "^_qb_kept_said_")
    list(APPEND _siblings_leaked ${_leaked})
    set(_siblings_leaked "${_siblings_leaked}" PARENT_SCOPE)
else()
    list(APPEND CMAKE_MODULE_PATH "${_prefix}/lib/cmake/qb")
    find_package(${_sibling} REQUIRED)
endif()
