#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The architecture qb publishes (QB_ARCH, QB_ARCH_64 / _32, QB_ARCH_ARM / _ARM64 / _ARM32, and the QB_ARCH_* compile
# definitions) is the one the COMPILER targets (Huly QB-1004). It was read from CMAKE_SYSTEM_PROCESSOR, which CMake
# documents as the HOST's processor unless a toolchain file cross-compiles: a Visual Studio `-A ARM64` configure on an
# x64 host -- or Ninja under an x64_arm64 developer prompt, or clang-cl --target=aarch64-pc-windows-msvc -- built arm64
# code with QB_ARCH "x64" and no QB_ARCH_ARM64, which cpu-topology.cpp's preprocessor check refuses. An MSVC-family
# compiler names its target in CMAKE_CXX_COMPILER_ARCHITECTURE_ID (the variable CMake's own MSVC rules derive /machine
# from); every other toolchain names it in CMAKE_SYSTEM_PROCESSOR.
#
# Checked by CONFIGURING this languageless project with the toolchain identity fabricated per case: no compiler, no
# build. Run by ctest as qb-io-test-unit-target-arch (tests/io/unit/CMakeLists.txt); a mismatch is a SEND_ERROR.
#
#   cmake --fresh -S <this dir> -B <build> -DQB_CMAKE_MODULE_DIR=<qb>/cmake
#
cmake_minimum_required(VERSION 3.24)
project(qb_target_arch_contract NONE)

if(NOT QB_CMAKE_MODULE_DIR OR NOT EXISTS "${QB_CMAKE_MODULE_DIR}/qbConfig.cmake")
    message(FATAL_ERROR "QB_CMAKE_MODULE_DIR must name qb/cmake (got '${QB_CMAKE_MODULE_DIR}')")
endif()
include("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake")

set(_qb_cases 0)
set(_qb_failed 0)

# _qb_arch_case(<name> <msvc> <compiler arch id> <system processor> <sizeof void*> <QB_ARCH> <ARM64> <ARM32>)
function(_qb_arch_case name msvc arch_id processor pointer_size want_arch want_arm64 want_arm32)
    set(MSVC ${msvc})
    set(CMAKE_CXX_COMPILER_ID "")
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "${arch_id}")
    set(CMAKE_SYSTEM_PROCESSOR "${processor}")
    set(CMAKE_SIZEOF_VOID_P ${pointer_size})
    unset(CMAKE_OSX_ARCHITECTURES)
    qb_initialize_project_configuration()

    set(_got "QB_ARCH=${QB_ARCH} ARM64=${QB_ARCH_ARM64} ARM32=${QB_ARCH_ARM32} 64=${QB_ARCH_64}")
    if(pointer_size EQUAL 8)
        set(_want_64 TRUE)
    else()
        set(_want_64 FALSE)
    endif()
    set(_want "QB_ARCH=${want_arch} ARM64=${want_arm64} ARM32=${want_arm32} 64=${_want_64}")
    if(want_arm64)
        set(_want_definition TRUE)
    else()
        set(_want_definition FALSE)
    endif()
    if("QB_ARCH_ARM64=1" IN_LIST QB_COMPILE_DEFINITIONS)
        set(_got_definition TRUE)
    else()
        set(_got_definition FALSE)
    endif()
    math(EXPR _n "${_qb_cases} + 1")
    set(_qb_cases ${_n} PARENT_SCOPE)
    if(NOT _got STREQUAL _want OR NOT _got_definition STREQUAL _want_definition)
        message(SEND_ERROR "target-arch: ${name}\n  got  ${_got} (definition QB_ARCH_ARM64=1: ${_got_definition})\n"
                           "  want ${_want} (definition QB_ARCH_ARM64=1: ${_want_definition})")
        math(EXPR _f "${_qb_failed} + 1")
        set(_qb_failed ${_f} PARENT_SCOPE)
    endif()
endfunction()

# MSVC-family compilers: the target is the compiler's, whatever the host.
_qb_arch_case("MSVC x64 on an x64 host"                     TRUE  "x64"   "AMD64" 8 "x64"   FALSE FALSE)
_qb_arch_case("MSVC ARM64 on an x64 host (VS -A ARM64)"     TRUE  "ARM64" "AMD64" 8 "arm64" TRUE  FALSE)
_qb_arch_case("MSVC ARM64 on an ARM64 host"                 TRUE  "ARM64" "ARM64" 8 "arm64" TRUE  FALSE)
_qb_arch_case("MSVC x64 on an ARM64 host"                   TRUE  "x64"   "ARM64" 8 "x64"   FALSE FALSE)
_qb_arch_case("MSVC x86 on an x64 host (VS -A Win32)"       TRUE  "X86"   "AMD64" 4 "x86"   FALSE FALSE)
_qb_arch_case("MSVC ARM64EC (an x64 ABI) on an x64 host"    TRUE  "ARM64EC" "AMD64" 8 "x64" FALSE FALSE)
_qb_arch_case("MSVC ARMv7 on an x64 host"                   TRUE  "ARMV7" "AMD64" 4 "arm"   FALSE TRUE)
_qb_arch_case("MSVC with no architecture id: the processor" TRUE  ""      "AMD64" 8 "x64"   FALSE FALSE)
# Every other toolchain: the processor, which a cross toolchain file sets to the target.
_qb_arch_case("GNU x86_64"                                  FALSE ""      "x86_64"  8 "x64"   FALSE FALSE)
_qb_arch_case("GNU aarch64"                                 FALSE ""      "aarch64" 8 "arm64" TRUE  FALSE)
_qb_arch_case("Clang arm64 (macOS, one architecture)"       FALSE ""      "arm64"   8 "arm64" TRUE  FALSE)
_qb_arch_case("GNU armv7l"                                  FALSE ""      "armv7l"  4 "arm"   FALSE TRUE)
_qb_arch_case("an id that a non-MSVC compiler reports is not read" FALSE "ARM64" "x86_64" 8 "x64" FALSE FALSE)

if(_qb_failed)
    message(SEND_ERROR "target-arch: ${_qb_failed} of ${_qb_cases} cases failed")
else()
    message(STATUS "target-arch: ${_qb_cases} cases, QB_ARCH follows the compiler's target")
endif()
