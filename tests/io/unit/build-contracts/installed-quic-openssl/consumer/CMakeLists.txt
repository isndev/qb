#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The consumer of an installed qb, configured by ../check.cmake to run the remedies the generated qbConfig.cmake prints
# when it reports qb not found, as printed (Huly QB-1256): a languageless project whose searches are confined to a
# planted root -- CMake's FindOpenSSL, and the FindNgtcp2.cmake installed beside qbConfig.cmake reading the planted
# libraries through a stand-in tool -- with pkg-config off and stand-in Threads and nlohmann_json modules. It asks for
# qb as a consumer does, REQUIRED, and writes what was found to <RESULT_DIR>/verdict.txt.
#
#   cmake -S <this dir> -B <build> -DQB_DIR=<prefix>/lib/cmake/qb -DPLANT_ROOT=<dir> -DSTANDINS=<dir>
#         -DRESULT_DIR=<dir> [-DNGTCP2_OTOOL_EXECUTABLE=<stand-in> | -DCMAKE_READELF=<stand-in>]
#         [-DCMAKE_SYSTEM_NAME=Windows [-DMSVC=ON]] [-DOLD_POLICIES=ON]
#
cmake_minimum_required(VERSION 3.24)
if(OLD_POLICIES) # a consumer whose policies predate CMP0074 (CMake 3.12): its own scope ignores <Package>_ROOT
    cmake_policy(VERSION 3.11)
endif()
project(qb_installed_consumer NONE)

set(CMAKE_FIND_ROOT_PATH "${PLANT_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH FALSE)
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
# FindOpenSSL calls pkg_check_modules() whether pkg-config was found or not before CMake 3.31: off, the command is one
# that does nothing (as ../../quic-openssl-verdict/fixture/CMakeLists.txt has it).
if(NOT COMMAND pkg_check_modules)
    function(pkg_check_modules)
    endfunction()
endif()
list(APPEND CMAKE_MODULE_PATH "${STANDINS}")

find_package(qb CONFIG REQUIRED PATHS "${QB_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
file(WRITE "${RESULT_DIR}/verdict.txt"
     "QB=${qb_FOUND}\nOPENSSL_VERSION=${OPENSSL_VERSION}\nOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}\n"
     "OPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}\nNGTCP2_CRYPTO_OSSL_LIBRARY=${NGTCP2_CRYPTO_OSSL_LIBRARY}\n"
     "NGTCP2_OPENSSL=${Ngtcp2_OPENSSL_MAJOR}|${Ngtcp2_OPENSSL_CRYPTO_MAJOR}\n")
cmake_policy(GET CMP0074 _cmp0074)
file(APPEND "${RESULT_DIR}/verdict.txt" "POLICY_CMP0074=${_cmp0074}\n") # its scope's own setting, as qb found left it
