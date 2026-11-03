#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The consumer of an installed qb (Huly QB-1201): a languageless project that runs find_package(qb) against the prefix
# ../check.cmake generated in <RESULT_DIR>/prefix -- the qbConfig.cmake an install generates from
# cmake/qbConfig.cmake.in, and the files an install puts beside it, with stand-in Threads and nlohmann_json modules --
# with every search confined to the planted root and pkg-config off unless asked, and none of qb's own CMake loaded: an
# installed qb has to search its consumer's libraries on its own. Writes what was found, the cache entries, the
# configure's inputs and the module path find_package(qb) left (it puts its own directory there for its searches only)
# to <RESULT_DIR>/verdict.txt, and one line per configure to <RESULT_DIR>/configures.txt. Configured, and rebuilt, by
# ../check.cmake only.
#
#   cmake -S <this dir> -B <build> -DRESULT_DIR=<dir> -DPLANT_ROOT=<dir> [-DCMAKE_PREFIX_PATH=<under the root>]
#         [-D<entry>=.. cache entries] [-DOPTIONAL=ON] [-DFIND_TWICE=ON] [-DPACKAGE_NAME=QB]
#         [-DPARENT_TARGETS=<imported targets this project made before it asks for qb, '|'-separated>]
#         [-DUSE_PKG_CONFIG=ON, PKG_CONFIG_LIBDIR in the environment]
#         [-DSIBLINGS=<qb[+<target>,...], or a package found by the project itself>|... : side by side, in sibling/]
#         [-DCMP0124_OLD=ON: as a project whose cmake_minimum_required is before 3.21, foreach keeps its variable]
#         [-DSTALE_SAVED_MODULE_PATH=<path>: what an earlier find_package(qb) whose find_dependency() failed left]
#
cmake_minimum_required(VERSION 3.24)
project(qb_found_libraries_consumer NONE)

set(_prefix "${RESULT_DIR}/prefix")
if(NOT EXISTS "${_prefix}/lib/cmake/qb/qbConfig.cmake")
    message(FATAL_ERROR "no qbConfig.cmake generated in ${_prefix}/lib/cmake/qb")
endif()

set(CMAKE_FIND_ROOT_PATH "${PLANT_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
if(USE_PKG_CONFIG)
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF) # PKG_CONFIG_LIBDIR alone names the .pc files: ../fixture says why
else()
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    if(NOT COMMAND pkg_check_modules) # FindOpenSSL's unguarded call before CMake 3.31: ../fixture says why
        function(pkg_check_modules)
        endfunction()
    endif()
endif()
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH FALSE)
# FindThreads needs a language this project does not enable, and nlohmann_json's package is not planted: stand-ins.
list(PREPEND CMAKE_MODULE_PATH "${_prefix}/stand-ins")
if(CMP0124_OLD)
    cmake_policy(SET CMP0124 OLD)
endif()
if(DEFINED STALE_SAVED_MODULE_PATH) # qbConfig.cmake must not take it for a path it saved itself
    set(_qb_saved_module_path "${STALE_SAVED_MODULE_PATH}")
endif()
string(REPLACE "|" ";" _parents "${PARENT_TARGETS}")
foreach(_parent IN LISTS _parents) # a library this project found before it asked for qb
    add_library(${_parent} INTERFACE IMPORTED)
endforeach()

set(_name qb)
if(PACKAGE_NAME) # QB: the same files, on a file system that ignores case
    set(_name "${PACKAGE_NAME}")
endif()
set(_required REQUIRED)
if(OPTIONAL)
    set(_required "")
endif()
if(SIBLINGS) # directories side by side, each finding qb, or a package of the project's own (sibling/CMakeLists.txt)
    set(_siblings_leaked "") # what find_package(qb) left in each sibling's scope
    string(REPLACE "|" ";" _siblings "${SIBLINGS}")
    set(_index 0)
    foreach(_sibling IN LISTS _siblings)
        math(EXPR _index "${_index} + 1")
        add_subdirectory(sibling sibling-${_index})
    endforeach()
else()
    if(FIND_TWICE) # as two qbm packages each find_dependency(qb): the first does not require it
        find_package(${_name} CONFIG QUIET PATHS "${_prefix}/lib/cmake/qb" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
        set(_first "${${_name}_FOUND}|${${_name}_NOT_FOUND_MESSAGE}")
    endif()
    find_package(${_name} CONFIG ${_required} PATHS "${_prefix}/lib/cmake/qb" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()

file(APPEND "${RESULT_DIR}/configures.txt" "configured\n")
set(_entries "")
foreach(_entry IN ITEMS ARGON2_INCLUDE_DIR ARGON2_LIBRARY ZLIB_INCLUDE_DIR ZLIB_LIBRARY_RELEASE ZSTD_INCLUDE_DIR
                        ZSTD_LIBRARY BROTLI_INCLUDE_DIR BROTLI_ENCODER_LIBRARY BROTLI_DECODER_LIBRARY
                        BROTLI_COMMON_LIBRARY OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY
                        NGTCP2_INCLUDE_DIR NGTCP2_LIBRARY NGTCP2_CRYPTO_OSSL_LIBRARY)
    string(APPEND _entries "${_entry}=$CACHE{${_entry}}\n")
endforeach()
get_property(_inputs DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
list(JOIN _inputs "|" _inputs)
string(REPLACE "\n" " " _reason "${${_name}_NOT_FOUND_MESSAGE}")
string(REPLACE "\n" " " _first "${_first}")
get_cmake_property(_leaked VARIABLES) # what find_package(qb) left in this scope of its own internals
list(FILTER _leaked INCLUDE REGEX "^_qb_") # its cache records are _QB_..., and _qb_kept_said_<entry> says a path once
list(FILTER _leaked EXCLUDE REGEX "^_qb_kept_said_")
list(APPEND _leaked ${_siblings_leaked})
list(JOIN _leaked "|" _leaked)
set(_transitive "") # find_dependency()'s mark, which FeatureSummary reads: a dependency of qb's, not the project's own
foreach(_package IN ITEMS ZLIB OpenSSL Argon2 Zstd Brotli Ngtcp2)
    get_property(_mark GLOBAL PROPERTY _CMAKE_${_package}_TRANSITIVE_DEPENDENCY)
    string(APPEND _transitive "${_package}:${_mark} ")
endforeach()
file(WRITE "${RESULT_DIR}/verdict.txt"
     "QB=${${_name}_FOUND}\nREASON=${_reason}\nFIRST=${_first}\nLEAKED=|${_leaked}|\nTRANSITIVE=${_transitive}\n"
     "ARGON2=${Argon2_FOUND}\nZLIB=${ZLIB_FOUND}\nZSTD=${Zstd_FOUND}\n"
     "BROTLI=${Brotli_FOUND}\nOPENSSL=${OpenSSL_FOUND}\nNGTCP2=${Ngtcp2_FOUND}\nINPUTS=|${_inputs}|\n"
     "MODULE_PATH=${CMAKE_MODULE_PATH}\nIMPORT_PREFIX=${_IMPORT_PREFIX}\n${_entries}")
