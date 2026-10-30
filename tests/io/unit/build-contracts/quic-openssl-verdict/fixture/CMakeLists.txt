#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The OpenSSL and QUIC decisions of qbDependencies.cmake (cmake/qbOpenSSL.cmake), driven with planted libraries (Huly
# QB-1185, QB-1190): a languageless project that runs, with every search confined to the planted root and pkg-config
# off, what qbDependencies.cmake runs -- _qb_find_openssl() when OPENSSL_ROOT_DIR names a planted OpenSSL or
# FIND_OPENSSL asks for the default search (else the OPENSSL_* entries given are the readings), _qb_check_openssl() and
# _qb_openssl_command() on the OpenSSL found or given, qb's own FindNgtcp2.cmake (or a parent's, PARENT_MODULE_DIR),
# then _qb_decide_quic() with qb's message helpers -- and writes what it decided to <RESULT_DIR>/verdict.txt and one
# line per configure to <RESULT_DIR>/configures.txt. The libraries are read through a stand-in tool, or the helper's
# readings are planted outright (PLANTED_NGTCP2) where nothing can be read. Configured, and rebuilt, by ../check.cmake
# only.
#
#   cmake -S <this dir> -B <build> -DQB_CMAKE_MODULE_DIR=<qb>/cmake -DRESULT_DIR=<dir> -DPLANT_ROOT=<dir>
#         [-DOPENSSL_ROOT_DIR=<under the root> | -DFIND_OPENSSL=ON | -DOPENSSL_SSL_LIBRARY=.. -DOPENSSL_CRYPTO_LIBRARY=..
#          -DOPENSSL_VERSION=.. -DOPENSSL_INCLUDE_DIR=..]
#         [-DNGTCP2_...=.. cache entries] [-DCMAKE_PREFIX_PATH=<under the root>] [-DPARENT_MODULE_DIR=<dir>]
#         [-DPLANTED_NGTCP2=<ssl major>|<ssl path>|<crypto major>|<crypto path>]
#         [-DNGTCP2_OTOOL_EXECUTABLE=<stand-in> | -DCMAKE_READELF=<stand-in>] [-DQB_WITH_QUIC=..] [-DQB_REQUIRE_FEATURES=..]
#         [-DUSE_PKG_CONFIG=ON, PKG_CONFIG_LIBDIR in the environment] [-DCMAKE_SYSTEM_NAME=<another system>]
#         [-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=<mode>, NEVER otherwise]
#         [-DPARENT_OPENSSL_EXECUTABLE=<a parent project's OPENSSL_EXECUTABLE variable>] [-DPARENT_FINDS_OPENSSL=ON]
#         [-DPARENT_FINDS_NOTHING=ON] [-DLATE_FINDS_OPENSSL=ON]
#
cmake_minimum_required(VERSION 3.24)
project(qb_quic_openssl_verdict NONE)

if(NOT QB_CMAKE_MODULE_DIR OR NOT EXISTS "${QB_CMAKE_MODULE_DIR}/qbOpenSSL.cmake")
    message(FATAL_ERROR "QB_CMAKE_MODULE_DIR must name qb/cmake (got '${QB_CMAKE_MODULE_DIR}')")
endif()
include("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake") # QB_WITH_QUIC, QB_REQUIRE_FEATURES and the message helpers

set(CMAKE_FIND_ROOT_PATH "${PLANT_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
if(NOT DEFINED CMAKE_FIND_ROOT_PATH_MODE_PROGRAM)
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER) # as a cross toolchain sets it: a program is searched where it is named
endif()
if(USE_PKG_CONFIG)
    # PKG_CONFIG_LIBDIR names the planted .pc files only, and FindPkgConfig adds nothing ahead of it: by default it
    # searches the lib/pkgconfig of every CMAKE_PREFIX_PATH entry first, the environment's too, and the GitHub macOS
    # runner exports one naming Homebrew's OpenSSL and libngtcp2 -- answers the confined searches find nowhere.
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)
else()
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    # FindOpenSSL calls pkg_check_modules() whether find_package(PkgConfig) found pkg-config or not before CMake 3.31,
    # which asks only once it is found: turned off, FindPkgConfig is never read and defines no such command, and its
    # search would stop on it, from the 3.24 floor to 3.30. A command that does nothing is the pkg-config that is off,
    # as 3.31 has it; every other search here asks only once found.
    if(NOT COMMAND pkg_check_modules)
        function(pkg_check_modules)
        endfunction()
    endif()
endif()
list(PREPEND CMAKE_MODULE_PATH "${QB_CMAKE_MODULE_DIR}")
if(PARENT_MODULE_DIR) # a parent project's FindNgtcp2 module, first on the path: it wins the search
    list(PREPEND CMAKE_MODULE_PATH "${PARENT_MODULE_DIR}")
endif()

include("${QB_CMAKE_MODULE_DIR}/qbOpenSSL.cmake")
if(DEFINED OPENSSL_ROOT_DIR OR FIND_OPENSSL)
    _qb_find_openssl()
endif()
if(OpenSSL_FOUND OR NOT (DEFINED OPENSSL_ROOT_DIR OR FIND_OPENSSL)) # as qbDependencies.cmake checks the one it found
    _qb_check_openssl()
    if(DEFINED PARENT_OPENSSL_EXECUTABLE) # a parent project's variable
        set(OPENSSL_EXECUTABLE "${PARENT_OPENSSL_EXECUTABLE}")
    endif()
    if(PARENT_FINDS_OPENSSL) # a parent project's own search, before qb's
        find_program(OPENSSL_EXECUTABLE openssl)
    endif()
    if(PARENT_FINDS_NOTHING) # ... one that finds nothing
        find_program(OPENSSL_EXECUTABLE qb-no-such-openssl)
    endif()
    _qb_openssl_command(_command _by_hand) # as tests/io/system/CMakeLists.txt finds the test certificates' command
    if(LATE_FINDS_OPENSSL) # a consumer's own search, after qb's
        find_program(OPENSSL_EXECUTABLE openssl)
        set(_late "${OPENSSL_EXECUTABLE}")
    endif()
endif()

find_package(Ngtcp2 QUIET)
if(FIND_NGTCP2_TWICE) # as an installed qb's qbConfig.cmake runs it: find_package(), then find_dependency()
    find_package(Ngtcp2 QUIET)
endif()
if(DEFINED PLANTED_NGTCP2)
    string(REPLACE "|" ";" _planted "${PLANTED_NGTCP2}")
    list(GET _planted 0 Ngtcp2_OPENSSL_MAJOR)
    list(GET _planted 1 Ngtcp2_OPENSSL_LIBRARY)
    list(GET _planted 2 Ngtcp2_OPENSSL_CRYPTO_MAJOR)
    list(GET _planted 3 Ngtcp2_OPENSSL_CRYPTO_LIBRARY)
endif()

# As qbDependencies.cmake reaches the decision: SSL found, QB_WITH_QUIC not OFF.
set(QB_EXTERNAL_LIBRARIES "")
set(_qb_quic_compared FALSE)
string(TOUPPER "${QB_WITH_QUIC}" _qb_quic_mode)
if(_qb_quic_mode MATCHES "^(OFF|FALSE|0|NO|N)$")
    set(QB_HAS_QUIC FALSE)
else()
    set(_qb_quic_required TRUE)
    if(_qb_quic_mode STREQUAL "AUTO")
        set(_qb_quic_required FALSE)
    endif()
    _qb_decide_quic()
endif()

file(APPEND "${RESULT_DIR}/configures.txt" "configured\n")
set(_entries "NGTCP2_VANISHED=${Ngtcp2_VANISHED}\n") # FindNgtcp2's own answer, which a consumer of an installed qb reads
foreach(_entry IN ITEMS NGTCP2_INCLUDE_DIR NGTCP2_LIBRARY NGTCP2_CRYPTO_OSSL_LIBRARY NGTCP2_CRYPTO_INCLUDE_DIR
                        NGTCP2_CRYPTO_OSSL_INCLUDE_DIR OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
    string(APPEND _entries "${_entry}=$CACHE{${_entry}}\n")
endforeach()
file(WRITE "${RESULT_DIR}/verdict.txt"
     "QB_HAS_QUIC=${QB_HAS_QUIC}\nVERDICT=${_qb_quic_conflict}\nCOMPARED=${_qb_quic_compared}\n"
     "LIBRARIES=${QB_EXTERNAL_LIBRARIES}\nNGTCP2=${Ngtcp2_FOUND}\nOPENSSL=${OpenSSL_FOUND}\n"
     "OPENSSL_COMPARED=${_qb_openssl_compared}\nOPENSSL_READ=${_qb_openssl_ssl_read}|${_qb_openssl_crypto_read}\n"
     "OPENSSL_COMMAND=${_command}\nOPENSSL_COMMAND_BY_HAND=${_by_hand}\n"
     "OPENSSL_EXECUTABLE_ENTRY=$CACHE{OPENSSL_EXECUTABLE}\nLATE_FOUND=${_late}\n"
     "${_entries}")
