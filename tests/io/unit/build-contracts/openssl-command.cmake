#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The openssl command that makes qb's test certificates is the one of the OpenSSL qb found (Huly QB-1175). From what the
# configure chose it from -- OPENSSL_EXECUTABLE and its cache entry as they were before the choice, the found OpenSSL's
# include directory, the configure's default search and PATH's -- this makes the choice again, apart from
# _qb_openssl_command(), and checks the configure's: an OPENSSL_EXECUTABLE already set (by hand, by a variable, by a
# find_program(); not qb's own earlier choice as qb wrote it, nor NOTFOUND) as given, an empty one meaning no
# certificates; else the first that makes a certificate here of the found OpenSSL's own in the bin directory beside its
# include directory (not when cross-compiling), the default search's and PATH's -- on a host where they differ (Homebrew
# links openssl@3's while qb builds against openssl@4) the wiring of tests/io/system/CMakeLists.txt shows here -- or
# none, when none makes one. The certificate generator runs the command used, and that command runs and names a version.
# _qb_openssl_command()'s own choices are pinned by qb-io-test-unit-quic-openssl-verdict on planted prefixes.
#
# Run by ctest as qb-io-test-unit-openssl-command (registered whenever qb has SSL, a command chosen or none):
#   cmake -DUSED=<the command the configure used> -DBY_HAND=<its verdict: taken as given> -DGENERATOR=<the command the
#         certificate generator runs> -DOPENSSL_INCLUDE_DIR=<the found one's> -DVARIABLE=<OPENSSL_EXECUTABLE before the
#         choice> -DVARIABLE_DEFINED=<bool> -DCACHED=<its cache entry then> -DCACHED_DEFINED=<bool>
#         -DCACHED_HELP=<the entry's help string> -DOWN_HELP=<qb's own> -DCHOSEN=<what qb last wrote there, or
#         "<none>"> -DDEFAULT=<the default search's result>
#         -DPATH_ONLY=<PATH's> -DCROSSCOMPILING=<bool> -DPROBE_DIR=<a scratch directory> -P <this file>
#
if(POLICY CMP0152)
    cmake_policy(SET CMP0152 NEW) # file(REAL_PATH) resolves a link before a "..", as the file system does
endif()
function(makes_certificate command out_var) # as the configure's probe runs it, into PROBE_DIR
    file(REMOVE_RECURSE "${PROBE_DIR}")
    file(MAKE_DIRECTORY "${PROBE_DIR}")
    execute_process(COMMAND "${command}" req -x509 -newkey rsa:2048 -keyout "${PROBE_DIR}/key.pem"
                            -out "${PROBE_DIR}/cert.pem" -days 1 -nodes -subj "/CN=localhost"
                    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET TIMEOUT 60)
    file(REMOVE_RECURSE "${PROBE_DIR}")
    set(${out_var} FALSE PARENT_SCOPE)
    if("${_rc}" STREQUAL "0")
        set(${out_var} TRUE PARENT_SCOPE)
    endif()
endfunction()
set(_named "")
set(_why "no command makes a certificate here")
set(_mine FALSE) # the cache entry is qb's own earlier choice, as qb wrote it: chosen again
if(CACHED_DEFINED AND "${CACHED_HELP}" STREQUAL "${OWN_HELP}" AND NOT "${CHOSEN}" STREQUAL "<none>"
   AND "${CACHED}" STREQUAL "${CHOSEN}")
    set(_mine TRUE)
endif()
if(VARIABLE_DEFINED AND NOT "${VARIABLE}" MATCHES "(^|-)NOTFOUND$"
   AND NOT ("${VARIABLE}" STREQUAL "${CACHED}" AND _mine))
    set(_named "${VARIABLE}")
    set(_why "OPENSSL_EXECUTABLE was set")
endif()
set(_want "")
set(_want_by_hand FALSE)
if(_why STREQUAL "OPENSSL_EXECUTABLE was set")
    set(_want "${_named}")
    set(_want_by_hand TRUE)
else()
    set(_candidates "")
    if(NOT CROSSCOMPILING AND IS_ABSOLUTE "${OPENSSL_INCLUDE_DIR}")
        get_filename_component(_bin "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE) # as the configure resolves it
        set(_names openssl)
        if(CMAKE_HOST_WIN32)
            set(_names openssl.com openssl.exe openssl) # find_program()'s order there
        endif()
        foreach(_name IN LISTS _names)
            if(EXISTS "${_bin}/${_name}" AND NOT IS_DIRECTORY "${_bin}/${_name}")
                list(APPEND _candidates "${_bin}/${_name}|the found OpenSSL's own makes a certificate")
                break()
            endif()
        endforeach()
    endif()
    list(APPEND _candidates "${DEFAULT}|the default search's makes a certificate, nothing before it"
                            "${PATH_ONLY}|PATH's makes a certificate, nothing before it")
    foreach(_entry IN LISTS _candidates)
        string(REPLACE "|" ";" _entry "${_entry}")
        list(GET _entry 0 _candidate)
        list(GET _entry 1 _reason)
        if(IS_ABSOLUTE "${_candidate}")
            makes_certificate("${_candidate}" _makes)
            if(_makes)
                set(_want "${_candidate}")
                set(_why "${_reason}")
                break()
            endif()
        endif()
    endforeach()
endif()
set(_same FALSE)
if("${USED}" STREQUAL "${_want}")
    set(_same TRUE)
elseif(EXISTS "${USED}" AND EXISTS "${_want}")
    file(REAL_PATH "${USED}" _used_real)
    file(REAL_PATH "${_want}" _want_real)
    if(_used_real STREQUAL _want_real)
        set(_same TRUE)
    endif()
endif()
if(NOT _same)
    message(FATAL_ERROR "openssl-command: the test certificates are made by '${USED}', where '${_want}' was due: "
                        "${_why}")
endif()
if(NOT "${BY_HAND}" STREQUAL "${_want_by_hand}")
    message(FATAL_ERROR "openssl-command: the configure says named '${BY_HAND}', where it is '${_want_by_hand}'")
endif()
if(NOT USED) # empty, or a value CMake reads as false: no certificates generated
    if(NOT "${GENERATOR}" STREQUAL "")
        message(FATAL_ERROR "openssl-command: no command ('${USED}'), yet the certificate generator runs "
                            "'${GENERATOR}'")
    endif()
    message(STATUS "openssl-command: no command ('${USED}'), for the OpenSSL in '${OPENSSL_INCLUDE_DIR}': ${_why}")
    return()
endif()
if(NOT "${GENERATOR}" STREQUAL "${USED}")
    message(FATAL_ERROR "openssl-command: the certificate generator runs '${GENERATOR}', not the command chosen, "
                        "'${USED}'")
endif()
execute_process(COMMAND "${USED}" version OUTPUT_VARIABLE _version ERROR_VARIABLE _version RESULT_VARIABLE _rc
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _rc EQUAL 0 OR NOT _version MATCHES "[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "openssl-command: ${USED} version answered ${_rc}: ${_version}")
endif()
message(STATUS "openssl-command: ${USED} (${_version}) for the OpenSSL in ${OPENSSL_INCLUDE_DIR}: ${_why}")
