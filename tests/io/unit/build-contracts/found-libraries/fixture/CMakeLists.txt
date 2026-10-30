#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The searches qbDependencies.cmake runs for Argon2, zlib, zstd and brotli (cmake/qbFoundLibraries.cmake), driven with
# planted libraries (Huly QB-1192): a languageless project that runs, with every search confined to the planted root and
# pkg-config off unless asked, _qb_find_argon2(), _qb_find_zlib(), _qb_find_zstd(), _qb_find_brotli() and OpenSSL's
# _qb_find_openssl() for each name FIND lists, in its order -- qb's own Find modules and CMake's FindZLIB and
# FindOpenSSL, with qb's message helpers -- and writes what was found, the
# cache entries and the configure's inputs to <RESULT_DIR>/verdict.txt, and one line per configure to
# <RESULT_DIR>/configures.txt. Configured, and rebuilt, by ../check.cmake only.
#
#   cmake -S <this dir> -B <build> -DQB_CMAKE_MODULE_DIR=<qb>/cmake -DRESULT_DIR=<dir> -DPLANT_ROOT=<dir>
#         -DFIND=<argon2|zlib|zstd|brotli|openssl, '|'-separated> [-D<entry>=.. cache entries]
#         [-DCMAKE_PREFIX_PATH=<under the root>]
#         [-DQB_DEPS_FETCH_FALLBACK=<bool>] [-DPARENT_ZLIB_TARGET=ON]
#         [-DUSE_PKG_CONFIG=ON, PKG_CONFIG_LIBDIR in the environment]
#
cmake_minimum_required(VERSION 3.24)
project(qb_found_libraries NONE)

if(NOT QB_CMAKE_MODULE_DIR OR NOT EXISTS "${QB_CMAKE_MODULE_DIR}/qbFoundLibraries.cmake")
    message(FATAL_ERROR "QB_CMAKE_MODULE_DIR must name qb/cmake (got '${QB_CMAKE_MODULE_DIR}')")
endif()
include("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake") # the message helpers

set(CMAKE_FIND_ROOT_PATH "${PLANT_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
if(USE_PKG_CONFIG)
    # PKG_CONFIG_LIBDIR names the planted .pc files only, and FindPkgConfig adds nothing ahead of it: by default it
    # searches the lib/pkgconfig of every CMAKE_PREFIX_PATH entry first, the environment's too, and the GitHub macOS
    # runner exports one naming Homebrew's packages -- answers the confined searches find nowhere.
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)
else()
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    # FindOpenSSL calls pkg_check_modules() whether find_package(PkgConfig) found pkg-config or not before CMake 3.31,
    # which asks only once it is found: turned off, FindPkgConfig is never read and defines no such command, and OpenSSL's
    # search would stop on it, from the 3.24 floor to 3.30. A command that does nothing is the pkg-config that is off,
    # as 3.31 has it; every other search here asks only once found.
    if(NOT COMMAND pkg_check_modules)
        function(pkg_check_modules)
        endfunction()
    endif()
endif()
list(PREPEND CMAKE_MODULE_PATH "${QB_CMAKE_MODULE_DIR}")
# FindOpenSSL searches Threads for a static OpenSSL on Linux, and FindThreads needs a language this project does not
# enable: it links nothing, so nothing needs Threads::Threads.
set(CMAKE_DISABLE_FIND_PACKAGE_Threads TRUE)

include("${QB_CMAKE_MODULE_DIR}/qbFoundLibraries.cmake")
include("${QB_CMAKE_MODULE_DIR}/qbOpenSSL.cmake")
if(PARENT_ZLIB_TARGET) # a parent project's zlib, as a superbuild provides it
    add_library(ZLIB::ZLIB INTERFACE IMPORTED)
endif()
string(REPLACE "|" ";" _find "${FIND}")
foreach(_name IN LISTS _find)
    if(_name STREQUAL "argon2")
        _qb_find_argon2()
    elseif(_name STREQUAL "zlib")
        _qb_find_zlib()
    elseif(_name STREQUAL "zstd")
        _qb_find_zstd()
    elseif(_name STREQUAL "brotli")
        _qb_find_brotli()
    elseif(_name STREQUAL "openssl")
        _qb_find_openssl()
    else()
        message(FATAL_ERROR "FIND names '${_name}', not argon2, zlib, zstd, brotli or openssl")
    endif()
endforeach()

file(APPEND "${RESULT_DIR}/configures.txt" "configured\n")
set(_entries "")
foreach(_entry IN ITEMS ARGON2_INCLUDE_DIR ARGON2_LIBRARY ZLIB_INCLUDE_DIR ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG ZSTD_INCLUDE_DIR
                        ZSTD_LIBRARY BROTLI_INCLUDE_DIR BROTLI_ENCODER_LIBRARY BROTLI_DECODER_LIBRARY
                        BROTLI_COMMON_LIBRARY)
    string(APPEND _entries "${_entry}=$CACHE{${_entry}}\n")
endforeach()
get_property(_inputs DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
list(JOIN _inputs "|" _inputs)
file(WRITE "${RESULT_DIR}/verdict.txt"
     "ARGON2=${Argon2_FOUND}\nZLIB=${ZLIB_FOUND}\nZSTD=${Zstd_FOUND}\nBROTLI=${Brotli_FOUND}\n"
     "OPENSSL=${OpenSSL_FOUND}\nOPENSSL_LINKS_ZLIB=${_qb_openssl_links_zlib}\n"
     "INPUTS=|${_inputs}|\n${_entries}")
