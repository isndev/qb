# qb-io-test-unit-ngtcp2-openssl-listing (Huly QB-1179, QB-1185). FindNgtcp2.cmake reads which OpenSSL
# libngtcp2_crypto_ossl links, so that qbDependencies.cmake disables QUIC when qb links another major: two OpenSSL majors
# in one process crash at the first QUIC handshake (two copies of one major are reported, not refused). This runs the
# module's parser on the listings each platform's tool prints, for libssl and for libcrypto, its verdict on the readings
# of both sides and its tool fallbacks and Mach-O install names (stand-in tools, off Windows), then the real libraries:
# qb's own libssl and libcrypto must read wherever their files are versioned and a tool exists, and -- when the
# configure compared them -- what the module reads now must be what the configure read, the verdict the one it acted on,
# QUIC enabled unless they are two majors, and every file read an input of the configure.
#
#   cmake -DFIND_MODULE=<FindNgtcp2.cmake> [-DLIBRARY=<libngtcp2_crypto_ossl>] [-DQB_SSL_LIBRARY=<qb's libssl>]
#         [-DQB_CRYPTO_LIBRARY=<qb's libcrypto>] [-DQB_OPENSSL_INCLUDE_DIR=<its headers>] [-DQB_OPENSSL_VERSION=<x.y.z>]
#         [-DQB_QUIC_VERDICT=<the configure's>]
#         [-DWORK_DIR=<scratch directory>]
#         [-DQB_QUIC_NGTCP2_MAJOR=.. -DQB_QUIC_NGTCP2_PATH=.. -DQB_QUIC_QB_MAJOR=.. -DQB_QUIC_QB_PATH=..]
#         [-DQB_QUIC_NGTCP2_CRYPTO_MAJOR=.. -DQB_QUIC_NGTCP2_CRYPTO_PATH=.. -DQB_QUIC_QB_CRYPTO_MAJOR=..
#          -DQB_QUIC_QB_CRYPTO_PATH=..] [-DQB_QUIC_INPUTS=<the configure's inputs, '|'-separated>]
#         [-DQB_QUIC_FOUND_BY_QB=TRUE -DQB_QUIC_COMPARED=<bool>]
#         [-DQB_HAS_QUIC=<bool>] [-DQB_TARGET_APPLE=<bool>] [-DQB_TARGET_WIN32=<bool>]
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
if(_failures)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_tools} tool readings, wrong ones:${_failures}")
endif()

# qb's own libssl and libcrypto, read wherever the comparison could run: a file that is a versioned libssl or libcrypto
# must read, whether or not this configure found libngtcp2, so an ELF reader that reads nothing fails on every lane with
# a readelf.
set(_tool_exists FALSE)
if(APPLE)
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
    string(REGEX MATCH "^[0-9]+" _qb_headers_major "${QB_OPENSSL_VERSION}")
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
    if(_tool_exists AND NOT LIBRARY MATCHES "\\.(a|lib)$"
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
    message(STATUS "ngtcp2-openssl-listing: ${_cases} listings, ${_decisions} decisions and ${_tools} tool readings "
                   "right; ${_readings}, verdict '${_live_conflict}', QUIC ${QB_HAS_QUIC}${_live_note}")
else()
    message(STATUS "ngtcp2-openssl-listing: ${_cases} listings, ${_decisions} decisions and ${_tools} tool readings "
                   "right; qb's own OpenSSL read:${_qb_read}; no libngtcp2 in this configure")
endif()
