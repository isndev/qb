# qb-io-test-unit-ngtcp2-openssl-listing (Huly QB-1179, QB-1185, QB-1190). FindNgtcp2.cmake reads which OpenSSL
# libngtcp2_crypto_ossl links, so that qbDependencies.cmake disables QUIC when qb links another major: two OpenSSL majors
# in one process crash at the first QUIC handshake (two copies of one major are reported, not refused). This runs the
# module's parser on the listings each platform's tool prints, for libssl and for libcrypto, its verdict on the readings
# of both sides and its tool fallbacks and Mach-O install names (stand-in tools, off Windows), then the real libraries:
# qb's own libssl and libcrypto must read wherever their files are versioned and a tool exists; qb's OpenSSL check must
# have compared them on Linux and macOS (LibreSSL apart), read what the module reads now, and made each file it read an
# input of the configure; and -- when the configure compared libngtcp2's with them -- what the module reads now must be
# what the configure read, the verdict the one it acted on, QUIC enabled unless they are two majors, and every file read
# an input of the configure.
#
#   cmake -DFIND_MODULE=<FindNgtcp2.cmake> [-DLIBRARY=<libngtcp2_crypto_ossl>] [-DQB_SSL_LIBRARY=<qb's libssl>]
#         [-DQB_CRYPTO_LIBRARY=<qb's libcrypto>] [-DQB_OPENSSL_INCLUDE_DIR=<its headers>] [-DQB_OPENSSL_VERSION=<x.y.z>]
#         [-DQB_HAS_SSL=<bool>] [-DQB_OPENSSL_COMPARED=<bool> -DQB_OPENSSL_HEADERS_MAJOR=..
#          -DQB_OPENSSL_SSL_READ=.. -DQB_OPENSSL_CRYPTO_READ=.. -DQB_OPENSSL_CHECK_INPUTS=<its inputs, '|'-separated>]
#         [-DQB_QUIC_VERDICT=<the configure's>]
#         [-DWORK_DIR=<scratch directory>]
#         [-DQB_QUIC_NGTCP2_MAJOR=.. -DQB_QUIC_NGTCP2_PATH=.. -DQB_QUIC_QB_MAJOR=.. -DQB_QUIC_QB_PATH=..]
#         [-DQB_QUIC_NGTCP2_CRYPTO_MAJOR=.. -DQB_QUIC_NGTCP2_CRYPTO_PATH=.. -DQB_QUIC_QB_CRYPTO_MAJOR=..
#          -DQB_QUIC_QB_CRYPTO_PATH=..] [-DQB_QUIC_INPUTS=<the configure's inputs, '|'-separated>]
#         [-DQB_QUIC_FOUND_BY_QB=TRUE -DQB_QUIC_COMPARED=<bool>]
#         [-DQB_HAS_QUIC=<bool>] [-DQB_TARGET_APPLE=<bool>] [-DQB_TARGET_WIN32=<bool>] [-DQB_TARGET_SYSTEM=<name>]
#         [-DCMAKE_READELF=<tool>] [-DCMAKE_OBJDUMP=<tool>] -P check.cmake

cmake_minimum_required(VERSION 3.24) # script mode sets no policy otherwise: CMP0054 and the others as qb configures

# The target's platform, not the host's: script mode defines APPLE and WIN32 for the host it runs on, and a -D of the
# same name does not override them (a cross build reads its target's kind of library; the tools found are the host's).
if(DEFINED QB_TARGET_APPLE)
    set(APPLE "${QB_TARGET_APPLE}")
endif()
if(DEFINED QB_TARGET_WIN32)
    set(WIN32 "${QB_TARGET_WIN32}")
endif()

if(NOT FIND_MODULE OR NOT EXISTS "${FIND_MODULE}")
    message(FATAL_ERROR "ngtcp2-openssl-listing: FIND_MODULE (FindNgtcp2.cmake) is required")
endif()
set(NGTCP2_FIND_FUNCTIONS_ONLY ON)
include("${FIND_MODULE}")
if(NOT COMMAND _ngtcp2_openssl_in_listing OR NOT COMMAND _ngtcp2_linked_openssl OR NOT COMMAND _ngtcp2_openssl_conflict)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${FIND_MODULE} defines no OpenSSL listing reader")
endif()

set(_failures "")
set(_cases 0)
set(_expect_name ssl)
# <label> <listing> <expected major> <expected path> -- of the listing's libssl; expect_crypto(), of its libcrypto
function(expect label listing major path)
    _ngtcp2_openssl_in_listing("${listing}" ${_expect_name} _major _path)
    math(EXPR _n "${_cases} + 1")
    set(_cases ${_n} PARENT_SCOPE)
    if(NOT "${_major}" STREQUAL "${major}" OR NOT "${_path}" STREQUAL "${path}")
        string(APPEND _failures "\n  ${label} (lib${_expect_name}): read major '${_major}' path '${_path}', "
                                "expected '${major}' '${path}'")
        set(_failures "${_failures}" PARENT_SCOPE)
    endif()
endfunction()
macro(expect_crypto label listing major path)
    set(_expect_name crypto)
    expect("${label}" "${listing}" "${major}" "${path}")
    set(_expect_name ssl)
endmacro()

# otool -L, as macOS prints it for Homebrew's libngtcp2 after its move onto openssl@4 (measured 2026-10-28).
expect("otool, OpenSSL 4" "/opt/homebrew/opt/libngtcp2/lib/libngtcp2_crypto_ossl.dylib:
\t/opt/homebrew/opt/libngtcp2/lib/libngtcp2_crypto_ossl.0.dylib (compatibility version 4.0.0, current version 4.1.0)
\t/opt/homebrew/Cellar/libngtcp2/1.25.0_1/lib/libngtcp2.16.dylib (compatibility version 28.0.0, current version 28.0.0)
\t/opt/homebrew/opt/openssl@4/lib/libssl.4.dylib (compatibility version 4.0.0, current version 4.0.0)
\t/opt/homebrew/opt/openssl@4/lib/libcrypto.4.dylib (compatibility version 4.0.0, current version 4.0.0)
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)"
       "4" "/opt/homebrew/opt/openssl@4/lib/libssl.4.dylib")
expect("otool, OpenSSL 3" "\t/usr/local/opt/openssl@3/lib/libssl.3.dylib (compatibility version 3.0.0, current version 3.0.0)"
       "3" "/usr/local/opt/openssl@3/lib/libssl.3.dylib")
expect("otool, OpenSSL 1.1" "\t/usr/local/opt/openssl@1.1/lib/libssl.1.1.dylib (compatibility version 1.1.0, current version 1.1.0)"
       "1" "/usr/local/opt/openssl@1.1/lib/libssl.1.1.dylib")
# readelf -d and objdump -p, as GNU binutils print the NEEDED entries: a soname, no path.
expect("readelf, OpenSSL 3" " 0x0000000000000001 (NEEDED)             Shared library: [libngtcp2.so.16]
 0x0000000000000001 (NEEDED)             Shared library: [libssl.so.3]
 0x0000000000000001 (NEEDED)             Shared library: [libcrypto.so.3]" "3" "")
expect("objdump, OpenSSL 1.1" "Dynamic Section:
  NEEDED               libssl.so.1.1
  NEEDED               libcrypto.so.1.1" "1" "")
# A libssl read for itself -- qb's own, compared with libngtcp2's: otool lists its install name first, readelf its
# SONAME.
expect("otool, a libssl itself" "/opt/homebrew/opt/openssl@4/lib/libssl.dylib:
\t/opt/homebrew/opt/openssl@4/lib/libssl.4.dylib (compatibility version 4.0.0, current version 4.0.0)
\t/opt/homebrew/Cellar/openssl@4/4.1.0/lib/libcrypto.4.dylib (compatibility version 4.0.0, current version 4.0.0)"
       "4" "/opt/homebrew/opt/openssl@4/lib/libssl.4.dylib")
expect("readelf, a libssl itself" " 0x000000000000000e (SONAME)             Library soname: [libssl.so.3]
 0x0000000000000001 (NEEDED)             Shared library: [libcrypto.so.3]" "3" "")
expect("otool, a path with spaces" "\t/Applications/My Tools/lib/libssl.3.dylib (compatibility version 3.0.0, current version 3.0.0)"
       "3" "/Applications/My Tools/lib/libssl.3.dylib")
# What must read as nothing: no libssl, an unversioned name, a library merely named like it, an empty listing.
expect("libcrypto only" "\t/opt/x/lib/libcrypto.3.dylib (compatibility version 3.0.0)" "" "")
expect("unversioned name" "\t/opt/x/lib/libssl.dylib (compatibility version 3.0.0)" "" "")
expect("look-alike" "  NEEDED               libsslkeylog.so.2" "" "")
expect("ELF look-alike" "  NEEDED               notlibssl.so.9" "" "")
expect("ELF, two libssl entries" "  NEEDED               libssl.so.3
  NEEDED               libssl.so.1.1" "3" "")
expect("ELF, a libssl named by its path" " 0x0000000000000001 (NEEDED)             Shared library: [/opt/x/lib/libssl.so.3]"
       "3" "")
expect("an RPATH named like a libssl" " 0x000000000000000f (RPATH)              Library rpath: [libssl.so.9-dir]
 0x0000000000000001 (NEEDED)             Shared library: [libssl.so.3]" "3" "")
expect("empty listing" "" "" "")
expect("Mach-O look-alike" "\t/opt/x/lib/notlibssl.3.dylib (compatibility version 3.0.0)" "" "")
# objdump's heading names the file it read: a directory named like a libssl is not one.
expect("objdump, a decoy heading" "/opt/libssl.so.9-decoy/libngtcp2_crypto_ossl.so.0:     file format elf64-x86-64

Dynamic Section:
  NEEDED               libssl.so.3" "3" "")
# The first libssl entry decides; a line is a line, even after an unbalanced '[' (a CMake list would join them).
expect("two libssl entries" "\t/opt/a/lib/libssl.4.dylib (compatibility version 4.0.0)
\t/opt/b/lib/libssl.3.dylib (compatibility version 3.0.0)" "4" "/opt/a/lib/libssl.4.dylib")
expect("an unbalanced bracket" " 0x000000000000001d (RUNPATH)            Library runpath: [/opt/x[
 0x0000000000000001 (NEEDED)             Shared library: [libcrypto.so.3]
 0x000000000000001d (RUNPATH)            Library runpath: [/opt/libssl.so.9/lib]
 0x0000000000000001 (NEEDED)             Shared library: [libssl.so.3]" "3" "")
# The libcrypto beside it (Huly QB-1185): read from the same listings, and from a libcrypto read for itself -- qb's own,
# where a half-cleared cache can leave it on another major than qb's libssl.
expect_crypto("otool, OpenSSL 4" "/opt/homebrew/opt/libngtcp2/lib/libngtcp2_crypto_ossl.dylib:
\t/opt/homebrew/opt/libngtcp2/lib/libngtcp2_crypto_ossl.0.dylib (compatibility version 4.0.0, current version 4.1.0)
\t/opt/homebrew/opt/openssl@4/lib/libssl.4.dylib (compatibility version 4.0.0, current version 4.0.0)
\t/opt/homebrew/opt/openssl@4/lib/libcrypto.4.dylib (compatibility version 4.0.0, current version 4.0.0)"
              "4" "/opt/homebrew/opt/openssl@4/lib/libcrypto.4.dylib")
expect_crypto("readelf, OpenSSL 3" " 0x0000000000000001 (NEEDED)             Shared library: [libssl.so.3]
 0x0000000000000001 (NEEDED)             Shared library: [libcrypto.so.3]" "3" "")
expect_crypto("objdump, OpenSSL 1.1" "Dynamic Section:
  NEEDED               libssl.so.1.1
  NEEDED               libcrypto.so.1.1" "1" "")
expect_crypto("otool, a libssl itself" "/opt/homebrew/opt/openssl@4/lib/libssl.dylib:
\t/opt/homebrew/opt/openssl@4/lib/libssl.4.dylib (compatibility version 4.0.0, current version 4.0.0)
\t/opt/homebrew/Cellar/openssl@4/4.1.0/lib/libcrypto.4.dylib (compatibility version 4.0.0, current version 4.0.0)"
              "4" "/opt/homebrew/Cellar/openssl@4/4.1.0/lib/libcrypto.4.dylib")
expect_crypto("otool, a libcrypto itself" "/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib:
\t/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib (compatibility version 3.0.0, current version 3.0.0)
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)"
              "3" "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib")
expect("otool, a libcrypto itself" "/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib:
\t/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib (compatibility version 3.0.0, current version 3.0.0)" "" "")
expect_crypto("readelf, a libcrypto itself" " 0x000000000000000e (SONAME)             Library soname: [libcrypto.so.3]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]" "3" "")
expect_crypto("libssl only" "\t/opt/x/lib/libssl.3.dylib (compatibility version 3.0.0)" "" "")
expect_crypto("unversioned name" "\t/opt/x/lib/libcrypto.dylib (compatibility version 3.0.0)" "" "")
expect_crypto("look-alike" "  NEEDED               libcryptopp.so.8" "" "")
expect_crypto("ELF look-alike" "  NEEDED               notlibcrypto.so.9" "" "")
expect_crypto("Mach-O look-alike" "\t/opt/x/lib/libcrypto_extra.3.dylib (compatibility version 3.0.0)" "" "")
expect_crypto("ELF, a libcrypto named by its path" " 0x0000000000000001 (NEEDED)             Shared library: [/opt/x/lib/libcrypto.so.3]"
              "3" "")
# PE (Huly QB-1184): the DLLs a Windows binary imports, a name per line in `dumpbin /DEPENDENTS`, after "DLL Name: " in
# `objdump -p`'s import tables, GNU's and LLVM's -- as each printed vcpkg's ngtcp2_crypto_ossl.dll (dumpbin 14.51 and
# llvm-objdump 22, measured 2026-11-06; GNU's from binutils' format) -- with CRLF line ends, as dumpbin writes them.
# OpenSSL names its DLLs lib<name>-<major>[_<minor>][-<arch>].dll, in any case: x64, arm64, x86 (no suffix), 1.1.
set(_dumpbin "Microsoft (R) COFF/PE Dumper Version 14.51.36256.0\r
Copyright (C) Microsoft Corporation.  All rights reserved.\r
\r
\r
Dump of file D:\\vcpkg_installed\\x64-windows\\bin\\ngtcp2_crypto_ossl.dll\r
\r
File Type: DLL\r
\r
  Image has the following dependencies:\r
\r
    ngtcp2.dll\r
    libssl-4-x64.dll\r
    libcrypto-4-x64.dll\r
    VCRUNTIME140.dll\r
    api-ms-win-crt-runtime-l1-1-0.dll\r
    KERNEL32.dll\r
\r
  Summary\r
\r
        1000 .data\r
        6000 .text\r
")
expect("dumpbin, OpenSSL 4" "${_dumpbin}" "4" "")
set(_expect_name crypto) # not expect_crypto(): a macro re-reads the backslashes of its arguments as escapes
expect("dumpbin, OpenSSL 4" "${_dumpbin}" "4" "")
set(_expect_name ssl)
set(_gnu_objdump "The Import Tables (interpreted .idata section contents)
 vma:            Hint    Time      Forward  DLL       First
                 Table   Stamp     Chain     Name      Thunk
 0001e000\t0001e050 00000000 00000000 0001ea10 0001e1b8

\tDLL Name: libngtcp2-16.dll
\tvma:  Hint/Ord Member-Name Bound-To
\t1e3a0\t  112  ngtcp2_conn_get_tls_native_handle

 0001e014\t0001e068 00000000 00000000 0001ea2c 0001e1d0

\tDLL Name: libcrypto-3-x64.dll
\tvma:  Hint/Ord Member-Name Bound-To
\t1e3b8\t 1826  EVP_KDF_derive

 0001e028\t0001e080 00000000 00000000 0001ea48 0001e1e8

\tDLL Name: libssl-3-x64.dll
\tvma:  Hint/Ord Member-Name Bound-To
\t1e3d0\t  529  SSL_set_quic_tls_cbs
")
expect("objdump (GNU), OpenSSL 3" "${_gnu_objdump}" "3" "")
expect_crypto("objdump (GNU), OpenSSL 3" "${_gnu_objdump}" "3" "")
set(_llvm_objdump "The Import Tables:
  lookup 00009300 time 00000000 fwd 00000000 name 0000993e addr 00007208

    DLL Name: libssl-3-x64.dll
    Hint/Ord  Name
         529  SSL_set_quic_tls_cbs
  lookup 000091c8 time 00000000 fwd 00000000 name 00009c88 addr 000070d0

    DLL Name: libcrypto-3-x64.dll
    Hint/Ord  Name
        4077  RAND_bytes

Export Table:
 DLL name: ngtcp2_crypto_ossl.dll
 Ordinal base: 1")
expect("llvm-objdump, OpenSSL 3" "${_llvm_objdump}" "3" "")
expect_crypto("llvm-objdump, OpenSSL 3" "${_llvm_objdump}" "3" "")
expect("PE, OpenSSL 1.1" "    libssl-1_1-x64.dll" "1" "")
expect_crypto("PE, OpenSSL 1.1" "    libcrypto-1_1-x64.dll" "1" "")
expect("PE, arm64" "    libssl-3-arm64.dll" "3" "")
expect("PE, x86" "\tDLL Name: libssl-3.dll" "3" "")
expect("PE, upper case" "    LIBSSL-4-X64.DLL" "4" "")
# What must read as nothing: an unversioned DLL, look-alikes, a DLL's own name in LLVM's export table (it imports nothing
# of that name), a heading that names a directory like a libssl, a libcrypto alone.
expect("PE, unversioned" "    libssl.dll" "" "")
expect("PE, look-alike" "    libsslkeylog-2.dll" "" "")
expect("PE, look-alike before" "\tDLL Name: notlibssl-3-x64.dll" "" "")
expect("PE, its own name exported" "Export Table:
 DLL name: libssl-3-x64.dll" "" "")
expect("PE, a decoy heading" "Dump of file C:\\libssl-9-x64.dll\\ngtcp2_crypto_ossl.dll\r
\r
    libssl-3-x64.dll\r
" "3" "")
expect("PE, libcrypto only" "    libcrypto-3-x64.dll" "" "")
expect_crypto("PE, look-alike" "    libcryptopp-8.dll" "" "")
expect_crypto("PE, libssl only" "    libssl-3-x64.dll" "" "")

if(_failures)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_cases} listings, wrong readings:${_failures}")
endif()

# The verdict qbDependencies.cmake acts on, for the readings of both sides: MAJORS disables QUIC, COPIES is reported,
# "" is one library or an unknown side. decide_both() takes each side's libssl and libcrypto (major, path); decide() a
# libssl per side and, for its libcrypto, the same major with no path -- the readings of every coherent install.
set(_decisions 0)
function(decide_both label ngtcp2_ssl ngtcp2_ssl_path ngtcp2_crypto ngtcp2_crypto_path
                     qb_ssl qb_ssl_path qb_crypto qb_crypto_path qb_static expected)
    _ngtcp2_openssl_conflict("${ngtcp2_ssl}" "${ngtcp2_ssl_path}" "${ngtcp2_crypto}" "${ngtcp2_crypto_path}"
                             "${qb_ssl}" "${qb_ssl_path}" "${qb_crypto}" "${qb_crypto_path}" "${qb_static}" _verdict)
    math(EXPR _n "${_decisions} + 1")
    set(_decisions ${_n} PARENT_SCOPE)
    if(NOT "${_verdict}" STREQUAL "${expected}")
        set(_failures "${_failures}\n  ${label}: verdict '${_verdict}', expected '${expected}'" PARENT_SCOPE)
    endif()
endfunction()
macro(decide label ngtcp2_major ngtcp2_path qb_major qb_path qb_static expected)
    decide_both("${label}" "${ngtcp2_major}" "${ngtcp2_path}" "${ngtcp2_major}" "" "${qb_major}" "${qb_path}"
                "${qb_major}" "" "${qb_static}" "${expected}")
endmacro()
decide("one library, one path" "4" "/opt/x/lib/libssl.4.dylib" "4" "/opt/x/lib/libssl.4.dylib" FALSE "")
decide("two majors, two paths" "4" "/opt/homebrew/opt/openssl@4/lib/libssl.4.dylib"
       "3" "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib" FALSE MAJORS)
decide("two majors, sonames only" "4" "" "3" "" FALSE MAJORS)
decide("one major, sonames only" "3" "" "3" "" FALSE "")
decide("the same major from two prefixes" "3" "/opt/homebrew/lib/libssl.3.dylib"
       "3" "/opt/local/lib/libssl.3.dylib" FALSE COPIES)
decide("libngtcp2's OpenSSL unread" "" "" "3" "" FALSE "")
decide("qb's OpenSSL unread" "4" "" "" "" FALSE "")
# An @rpath install name resolves only at run time: the majors decide.
decide("an @rpath name, one major" "3" "@rpath/libssl.3.dylib" "3" "/opt/x/lib/libssl.3.dylib" FALSE "")
decide("an @rpath name, two majors" "4" "@rpath/libssl.4.dylib" "3" "/opt/x/lib/libssl.3.dylib" FALSE MAJORS)
decide("an @rpath name on qb's side" "3" "/opt/x/lib/libssl.3.dylib" "3" "@rpath/libssl.3.dylib" FALSE "")
# A static libssl in qb is a copy of its own: beside a shared one of the same major, two copies; of another, two majors.
decide("qb static, the helper on a shared libssl" "3" "" "3" "" TRUE COPIES)
decide("qb static, the helper on another major" "4" "" "3" "" TRUE MAJORS)
decide("qb static, the helper static too" "" "" "3" "" TRUE "")
# libcrypto (Huly QB-1185): every major read on either side is one, or QUIC is refused -- a cache that kept qb's
# libcrypto on another major than the libssl it re-found is two majors whatever libngtcp2 links.
decide_both("a half-cleared cache: qb's libcrypto on the other major"
            "4" "/opt/o4/lib/libssl.4.dylib" "4" "/opt/o4/lib/libcrypto.4.dylib"
            "4" "/opt/o4/lib/libssl.4.dylib" "3" "/opt/o3/lib/libcrypto.3.dylib" FALSE MAJORS)
decide_both("qb's libssl and libcrypto on two majors, libngtcp2 unread" "" "" "" "" "4" "" "3" "" FALSE MAJORS)
decide_both("libngtcp2's libcrypto on another major" "3" "" "4" "" "3" "" "3" "" FALSE MAJORS)
decide_both("libngtcp2's libcrypto unread, one major" "3" "" "" "" "3" "" "3" "" FALSE "")
decide_both("qb static, its libcrypto on another major" "4" "" "4" "" "4" "" "3" "" TRUE MAJORS)
decide_both("one major, the libcrypto from two prefixes"
            "3" "/opt/a/lib/libssl.3.dylib" "3" "/opt/a/lib/libcrypto.3.dylib"
            "3" "/opt/a/lib/libssl.3.dylib" "3" "/opt/b/lib/libcrypto.3.dylib" FALSE COPIES)
decide_both("one major, one library each"
            "3" "/opt/a/lib/libssl.3.dylib" "3" "/opt/a/lib/libcrypto.3.dylib"
            "3" "/opt/a/lib/libssl.3.dylib" "3" "/opt/a/lib/libcrypto.3.dylib" FALSE "")
decide_both("an @rpath libcrypto, one major"
            "3" "/opt/a/lib/libssl.3.dylib" "3" "@rpath/libcrypto.3.dylib"
            "3" "/opt/a/lib/libssl.3.dylib" "3" "/opt/b/lib/libcrypto.3.dylib" FALSE "")
# One file under two spellings is one library: the paths are compared resolved (not on Windows, never read).
if(WORK_DIR AND NOT WIN32)
    file(REMOVE_RECURSE "${WORK_DIR}")
    file(MAKE_DIRECTORY "${WORK_DIR}/keg/lib" "${WORK_DIR}/opt")
    file(WRITE "${WORK_DIR}/keg/lib/libssl.3.dylib" "")
    file(CREATE_LINK "${WORK_DIR}/keg" "${WORK_DIR}/opt/openssl" SYMBOLIC)
    decide("one file, two spellings" "3" "${WORK_DIR}/opt/openssl/lib/libssl.3.dylib"
           "3" "${WORK_DIR}/keg/lib/libssl.3.dylib" FALSE "")
endif()
if(_failures)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_decisions} decisions, wrong ones:${_failures}")
endif()

# The ELF reader's tools, through stand-ins that print a listing and exit as told: a failing tool's output is no
# answer, objdump takes over from a readelf that fails, no tool and a static archive read nothing.
set(_tools 0)
function(read_through label library readelf objdump major)
    set(APPLE FALSE) # an ELF host's reading, whatever runs the test
    set(CMAKE_READELF "${readelf}")
    set(CMAKE_OBJDUMP "${objdump}")
    _ngtcp2_linked_openssl("${library}" _major _path _crypto_major _crypto_path)
    math(EXPR _n "${_tools} + 1")
    set(_tools ${_n} PARENT_SCOPE)
    if(NOT "${_major}" STREQUAL "${major}" OR NOT "${_crypto_major}" STREQUAL "${major}")
        set(_failures "${_failures}\n  ${label}: read libssl '${_major}' libcrypto '${_crypto_major}', expected '${major}'"
            PARENT_SCOPE)
    endif()
endfunction()
if(WORK_DIR AND NOT WIN32 AND EXISTS /bin/sh)
    file(MAKE_DIRECTORY "${WORK_DIR}/tools")
    function(stand_in name soname rc) # prints the NEEDED entries of a libssl and a libcrypto, then exits <rc>
        file(WRITE "${WORK_DIR}/tools/${name}"
             "#!/bin/sh\necho ' 0x0000000000000001 (NEEDED) Shared library: [libssl.so.${soname}]'\n"
             "echo ' 0x0000000000000001 (NEEDED) Shared library: [libcrypto.so.${soname}]'\nexit ${rc}\n")
        file(CHMOD "${WORK_DIR}/tools/${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endfunction()
    stand_in(ok 3 0)
    stand_in(fails 7 1)
    file(WRITE "${WORK_DIR}/tools/libngtcp2_crypto_ossl.so.0" "")
    file(WRITE "${WORK_DIR}/tools/libngtcp2_crypto_ossl.a" "")
    set(_lib "${WORK_DIR}/tools/libngtcp2_crypto_ossl.so.0")
    read_through("readelf" "${_lib}" "${WORK_DIR}/tools/ok" "" "3")
    read_through("a readelf that fails, no objdump" "${_lib}" "${WORK_DIR}/tools/fails" "" "")
    read_through("a readelf that fails, then objdump" "${_lib}" "${WORK_DIR}/tools/fails" "${WORK_DIR}/tools/ok" "3")
    read_through("objdump alone" "${_lib}" "" "${WORK_DIR}/tools/ok" "3")
    read_through("no tool" "${_lib}" "" "" "")
    read_through("a static archive" "${WORK_DIR}/tools/libngtcp2_crypto_ossl.a" "${WORK_DIR}/tools/ok" "" "")
endif()
# Mach-O: the file read is the one dyld loads, the install name a binary records (`otool -D`), through a stand-in otool
# that prints <file>.id for -D and <file>.listing for -L. Homebrew's search finds a Cellar path whose install name is
# the opt/ link: after an upgrade that kept the old keg, the link names the new library, and that is the one that runs.
function(read_loaded label library major)
    set(APPLE TRUE) # a Mach-O host's reading, whatever runs the test
    set(NGTCP2_OTOOL_EXECUTABLE "${WORK_DIR}/otool/otool")
    _ngtcp2_linked_openssl("${library}" _major _path _crypto_major _crypto_path)
    math(EXPR _n "${_tools} + 1")
    set(_tools ${_n} PARENT_SCOPE)
    if(NOT "${_major}" STREQUAL "${major}" OR NOT "${_crypto_major}" STREQUAL "${major}")
        set(_failures "${_failures}\n  ${label}: read libssl '${_major}' libcrypto '${_crypto_major}', expected '${major}'"
            PARENT_SCOPE)
    endif()
endfunction()
if(WORK_DIR AND NOT WIN32 AND EXISTS /bin/sh)
    file(MAKE_DIRECTORY "${WORK_DIR}/otool" "${WORK_DIR}/otool/Cellar" "${WORK_DIR}/otool/opt")
    file(WRITE "${WORK_DIR}/otool/otool" "#!/bin/sh\necho \"$2:\"\ncase \"$1\" in\n"
               "-D) [ -f \"$2.fail\" ] && exit 1; [ -f \"$2.id\" ] && cat \"$2.id\"; exit 0 ;;\n"
               "-L) cat \"$2.listing\"; exit 0 ;;\nesac\nexit 2\n")
    file(CHMOD "${WORK_DIR}/otool/otool" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    function(planted_dylib path major id) # a library whose listing names OpenSSL <major>, and its install name
        file(WRITE "${path}" "")
        file(WRITE "${path}.listing" "\t${id} (compatibility version 1.0.0)\n"
                   "\t/opt/o${major}/lib/libssl.${major}.dylib (compatibility version ${major}.0.0)\n"
                   "\t/opt/o${major}/lib/libcrypto.${major}.dylib (compatibility version ${major}.0.0)\n")
        if(NOT "${id}" STREQUAL "")
            file(WRITE "${path}.id" "${id}\n")
        endif()
    endfunction()
    set(_opt "${WORK_DIR}/otool/opt/libngtcp2_crypto_ossl.0.dylib")
    planted_dylib("${_opt}" 4 "${_opt}")
    planted_dylib("${WORK_DIR}/otool/Cellar/kept.dylib" 3 "${_opt}")
    planted_dylib("${WORK_DIR}/otool/Cellar/rpath.dylib" 3 "@rpath/libngtcp2_crypto_ossl.0.dylib")
    planted_dylib("${WORK_DIR}/otool/Cellar/gone.dylib" 3 "${WORK_DIR}/otool/opt/gone.0.dylib")
    planted_dylib("${WORK_DIR}/otool/Cellar/unnamed.dylib" 3 "")
    planted_dylib("${WORK_DIR}/otool/Cellar/failing.dylib" 3 "${_opt}")
    file(WRITE "${WORK_DIR}/otool/Cellar/failing.dylib.fail" "")
    read_loaded("the old keg kept, its install name on the new one" "${WORK_DIR}/otool/Cellar/kept.dylib" 4)
    read_loaded("the library at its install name" "${_opt}" 4)
    read_loaded("an @rpath install name" "${WORK_DIR}/otool/Cellar/rpath.dylib" 3)
    read_loaded("an install name that is gone" "${WORK_DIR}/otool/Cellar/gone.dylib" 3)
    read_loaded("no install name" "${WORK_DIR}/otool/Cellar/unnamed.dylib" 3)
    read_loaded("otool -D failing" "${WORK_DIR}/otool/Cellar/failing.dylib" 3)
endif()
# Windows (Huly QB-1184), as a Windows configure reads, whatever runs the test: the DLL an import library names -- read
# from the import library, in the bin directory beside its lib directory first, else beside it -- through `objdump -p`
# (CMAKE_OBJDUMP), then `dumpbin /DEPENDENTS`, then `llvm-objdump -p`, each when the one before is absent or fails.
# Stand-ins that print, for their own flags only, the planted DLL's lines (DLL names) as their tool lists imports; a
# planted import library is text holding the DLL name it records, which is what the module reads of a real one.
function(read_pe label library objdump dumpbin llvm major)
    set(WIN32 TRUE)
    set(APPLE FALSE)
    set(CMAKE_OBJDUMP "${objdump}")
    foreach(_tool IN ITEMS "NGTCP2_DUMPBIN_EXECUTABLE|${dumpbin}" "NGTCP2_LLVM_OBJDUMP_EXECUTABLE|${llvm}")
        string(REGEX REPLACE "[|].*$" "" _var "${_tool}")
        string(REGEX REPLACE "^[^|]*[|]" "" _path "${_tool}")
        if("${_path}" STREQUAL "")
            unset(${_var}) # the module searches it, as a configure does
        else()
            set(${_var} "${_path}")
        endif()
    endforeach()
    set(CMAKE_LINKER "${ARGN}")
    _ngtcp2_linked_openssl("${library}" _major _path _crypto_major _crypto_path)
    math(EXPR _n "${_tools} + 1")
    set(_tools ${_n} PARENT_SCOPE)
    if(NOT "${_major}" STREQUAL "${major}" OR NOT "${_crypto_major}" STREQUAL "${major}" OR NOT "${_path}" STREQUAL "")
        set(_failures "${_failures}\n  ${label}: read libssl '${_major}' (${_path}) libcrypto '${_crypto_major}', expected "
                      "'${major}'" PARENT_SCOPE)
    endif()
endfunction()
if(WORK_DIR AND NOT WIN32 AND EXISTS /bin/sh)
    set(_pe "${WORK_DIR}/pe")
    file(MAKE_DIRECTORY "${_pe}/tools" "${_pe}/msvc")
    set(_each "while IFS= read -r n; do printf")
    file(WRITE "${_pe}/tools/objdump" "#!/bin/sh\n[ \"$1\" = -p ] || exit 2\n${_each} '\\tDLL Name: %s\\n' \"$n\"; done < \"$2\"\n")
    file(WRITE "${_pe}/tools/llvm-objdump" "#!/bin/sh\n[ \"$1\" = -p ] || exit 2\necho 'The Import Tables:'\n"
               "${_each} '    DLL Name: %s\\n' \"$n\"; done < \"$2\"\n")
    file(WRITE "${_pe}/msvc/dumpbin" "#!/bin/sh\n[ \"$1\" = /NOLOGO ] && [ \"$2\" = /DEPENDENTS ] || exit 2\n"
               "printf 'Dump of file %s\\r\\n\\r\\n  Image has the following dependencies:\\r\\n\\r\\n' \"$3\"\n"
               "${_each} '    %s\\r\\n' \"$n\"; done < \"$3\"\n")
    file(WRITE "${_pe}/tools/fails" "#!/bin/sh\necho '    libssl-7-x64.dll'\necho '    libcrypto-7-x64.dll'\nexit 1\n")
    foreach(_tool "tools/objdump" "tools/llvm-objdump" "msvc/dumpbin" "tools/fails")
        file(CHMOD "${_pe}/${_tool}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endforeach()
    set(_objdump "${_pe}/tools/objdump")
    set(_dumpbin "${_pe}/msvc/dumpbin")
    set(_llvm "${_pe}/tools/llvm-objdump")
    set(_fails "${_pe}/tools/fails")
    set(_absent "${_pe}/tools/absent")
    function(plant_pe dll major) # a DLL whose imports name OpenSSL <major>'s libssl and libcrypto
        get_filename_component(_dir "${dll}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dir}")
        file(WRITE "${dll}" "ngtcp2.dll\nlibssl-${major}-x64.dll\nlibcrypto-${major}-x64.dll\nKERNEL32.dll\n")
    endfunction()
    function(plant_implib file dll) # an import library recording <dll>, among an archive's other strings
        get_filename_component(_dir "${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dir}")
        file(WRITE "${file}" "!<arch>\n/               -1                      0       490       `\n__imp_qb\n${dll}\n")
    endfunction()
    # vcpkg's layout: lib/<name>.lib names bin/<name>.dll.
    plant_implib("${_pe}/vcpkg/lib/ngtcp2_crypto_ossl.lib" ngtcp2_crypto_ossl.dll)
    plant_pe("${_pe}/vcpkg/bin/ngtcp2_crypto_ossl.dll" 3)
    set(_lib "${_pe}/vcpkg/lib/ngtcp2_crypto_ossl.lib")
    read_pe("PE, objdump" "${_lib}" "${_objdump}" "${_fails}" "${_fails}" 3)
    read_pe("PE, objdump fails, then dumpbin" "${_lib}" "${_fails}" "${_dumpbin}" "${_fails}" 3)
    read_pe("PE, objdump and dumpbin fail, then llvm-objdump" "${_lib}" "${_fails}" "${_fails}" "${_llvm}" 3)
    read_pe("PE, no objdump, dumpbin" "${_lib}" "" "${_dumpbin}" "${_fails}" 3)
    read_pe("PE, no objdump, no dumpbin, llvm-objdump" "${_lib}" "" "${_absent}" "${_llvm}" 3)
    read_pe("PE, every tool fails" "${_lib}" "${_fails}" "${_fails}" "${_fails}" "")
    read_pe("PE, no tool" "${_lib}" "" "${_absent}" "${_absent}" "")
    read_pe("PE, the DLL given" "${_pe}/vcpkg/bin/ngtcp2_crypto_ossl.dll" "${_objdump}" "${_fails}" "${_fails}" 3)
    # dumpbin beside the linker (MSVC's link.exe), found by the module itself.
    read_pe("PE, dumpbin beside the linker" "${_lib}" "" "" "${_fails}" 3 "${_pe}/msvc/link.exe")
    # MinGW's layout: lib/lib<name>.dll.a names bin/lib<name>-0.dll; a DLL named like the import library is not the one
    # it records, and is never read.
    plant_implib("${_pe}/msys2/lib/libngtcp2_crypto_ossl.dll.a" libngtcp2_crypto_ossl-0.dll)
    plant_pe("${_pe}/msys2/bin/libngtcp2_crypto_ossl-0.dll" 4)
    plant_pe("${_pe}/msys2/bin/libngtcp2_crypto_ossl.dll" 9)
    read_pe("PE, MinGW's suffixed DLL" "${_pe}/msys2/lib/libngtcp2_crypto_ossl.dll.a" "${_objdump}" "" "" 4)
    # bin/ first, then beside the import library; an upper-case extension is one too.
    plant_implib("${_pe}/both/lib/x.lib" x.dll)
    plant_pe("${_pe}/both/bin/x.dll" 3)
    plant_pe("${_pe}/both/lib/x.dll" 9)
    read_pe("PE, bin/ before beside" "${_pe}/both/lib/x.lib" "${_objdump}" "" "" 3)
    plant_implib("${_pe}/beside/X.LIB" X.DLL)
    plant_pe("${_pe}/beside/X.DLL" 4)
    read_pe("PE, beside the import library" "${_pe}/beside/X.LIB" "${_objdump}" "" "" 4)
    # No answer: a static library, which records no DLL (one named like it beside is not read), and an import library
    # whose DLL is in neither place.
    file(WRITE "${_pe}/static/lib/ngtcp2_crypto_ossl.lib" "!<arch>\n/               \nngtcp2_crypto_ossl.obj\n")
    plant_pe("${_pe}/static/bin/ngtcp2_crypto_ossl.dll" 9)
    read_pe("PE, a static library" "${_pe}/static/lib/ngtcp2_crypto_ossl.lib" "${_objdump}" "" "" "")
    plant_implib("${_pe}/nodll/lib/ngtcp2_crypto_ossl.lib" ngtcp2_crypto_ossl.dll)
    read_pe("PE, its DLL nowhere" "${_pe}/nodll/lib/ngtcp2_crypto_ossl.lib" "${_objdump}" "" "" "")
endif()
if(_failures)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_tools} tool readings, wrong ones:${_failures}")
endif()

# The real thing, on Windows (Huly QB-1184): the DLLs this build planted with its own linker (tests/io/unit/
# CMakeLists.txt) -- a libngtcp2_crypto_ossl stand-in importing a libssl and a libcrypto of OpenSSL 4, and one of
# OpenSSL 3, each DLL named apart from its import library -- read from their import libraries as the configure reads
# the real one, with the tools it finds, and their verdicts beside an OpenSSL 3 of qb's: two majors, then none. A
# Windows build always plants them, and an MSVC linker always has dumpbin beside it: neither missing passes.
set(_pe_tool "")
if(WIN32)
    if(CMAKE_OBJDUMP)
        set(_pe_tool "${CMAKE_OBJDUMP}")
    else()
        set(_pe_linker_dir "")
        if(IS_ABSOLUTE "${CMAKE_LINKER}")
            get_filename_component(_pe_linker_dir "${CMAKE_LINKER}" DIRECTORY)
        endif()
        find_program(_pe_dumpbin dumpbin HINTS "${_pe_linker_dir}")
        find_program(_pe_llvm_objdump llvm-objdump HINTS "${_pe_linker_dir}")
        if(_pe_dumpbin)
            set(_pe_tool "${_pe_dumpbin}")
        elseif(_pe_llvm_objdump)
            set(_pe_tool "${_pe_llvm_objdump}")
        elseif("${CMAKE_LINKER}" MATCHES "(^|[/\\\\])link\\.exe$")
            message(FATAL_ERROR "ngtcp2-openssl-listing: no dumpbin beside MSVC's linker ${CMAKE_LINKER}")
        endif()
    endif()
endif()
set(_planted 0)
if(WIN32 AND "${PLANTED_PE}" STREQUAL "")
    message(FATAL_ERROR "ngtcp2-openssl-listing: a Windows build plants its DLLs, none passed (PLANTED_PE)")
endif()
if(WIN32)
    string(REPLACE "|" ";" _planted_pe "${PLANTED_PE}")
    foreach(_planted_entry IN LISTS _planted_pe)
        string(REGEX MATCH "^([0-9]+)=(.+)$" _planted_match "${_planted_entry}")
        set(_planted_major "${CMAKE_MATCH_1}")
        set(_planted_lib "${CMAKE_MATCH_2}")
        if(NOT EXISTS "${_planted_lib}")
            message(FATAL_ERROR "ngtcp2-openssl-listing: the planted ${_planted_lib} is not built")
        endif()
        _ngtcp2_loaded_file("${_planted_lib}" _planted_dll)
        _ngtcp2_linked_openssl("${_planted_lib}" _major _path _crypto_major _crypto_path)
        math(EXPR _planted "${_planted} + 1")
        if(NOT "${_planted_dll}" MATCHES "/bin/ngtcp2_crypto_ossl-0\\.dll$")
            string(APPEND _failures "\n  planted OpenSSL ${_planted_major}: ${_planted_lib} resolved to '${_planted_dll}', "
                                    "not its DLL in bin/")
        elseif(_pe_tool AND (NOT "${_major}" STREQUAL "${_planted_major}"
                             OR NOT "${_crypto_major}" STREQUAL "${_planted_major}"))
            string(APPEND _failures "\n  planted OpenSSL ${_planted_major}: ${_planted_dll} read libssl '${_major}' "
                                    "libcrypto '${_crypto_major}' through ${_pe_tool}")
        endif()
        if(_pe_tool)
            set(_planted_expected "")
            if(NOT _planted_major STREQUAL "3")
                set(_planted_expected MAJORS)
            endif()
            _ngtcp2_openssl_conflict("${_major}" "${_path}" "${_crypto_major}" "${_crypto_path}" 3 "" 3 "" FALSE
                                     _planted_verdict)
            if(NOT "${_planted_verdict}" STREQUAL "${_planted_expected}")
                string(APPEND _failures "\n  planted OpenSSL ${_planted_major} beside qb's OpenSSL 3: verdict "
                                        "'${_planted_verdict}', expected '${_planted_expected}'")
            endif()
        endif()
    endforeach()
    if(_pe_tool AND NOT CMAKE_OBJDUMP)
        # The OpenSSL 4 one again: through each tool when the one before cannot run, CMAKE_OBJDUMP first where the
        # toolchain names one (llvm-objdump stands in for MinGW's here, where it is found); and copied into the other
        # layouts -- MinGW's .dll.a, an upper-case .LIB with its DLL beside it, a DLL in bin/ and another beside the
        # import library, where bin/'s is the one read.
        foreach(_planted_entry IN LISTS _planted_pe)
            if(_planted_entry MATCHES "^([0-9]+)=(.+)$")
                get_filename_component(_planted_dir "${CMAKE_MATCH_2}" DIRECTORY)
                set(_planted_lib_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
                set(_planted_dll_${CMAKE_MATCH_1} "${_planted_dir}/../bin/ngtcp2_crypto_ossl-0.dll")
            endif()
        endforeach()
        function(read_planted label library major objdump dumpbin llvm)
            set(CMAKE_OBJDUMP "${objdump}")
            set(NGTCP2_DUMPBIN_EXECUTABLE "${dumpbin}")
            set(NGTCP2_LLVM_OBJDUMP_EXECUTABLE "${llvm}")
            _ngtcp2_linked_openssl("${library}" _major _path _crypto_major _crypto_path)
            math(EXPR _n "${_planted} + 1")
            set(_planted ${_n} PARENT_SCOPE)
            if(NOT "${_major}" STREQUAL "${major}" OR NOT "${_crypto_major}" STREQUAL "${major}")
                set(_failures "${_failures}
  planted OpenSSL ${major}, ${label}: read libssl '${_major}' "
                              "libcrypto '${_crypto_major}'" PARENT_SCOPE)
            endif()
        endfunction()
        set(_absent "${WORK_DIR}/no-such-tool.exe")
        if(_pe_dumpbin)
            read_planted("an objdump that cannot run, then dumpbin" "${_planted_lib_4}" 4 "${_absent}" "${_pe_dumpbin}"
                         "${_absent}")
        endif()
        if(_pe_llvm_objdump)
            read_planted("objdump first" "${_planted_lib_4}" 4 "${_pe_llvm_objdump}" "${_absent}" "${_absent}")
            read_planted("no dumpbin, llvm-objdump" "${_planted_lib_4}" 4 "" "${_absent}" "${_pe_llvm_objdump}")
        endif()
        set(_layouts "${WORK_DIR}/pe-layouts")
        file(REMOVE_RECURSE "${_layouts}")
        foreach(_copy "${_planted_lib_4}|mingw/lib/libngtcp2_crypto_ossl.dll.a"
                      "${_planted_dll_4}|mingw/bin/ngtcp2_crypto_ossl-0.dll"
                      "${_planted_lib_4}|upper/NGTCP2_CRYPTO_OSSL.LIB" "${_planted_dll_4}|upper/ngtcp2_crypto_ossl-0.dll"
                      "${_planted_lib_4}|both/lib/ngtcp2_crypto_ossl.lib" "${_planted_dll_4}|both/bin/ngtcp2_crypto_ossl-0.dll"
                      "${_planted_dll_3}|both/lib/ngtcp2_crypto_ossl-0.dll")
            string(REGEX REPLACE "[|].*$" "" _from "${_copy}")
            string(REGEX REPLACE "^.*[|]" "${_layouts}/" _to "${_copy}")
            get_filename_component(_to_dir "${_to}" DIRECTORY)
            file(MAKE_DIRECTORY "${_to_dir}")
            file(COPY_FILE "${_from}" "${_to}")
        endforeach()
        set(_found_tools "${_pe_dumpbin}" "${_pe_llvm_objdump}") # each, or "": not run
        read_planted("MinGW's .dll.a" "${_layouts}/mingw/lib/libngtcp2_crypto_ossl.dll.a" 4 "" ${_found_tools})
        read_planted("an upper-case .LIB, its DLL beside it" "${_layouts}/upper/NGTCP2_CRYPTO_OSSL.LIB" 4 ""
                     ${_found_tools})
        read_planted("bin/ before beside" "${_layouts}/both/lib/ngtcp2_crypto_ossl.lib" 4 "" ${_found_tools})
    endif()
    if(NOT _pe_tool)
        message(STATUS "ngtcp2-openssl-listing: no objdump, dumpbin or llvm-objdump here: the planted DLLs are not read")
    endif()
endif()
if(_failures)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_planted} planted DLL readings, wrong ones:${_failures}")
endif()

# qb's own libssl and libcrypto, read wherever the comparison could run: a file that is a versioned libssl or libcrypto
# must read, whether or not this configure found libngtcp2, so an ELF reader that reads nothing fails on every lane with
# a readelf.
set(_tool_exists FALSE)
if(WIN32)
    if(_pe_tool)
        set(_tool_exists TRUE)
    endif()
elseif(APPLE)
    find_program(_otool otool)
    if(_otool)
        set(_tool_exists TRUE)
    endif()
elseif(CMAKE_READELF OR CMAKE_OBJDUMP)
    set(_tool_exists TRUE)
endif()
set(_qb_read "")
function(read_own name library)
    if(WIN32 OR NOT library OR NOT EXISTS "${library}" OR "${library}" MATCHES "\\.a$")
        return()
    endif()
    file(REAL_PATH "${library}" _real)
    _ngtcp2_linked_openssl("${library}" _ssl_major _ssl_path _crypto_major _crypto_path)
    set(_major "${_${name}_major}")
    set(_path "${_${name}_path}")
    set(_qb_read "${_qb_read} lib${name} '${_major}' (${_real})" PARENT_SCOPE)
    if(_tool_exists AND "${_real}" MATCHES "/lib${name}(\\.so\\.[0-9]|\\.[0-9]+(\\.[0-9]+)*\\.dylib$)"
       AND ("${_major}" STREQUAL "" OR (APPLE AND "${_path}" STREQUAL "")))
        message(FATAL_ERROR "ngtcp2-openssl-listing: the module could not read qb's own lib${name} ${library} "
                            "(${_real}): the configure-time check is vacuous on this host")
    endif()
endfunction()
read_own(ssl "${QB_SSL_LIBRARY}")
read_own(crypto "${QB_CRYPTO_LIBRARY}")
if("${_qb_read}" STREQUAL "")
    set(_qb_read " nothing")
endif()

# qb's OpenSSL check (Huly QB-1190), whenever the configure found OpenSSL: it ran; it compared the majors on Linux and
# macOS unless the headers are LibreSSL's; it took the headers' major from the number they name their libraries with;
# it read each library as the module reads it now -- a check that reads nothing passes on every host whose OpenSSL is
# one, and only its readings show it -- and it made each file it read an input.
if(QB_HAS_SSL AND NOT WIN32 AND NOT DEFINED QB_OPENSSL_COMPARED)
    message(FATAL_ERROR "ngtcp2-openssl-listing: the configure found OpenSSL, yet qb's OpenSSL check never ran")
endif()
if(DEFINED QB_OPENSSL_COMPARED)
    set(_libressl FALSE)
    set(_headers_major "")
    if(EXISTS "${QB_OPENSSL_INCLUDE_DIR}/openssl/opensslv.h")
        file(READ "${QB_OPENSSL_INCLUDE_DIR}/openssl/opensslv.h" _opensslv)
        if("${_opensslv}" MATCHES "(^|\n)[ \t]*#[ \t]*define[ \t]+LIBRESSL_VERSION_NUMBER[ \t]")
            set(_libressl TRUE)
        endif()
        if("${_opensslv}" MATCHES "(^|\n)[ \t]*#[ \t]*define[ \t]+OPENSSL_SHLIB_VERSION[ \t]+\"?([0-9]+)")
            set(_headers_major "${CMAKE_MATCH_2}")
        elseif("${_opensslv}" MATCHES "(^|\n)[ \t]*#[ \t]*define[ \t]+SHLIB_VERSION_NUMBER[ \t]+\"([0-9]+)")
            set(_headers_major "${CMAKE_MATCH_2}")
        endif()
    endif()
    if("${_headers_major}" STREQUAL "" AND "${QB_OPENSSL_VERSION}" MATCHES "^([0-9]+)")
        set(_headers_major "${CMAKE_MATCH_1}")
    endif()
    if(NOT "${QB_OPENSSL_HEADERS_MAJOR}" STREQUAL "${_headers_major}")
        message(FATAL_ERROR "ngtcp2-openssl-listing: qb's OpenSSL check took its headers as major "
                            "'${QB_OPENSSL_HEADERS_MAJOR}', their opensslv.h and version say '${_headers_major}'")
    endif()
    set(_should_compare FALSE)
    if(NOT WIN32 AND (QB_TARGET_SYSTEM STREQUAL "Linux" OR QB_TARGET_SYSTEM STREQUAL "Darwin") AND NOT _libressl)
        set(_should_compare TRUE)
    endif()
    if((QB_OPENSSL_COMPARED AND NOT _should_compare) OR (_should_compare AND NOT QB_OPENSSL_COMPARED))
        message(FATAL_ERROR "ngtcp2-openssl-listing: qb's OpenSSL check compared '${QB_OPENSSL_COMPARED}' on "
                            "'${QB_TARGET_SYSTEM}' (LibreSSL headers: '${_libressl}'), expected '${_should_compare}'")
    endif()
    if(QB_OPENSSL_COMPARED)
        _ngtcp2_linked_openssl("${QB_SSL_LIBRARY}" _check_ssl _unused _unused _unused)
        _ngtcp2_linked_openssl("${QB_CRYPTO_LIBRARY}" _unused _unused _check_crypto _unused)
        if(NOT "${QB_OPENSSL_SSL_READ}" STREQUAL "${_check_ssl}"
           OR NOT "${QB_OPENSSL_CRYPTO_READ}" STREQUAL "${_check_crypto}")
            message(FATAL_ERROR "ngtcp2-openssl-listing: qb's OpenSSL check read libssl '${QB_OPENSSL_SSL_READ}' and "
                                "libcrypto '${QB_OPENSSL_CRYPTO_READ}', the module reads '${_check_ssl}' and "
                                "'${_check_crypto}' now: a library changed since the configure (reconfigure), or the "
                                "check's reading is broken")
        endif()
        string(REPLACE "|" ";" _check_inputs "${QB_OPENSSL_CHECK_INPUTS}")
        foreach(_library IN ITEMS "${QB_SSL_LIBRARY}" "${QB_CRYPTO_LIBRARY}")
            if(IS_ABSOLUTE "${_library}" AND EXISTS "${_library}")
                _ngtcp2_loaded_file("${_library}" _loaded)
                file(REAL_PATH "${_loaded}" _loaded_real)
                foreach(_file IN ITEMS "${_loaded}" "${_loaded_real}")
                    if(NOT "${_file}" IN_LIST _check_inputs)
                        message(FATAL_ERROR "ngtcp2-openssl-listing: qb's OpenSSL check read ${_file} but does not "
                                            "re-run when it changes: its inputs are ${QB_OPENSSL_CHECK_INPUTS}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endif()
    string(APPEND _qb_read "; qb's OpenSSL check compared ${QB_OPENSSL_COMPARED} (libssl '${QB_OPENSSL_SSL_READ}', "
                           "libcrypto '${QB_OPENSSL_CRYPTO_READ}', headers ${QB_OPENSSL_VERSION} of major "
                           "'${QB_OPENSSL_HEADERS_MAJOR}')")
endif()

# qb's own module found the helper: its block compared the two, or a mutation made it skip them.
if(QB_QUIC_FOUND_BY_QB AND NOT QB_QUIC_COMPARED)
    message(FATAL_ERROR "ngtcp2-openssl-listing: qb's FindNgtcp2 found libngtcp2, yet the configure compared no "
                        "OpenSSL")
endif()

# The libraries this configure found, through the module's own reader and decision.
if(DEFINED QB_QUIC_NGTCP2_MAJOR AND NOT (LIBRARY AND EXISTS "${LIBRARY}"))
    message(FATAL_ERROR "ngtcp2-openssl-listing: the configure read a libngtcp2 (${LIBRARY}) that is gone: reconfigure")
endif()
if(LIBRARY AND EXISTS "${LIBRARY}")
    _ngtcp2_linked_openssl("${LIBRARY}" _live_major _live_path _live_crypto_major _live_crypto_path)
    _ngtcp2_linked_openssl("${QB_SSL_LIBRARY}" _qb_major _qb_path _qb_ssl_crypto_major _qb_ssl_crypto_path)
    _ngtcp2_linked_openssl("${QB_CRYPTO_LIBRARY}" _qb_crypto_ssl_major _qb_crypto_ssl_path _qb_crypto_major
                           _qb_crypto_path)
    set(_qb_headers_major "${QB_OPENSSL_HEADERS_MAJOR}") # the check's, re-derived above where it compared
    if(NOT DEFINED QB_OPENSSL_HEADERS_MAJOR)
        string(REGEX MATCH "^[0-9]+" _qb_headers_major "${QB_OPENSSL_VERSION}")
    endif()
    if("${_qb_major}" STREQUAL "")
        set(_qb_major "${_qb_headers_major}")
    endif()
    if("${_qb_crypto_major}" STREQUAL "")
        set(_qb_crypto_major "${_qb_headers_major}")
    endif()
    set(_qb_static FALSE)
    set(_qb_shown "${_qb_path}")
    if(NOT WIN32 AND "${QB_SSL_LIBRARY}" MATCHES "\\.a$")
        set(_qb_static TRUE)
        set(_qb_shown "${QB_SSL_LIBRARY}, static")
    endif()
    set(_live_note "")
    if(WIN32 AND _tool_exists)
        # On Windows (Huly QB-1184), the same oracle on the DLL the import library names -- and, the vcpkg layout's
        # DLL being in bin/ beside the import library under its name, one the module must have found.
        _ngtcp2_loaded_file("${LIBRARY}" _live_dll)
        get_filename_component(_live_dir "${LIBRARY}" DIRECTORY)
        get_filename_component(_live_name "${LIBRARY}" NAME_WE)
        if(EXISTS "${_live_dir}/../bin/${_live_name}.dll" AND NOT "${_live_dll}" MATCHES "\\.[dD][lL][lL]$")
            message(FATAL_ERROR "ngtcp2-openssl-listing: ${_live_dir}/../bin/${_live_name}.dll is beside the import "
                                "library ${LIBRARY}, and the module found no DLL for it")
        endif()
        if("${_live_dll}" MATCHES "\\.[dD][lL][lL]$"
           AND ("${_live_major}" STREQUAL "" OR "${_live_crypto_major}" STREQUAL ""))
            set(_raw_flags -p)
            if("${_pe_tool}" MATCHES "[dD][uU][mM][pP][bB][iI][nN]")
                set(_raw_flags /NOLOGO /DEPENDENTS)
            endif()
            execute_process(COMMAND "${_pe_tool}" ${_raw_flags} "${_live_dll}" OUTPUT_VARIABLE _raw ERROR_QUIET)
            string(TOLOWER "${_raw}" _raw)
            foreach(_name IN ITEMS ssl crypto)
                if(_name STREQUAL "ssl")
                    set(_read "${_live_major}")
                else()
                    set(_read "${_live_crypto_major}")
                endif()
                if("${_read}" STREQUAL "" AND "${_raw}" MATCHES "lib${_name}-[0-9]")
                    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_pe_tool}'s own listing of ${_live_dll} names a "
                                        "lib${_name}, the module read none: its reader is broken on this host")
                endif()
            endforeach()
        endif()
        if("${_live_major}" STREQUAL "")
            set(_live_note " -- it names no shared libssl: an OpenSSL linked into it is not seen")
        endif()
    elseif(_tool_exists AND NOT LIBRARY MATCHES "\\.(a|lib)$"
           AND ("${_live_major}" STREQUAL "" OR "${_live_crypto_major}" STREQUAL ""))
        # An oracle of the test's own, not the module's reader: the tool's raw listing naming a libssl or a libcrypto the
        # module did not read is a reader that reads nothing -- with a static libssl in qb, nothing else would show it on
        # macOS.
        if(APPLE)
            execute_process(COMMAND "${_otool}" -L "${LIBRARY}" OUTPUT_VARIABLE _raw ERROR_QUIET)
        elseif(CMAKE_READELF)
            execute_process(COMMAND "${CMAKE_READELF}" -d "${LIBRARY}" OUTPUT_VARIABLE _raw ERROR_QUIET)
        else()
            execute_process(COMMAND "${CMAKE_OBJDUMP}" -p "${LIBRARY}" OUTPUT_VARIABLE _raw ERROR_QUIET)
        endif()
        foreach(_name IN ITEMS ssl crypto)
            if(_name STREQUAL "ssl")
                set(_read "${_live_major}")
            else()
                set(_read "${_live_crypto_major}")
            endif()
            if("${_read}" STREQUAL "" AND "${_raw}" MATCHES "lib${_name}\\.(so\\.)?[0-9]")
                message(FATAL_ERROR "ngtcp2-openssl-listing: the tool's own listing of ${LIBRARY} names a lib${_name}, "
                                    "the module read none: its reader is broken on this host")
            endif()
        endforeach()
        if("${_live_major}" STREQUAL "")
            set(_live_note " -- it names no shared libssl: an OpenSSL linked into it is not seen")
        endif()
    endif()
    # What the configure read must be what the module reads now: a configure whose reading is broken shows here even
    # on a host whose two libraries agree, and so does a library replaced since the configure (reconfigure then).
    if(DEFINED QB_QUIC_NGTCP2_MAJOR)
        foreach(_reading "NGTCP2_MAJOR;_live_major" "NGTCP2_PATH;_live_path" "NGTCP2_CRYPTO_MAJOR;_live_crypto_major"
                         "NGTCP2_CRYPTO_PATH;_live_crypto_path" "QB_MAJOR;_qb_major" "QB_PATH;_qb_path"
                         "QB_CRYPTO_MAJOR;_qb_crypto_major" "QB_CRYPTO_PATH;_qb_crypto_path")
            list(GET _reading 0 _configured)
            list(GET _reading 1 _now)
            if(NOT "${QB_QUIC_${_configured}}" STREQUAL "${${_now}}")
                message(FATAL_ERROR "ngtcp2-openssl-listing: the configure read ${_configured} "
                                    "'${QB_QUIC_${_configured}}', the module reads '${${_now}}' now: a library changed "
                                    "since the configure (reconfigure), or the configure's reading is broken")
            endif()
        endforeach()
    endif()
    # Every file a reading depends on is an input of the configure, so that `cmake --build` re-runs it when one changes:
    # each library, the file it resolves to, on Mach-O the install name dyld loads and the file that resolves to, and
    # qb's opensslv.h.
    if(DEFINED QB_QUIC_INPUTS)
        string(REPLACE "|" ";" _inputs "${QB_QUIC_INPUTS}")
        set(_header "${QB_OPENSSL_INCLUDE_DIR}/openssl/opensslv.h")
        foreach(_library IN ITEMS "${LIBRARY}" "${QB_SSL_LIBRARY}" "${QB_CRYPTO_LIBRARY}" "${_header}")
            if(IS_ABSOLUTE "${_library}" AND EXISTS "${_library}")
                set(_loaded "${_library}")
                if(NOT _library STREQUAL _header)
                    _ngtcp2_loaded_file("${_library}" _loaded)
                endif()
                file(REAL_PATH "${_library}" _library_real)
                file(REAL_PATH "${_loaded}" _loaded_real)
                foreach(_file IN ITEMS "${_library}" "${_library_real}" "${_loaded}" "${_loaded_real}")
                    if(NOT "${_file}" IN_LIST _inputs)
                        message(FATAL_ERROR "ngtcp2-openssl-listing: the configure read ${_file} but does not re-run "
                                            "when it changes: its inputs are ${QB_QUIC_INPUTS}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endif()
    _ngtcp2_openssl_conflict("${_live_major}" "${_live_path}" "${_live_crypto_major}" "${_live_crypto_path}"
                             "${_qb_major}" "${_qb_path}" "${_qb_crypto_major}" "${_qb_crypto_path}" "${_qb_static}"
                             _live_conflict)
    if(DEFINED QB_QUIC_VERDICT AND NOT "${QB_QUIC_VERDICT}" STREQUAL "${_live_conflict}")
        message(FATAL_ERROR "ngtcp2-openssl-listing: the configure acted on the verdict '${QB_QUIC_VERDICT}', the "
                            "module gives '${_live_conflict}' on the same readings (${LIBRARY}, ${QB_SSL_LIBRARY}, "
                            "${QB_CRYPTO_LIBRARY})")
    endif()
    if(DEFINED QB_QUIC_VERDICT AND NOT "${QB_QUIC_VERDICT}" STREQUAL "MAJORS" AND NOT QB_HAS_QUIC)
        message(FATAL_ERROR "ngtcp2-openssl-listing: QUIC is disabled though libngtcp2 was found and the verdict is "
                            "'${QB_QUIC_VERDICT}': only two majors refuse it")
    endif()
    string(CONCAT _readings "${LIBRARY} links libssl '${_live_major}' (${_live_path}) and libcrypto "
                            "'${_live_crypto_major}' (${_live_crypto_path}), qb links libssl '${_qb_major}' "
                            "(${_qb_shown}) and libcrypto '${_qb_crypto_major}' (${_qb_crypto_path})")
    if(QB_HAS_QUIC AND "${_live_conflict}" STREQUAL "MAJORS")
        message(FATAL_ERROR "ngtcp2-openssl-listing: QUIC is enabled, yet ${_readings}")
    endif()
    message(STATUS "ngtcp2-openssl-listing: ${_cases} listings, ${_decisions} decisions, ${_tools} tool readings and "
                   "${_planted} planted DLL readings right; ${_readings}, verdict '${_live_conflict}', QUIC "
                   "${QB_HAS_QUIC}${_live_note}; qb's own OpenSSL read:${_qb_read}")
else()
    message(STATUS "ngtcp2-openssl-listing: ${_cases} listings, ${_decisions} decisions, ${_tools} tool readings and "
                   "${_planted} planted DLL readings right; qb's own OpenSSL read:${_qb_read}; no libngtcp2 in this "
                   "configure")
endif()
