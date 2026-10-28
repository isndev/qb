# qb-io-test-unit-ngtcp2-openssl-listing (Huly QB-1179). FindNgtcp2.cmake reads which OpenSSL libngtcp2_crypto_ossl
# links, so that qbDependencies.cmake disables QUIC when qb links another major: two OpenSSL majors in one process crash
# at the first QUIC handshake (two copies of one major are reported, not refused). This runs the module's parser on the
# listings each platform's tool prints, its verdict on pairs of readings and its tool fallbacks (stand-in tools, off
# Windows), then the real libraries: qb's own libssl must read wherever its file is a versioned libssl and a tool
# exists, and -- when the configure compared them -- what the module reads now must be what the configure read, the
# verdict the one it acted on, and QUIC enabled unless they are two majors.
#
#   cmake -DFIND_MODULE=<FindNgtcp2.cmake> [-DLIBRARY=<libngtcp2_crypto_ossl>] [-DQB_SSL_LIBRARY=<qb's libssl>]
#         [-DQB_OPENSSL_VERSION=<x.y.z>] [-DQB_QUIC_VERDICT=<the configure's>] [-DWORK_DIR=<scratch directory>]
#         [-DQB_QUIC_NGTCP2_MAJOR=.. -DQB_QUIC_NGTCP2_PATH=.. -DQB_QUIC_QB_MAJOR=.. -DQB_QUIC_QB_PATH=..]
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
# <label> <listing> <expected major> <expected path>
function(expect label listing major path)
    _ngtcp2_openssl_in_listing("${listing}" _major _path)
    math(EXPR _n "${_cases} + 1")
    set(_cases ${_n} PARENT_SCOPE)
    if(NOT "${_major}" STREQUAL "${major}" OR NOT "${_path}" STREQUAL "${path}")
        set(_failures "${_failures}\n  ${label}: read major '${_major}' path '${_path}', expected '${major}' '${path}'"
            PARENT_SCOPE)
    endif()
endfunction()

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

if(_failures)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_cases} listings, wrong readings:${_failures}")
endif()

# The verdict qbDependencies.cmake acts on, for two readings: MAJORS disables QUIC, COPIES is reported, "" is one
# library or an unknown side.
set(_decisions 0)
function(decide label ngtcp2_major ngtcp2_path qb_major qb_path qb_static expected)
    _ngtcp2_openssl_conflict("${ngtcp2_major}" "${ngtcp2_path}" "${qb_major}" "${qb_path}" "${qb_static}" _verdict)
    math(EXPR _n "${_decisions} + 1")
    set(_decisions ${_n} PARENT_SCOPE)
    if(NOT "${_verdict}" STREQUAL "${expected}")
        set(_failures "${_failures}\n  ${label}: verdict '${_verdict}', expected '${expected}'" PARENT_SCOPE)
    endif()
endfunction()
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
    _ngtcp2_linked_openssl("${library}" _major _path)
    math(EXPR _n "${_tools} + 1")
    set(_tools ${_n} PARENT_SCOPE)
    if(NOT "${_major}" STREQUAL "${major}")
        set(_failures "${_failures}\n  ${label}: read major '${_major}', expected '${major}'" PARENT_SCOPE)
    endif()
endfunction()
if(WORK_DIR AND NOT WIN32 AND EXISTS /bin/sh)
    file(MAKE_DIRECTORY "${WORK_DIR}/tools")
    function(stand_in name soname rc) # prints one NEEDED entry, then exits <rc>
        file(WRITE "${WORK_DIR}/tools/${name}"
             "#!/bin/sh\necho ' 0x0000000000000001 (NEEDED) Shared library: [libssl.so.${soname}]'\nexit ${rc}\n")
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
if(_failures)
    message(FATAL_ERROR "ngtcp2-openssl-listing: ${_tools} tool readings, wrong ones:${_failures}")
endif()

# qb's own libssl, read wherever the comparison could run: a file that is a versioned libssl must read, whether or not
# this configure found libngtcp2, so an ELF reader that reads nothing fails on every lane with a readelf.
set(_qb_read "not read")
set(_tool_exists FALSE)
if(APPLE)
    find_program(_otool otool)
    if(_otool)
        set(_tool_exists TRUE)
    endif()
elseif(CMAKE_READELF OR CMAKE_OBJDUMP)
    set(_tool_exists TRUE)
endif()
if(NOT WIN32 AND QB_SSL_LIBRARY AND EXISTS "${QB_SSL_LIBRARY}" AND NOT QB_SSL_LIBRARY MATCHES "\\.a$")
    file(REAL_PATH "${QB_SSL_LIBRARY}" _qb_real)
    _ngtcp2_linked_openssl("${QB_SSL_LIBRARY}" _qb_own_major _qb_own_path)
    set(_qb_read "'${_qb_own_major}' (${_qb_real})")
    if(_tool_exists AND "${_qb_real}" MATCHES "/libssl(\\.so\\.[0-9]|\\.[0-9]+(\\.[0-9]+)*\\.dylib$)"
       AND ("${_qb_own_major}" STREQUAL "" OR (APPLE AND "${_qb_own_path}" STREQUAL "")))
        message(FATAL_ERROR "ngtcp2-openssl-listing: the module could not read qb's own libssl ${QB_SSL_LIBRARY} "
                            "(${_qb_real}): the configure-time check is vacuous on this host")
    endif()
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
    _ngtcp2_linked_openssl("${LIBRARY}" _live_major _live_path)
    _ngtcp2_linked_openssl("${QB_SSL_LIBRARY}" _qb_major _qb_path)
    if("${_qb_major}" STREQUAL "")
        string(REGEX MATCH "^[0-9]+" _qb_major "${QB_OPENSSL_VERSION}")
    endif()
    set(_qb_static FALSE)
    set(_qb_shown "${_qb_path}")
    if(NOT WIN32 AND "${QB_SSL_LIBRARY}" MATCHES "\\.a$")
        set(_qb_static TRUE)
        set(_qb_shown "${QB_SSL_LIBRARY}, static")
    endif()
    set(_live_note "")
    if(_tool_exists AND NOT LIBRARY MATCHES "\\.(a|lib)$" AND "${_live_major}" STREQUAL "")
        # An oracle of the test's own, not the module's reader: the tool's raw listing naming a libssl the module did
        # not read is a reader that reads nothing -- with a static libssl in qb, nothing else would show it on macOS.
        if(APPLE)
            execute_process(COMMAND "${_otool}" -L "${LIBRARY}" OUTPUT_VARIABLE _raw ERROR_QUIET)
        elseif(CMAKE_READELF)
            execute_process(COMMAND "${CMAKE_READELF}" -d "${LIBRARY}" OUTPUT_VARIABLE _raw ERROR_QUIET)
        else()
            execute_process(COMMAND "${CMAKE_OBJDUMP}" -p "${LIBRARY}" OUTPUT_VARIABLE _raw ERROR_QUIET)
        endif()
        if("${_raw}" MATCHES "libssl\\.(so\\.)?[0-9]")
            message(FATAL_ERROR "ngtcp2-openssl-listing: the tool's own listing of ${LIBRARY} names a libssl, the "
                                "module read none: its reader is broken on this host")
        endif()
        set(_live_note " -- it names no shared libssl: an OpenSSL linked into it is not seen")
    endif()
    # What the configure read must be what the module reads now: a configure whose reading is broken shows here even
    # on a host whose two libraries agree, and so does a library replaced since the configure (reconfigure then).
    if(DEFINED QB_QUIC_NGTCP2_MAJOR)
        foreach(_reading "NGTCP2_MAJOR;_live_major" "NGTCP2_PATH;_live_path" "QB_MAJOR;_qb_major" "QB_PATH;_qb_path")
            list(GET _reading 0 _configured)
            list(GET _reading 1 _now)
            if(NOT "${QB_QUIC_${_configured}}" STREQUAL "${${_now}}")
                message(FATAL_ERROR "ngtcp2-openssl-listing: the configure read ${_configured} "
                                    "'${QB_QUIC_${_configured}}', the module reads '${${_now}}' now: a library changed "
                                    "since the configure (reconfigure), or the configure's reading is broken")
            endif()
        endforeach()
    endif()
    _ngtcp2_openssl_conflict("${_live_major}" "${_live_path}" "${_qb_major}" "${_qb_path}" "${_qb_static}"
                             _live_conflict)
    if(DEFINED QB_QUIC_VERDICT AND NOT "${QB_QUIC_VERDICT}" STREQUAL "${_live_conflict}")
        message(FATAL_ERROR "ngtcp2-openssl-listing: the configure acted on the verdict '${QB_QUIC_VERDICT}', the "
                            "module gives '${_live_conflict}' on the same readings (${LIBRARY}, ${QB_SSL_LIBRARY})")
    endif()
    if(DEFINED QB_QUIC_VERDICT AND NOT "${QB_QUIC_VERDICT}" STREQUAL "MAJORS" AND NOT QB_HAS_QUIC)
        message(FATAL_ERROR "ngtcp2-openssl-listing: QUIC is disabled though libngtcp2 was found and the verdict is "
                            "'${QB_QUIC_VERDICT}': only two majors refuse it")
    endif()
    if(QB_HAS_QUIC AND "${_live_conflict}" STREQUAL "MAJORS")
        message(FATAL_ERROR "ngtcp2-openssl-listing: QUIC is enabled, yet ${LIBRARY} links OpenSSL ${_live_major} "
                            "(${_live_path}) and qb links ${_qb_major} (${_qb_shown})")
    endif()
    message(STATUS "ngtcp2-openssl-listing: ${_cases} listings, ${_decisions} decisions and ${_tools} tool readings "
                   "right; ${LIBRARY} links OpenSSL '${_live_major}' (${_live_path}), qb links '${_qb_major}' "
                   "(${_qb_shown}), verdict '${_live_conflict}', QUIC ${QB_HAS_QUIC}${_live_note}")
else()
    message(STATUS "ngtcp2-openssl-listing: ${_cases} listings, ${_decisions} decisions and ${_tools} tool readings "
                   "right; qb's libssl read ${_qb_read}; no libngtcp2 in this configure")
endif()
