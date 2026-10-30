#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# -----------------------------------------------------------------------------
# qb Framework - qb's OpenSSL, and whether libngtcp2's can join it
#
# _qb_find_openssl() finds the OpenSSL qb links, searching again a path its own search found that is gone,
# _qb_check_openssl() stops the configure when its headers, libssl and libcrypto are not one major,
# _qb_openssl_command() finds that OpenSSL's openssl command for the test certificates (a function), and
# _qb_decide_quic(), once find_package(Ngtcp2) has run, decides whether the OpenSSL libngtcp2_crypto_ossl links and
# qb's can share one process, and so whether QUIC is enabled. A file of its own so that a fixture drives them with
# planted libraries (qb-io-test-unit-quic-openssl-verdict, Huly QB-1185, QB-1190); macros so that what they find and
# read stays in the caller's scope -- qbDependencies.cmake's, where tests/io/unit/CMakeLists.txt hands it to
# qb-io-test-unit-ngtcp2-openssl-listing.
# -----------------------------------------------------------------------------

# FindNgtcp2.cmake's library reader, QUIC or not: _qb_check_openssl() reads qb's libssl and libcrypto with it. Its
# functions only (NGTCP2_FIND_FUNCTIONS_ONLY): find_package(Ngtcp2) searches later, when QUIC is wanted.
set(NGTCP2_FIND_FUNCTIONS_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/FindNgtcp2.cmake")
unset(NGTCP2_FIND_FUNCTIONS_ONLY)

# _qb_forget_vanished(), _qb_record_found() and _qb_stop_if_gone(): a path the search itself found that is gone is
# searched again, and found nowhere else stops the configure (Huly QB-1185, QB-1192).
include("${CMAKE_CURRENT_LIST_DIR}/qbFoundLibraries.cmake")

# _qb_find_openssl() -- qbDependencies.cmake's find_package(OpenSSL), with the three entries it caches searched again
# when its own search found them and they are gone (qbFoundLibraries.cmake), and stopping when that search finds
# nothing. FindOpenSSL takes its hints from pkg-config, whose answer FindPkgConfig keeps cached until
# __pkg_config_checked__OPENSSL is gone: it is cleared with them, or the hints would still name the removed directory.
# The files found -- libssl and libcrypto as found and as resolved, and opensslv.h, whose version the configure
# reads -- become inputs of the configure, so that `cmake --build` re-runs it after an OpenSSL update, QUIC or not.
# Not on Windows: MSVC's FindOpenSSL caches its libraries under other names, and nothing is read there.
macro(_qb_find_openssl)
    set(_qb_openssl_entries OPENSSL_INCLUDE_DIR/openssl/ssl.h OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
    set(_qb_openssl_forgot "")
    # QB-1185's code recorded each entry its search found, and not whether it found OpenSSL: a build directory it
    # configured had an OpenSSL when what FindOpenSSL needs to find one -- the headers and libcrypto -- was there, each
    # recorded or set (by hand, by a parent project), one at least recorded -- read before this configure records its
    # own.
    set(_qb_openssl_recorded FALSE)
    if((DEFINED CACHE{_QB_FOUND_OPENSSL_INCLUDE_DIR} OR IS_ABSOLUTE "$CACHE{OPENSSL_INCLUDE_DIR}")
       AND (DEFINED CACHE{_QB_FOUND_OPENSSL_CRYPTO_LIBRARY} OR IS_ABSOLUTE "$CACHE{OPENSSL_CRYPTO_LIBRARY}")
       AND (DEFINED CACHE{_QB_FOUND_OPENSSL_INCLUDE_DIR} OR DEFINED CACHE{_QB_FOUND_OPENSSL_CRYPTO_LIBRARY}))
        set(_qb_openssl_recorded TRUE)
    endif()
    set(_qb_openssl_searched TRUE) # off Windows, and unless the search is turned off: nothing searched, nothing said
    if(WIN32 OR CMAKE_DISABLE_FIND_PACKAGE_OpenSSL)
        set(_qb_openssl_searched FALSE)
    endif()
    # FindOpenSSL searches zlib itself for an OpenSSL whose libraries name it (a static one, -lz in its pkg-config),
    # before qbDependencies.cmake's own zlib search, which would then take what it found unsearched, and links it
    # (ZLIB::ZLIB in its targets' interface). Unless zlib's search is turned off, its entries are searched again around
    # it, quietly -- whether FindOpenSSL searches zlib is known only after -- and said once a search looks: here where
    # FindOpenSSL's did (it linked zlib), else by _qb_find_zlib() as qb's own begins (compression on); nothing else
    # searches zlib, so nothing else is said. What FindOpenSSL's search finds is recorded, and its files are inputs.
    set(_qb_openssl_zlib "")
    set(_qb_openssl_zlib_forgot "")
    if(_qb_openssl_searched AND NOT CMAKE_DISABLE_FIND_PACKAGE_ZLIB)
        set(_qb_openssl_zlib ZLIB_INCLUDE_DIR/zlib.h ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG)
    endif()
    if(_qb_openssl_searched)
        _qb_forget_vanished(_qb_openssl_forgot ${_qb_openssl_entries})
        if(_qb_openssl_forgot)
            unset(__pkg_config_checked__OPENSSL CACHE)
        endif()
        set(_qb_forget_quiet TRUE) # said below (_qb_forget_tell), once known whether a search looked
        _qb_forget_vanished(_qb_openssl_zlib_forgot ${_qb_openssl_zlib})
        unset(_qb_forget_quiet)
    endif()
    find_package(OpenSSL QUIET)
    if(_qb_openssl_searched)
        # each entry found, OpenSSL found or not: a partial search counts
        _qb_record_found(${_qb_openssl_entries} ${_qb_openssl_zlib})
        set(_qb_openssl_links_zlib FALSE) # read by _qb_find_zlib() too
        foreach(_qb_openssl_target IN ITEMS OpenSSL::Crypto OpenSSL::SSL)
            if(TARGET ${_qb_openssl_target})
                get_target_property(_qb_openssl_link ${_qb_openssl_target} INTERFACE_LINK_LIBRARIES)
                if("ZLIB::ZLIB" IN_LIST _qb_openssl_link)
                    set(_qb_openssl_links_zlib TRUE)
                endif()
            endif()
        endforeach()
        if(_qb_openssl_links_zlib)
            _qb_forget_tell(_qb_openssl_zlib_forgot) # FindOpenSSL's search looked
            if(ZLIB_FOUND) # this build directory has a zlib: qb's own search, compression turned on, stops on its loss
                set(_QB_HAD_ZLIB TRUE CACHE INTERNAL "")
            endif()
            _qb_found_inputs(${_qb_openssl_zlib}) # qbDependencies.cmake's own search may not run (compression off)
            # Lost, and with compression off no search of qb's own follows to stop on it: the generate would fail on
            # the ZLIB::ZLIB OpenSSL links, naming neither.
            if(NOT QB_WITH_COMPRESSION AND NOT TARGET ZLIB::ZLIB)
                string(CONCAT _qb_openssl_zlib_remedy
                       "the OpenSSL found links it: install one, point CMAKE_PREFIX_PATH (or ZLIB_ROOT) at one, "
                       "configure with -DQB_WITH_SSL=OFF, or remove OpenSSL's three entries for one that does not "
                       "link zlib (cmake -U OPENSSL_INCLUDE_DIR -U OPENSSL_SSL_LIBRARY -U OPENSSL_CRYPTO_LIBRARY "
                       "-DOPENSSL_ROOT_DIR=<it> <build directory>)")
                _qb_stop_if_gone("${_qb_openssl_zlib_forgot}" zlib "${_qb_openssl_zlib_remedy}")
            endif()
        endif()
    endif()
    # What was searched again and is not found now stops the configure, whatever OpenSSL_FOUND says (FindOpenSSL
    # requires libcrypto and the headers only, and finds OpenSSL without libssl) -- once this build directory has found
    # an OpenSSL: what it never found one with (stray headers in a shared prefix, libraries without headers) stops
    # nothing, nor does an OpenSSL turned off (CMAKE_DISABLE_FIND_PACKAGE_OpenSSL, as a qb CI lane configures).
    if(NOT WIN32 AND OpenSSL_FOUND)
        set(_QB_HAD_OpenSSL TRUE CACHE INTERNAL "")
    elseif(NOT DEFINED CACHE{_QB_HAD_OpenSSL})
        set(_QB_HAD_OpenSSL ${_qb_openssl_recorded} CACHE INTERNAL "")
    endif()
    if("$CACHE{_QB_HAD_OpenSSL}") # turned off, nothing was searched again: _qb_openssl_forgot is empty
        _qb_stop_if_gone("${_qb_openssl_forgot}" OpenSSL
                         "install one, point OPENSSL_ROOT_DIR at one, or configure with -DQB_WITH_SSL=OFF")
    endif()
    set(_qb_openssl_inputs "")
    if(NOT WIN32 AND OpenSSL_FOUND)
        foreach(_qb_openssl_read IN ITEMS "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}"
                                          "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h")
            if(IS_ABSOLUTE "${_qb_openssl_read}" AND EXISTS "${_qb_openssl_read}")
                file(REAL_PATH "${_qb_openssl_read}" _qb_openssl_read_real)
                list(APPEND _qb_openssl_inputs "${_qb_openssl_read}" "${_qb_openssl_read_real}")
            endif()
        endforeach()
        if(_qb_openssl_inputs)
            list(REMOVE_DUPLICATES _qb_openssl_inputs)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_qb_openssl_inputs})
        endif()
    endif()
endmacro()

# One of qb's OpenSSL libraries, for _qb_check_openssl()'s message, into <out_var>: its cache entry, its path and the
# major it was taken as -- read from its file, or its headers' when it was not.
function(_qb_openssl_library_said entry name read out_var)
    set(_said "${entry} (${${entry}}) ")
    set(_as "")
    if(NOT "${_qb_openssl_headers_major}" STREQUAL "")
        set(_as ", taken as major ${_qb_openssl_headers_major} like its headers")
    endif()
    if(NOT "${read}" STREQUAL "")
        string(APPEND _said "is a lib${name} of major ${read}")
    elseif("${${entry}}" MATCHES "\\.a$")
        string(APPEND _said "is a static lib${name}${_as}")
    else()
        string(APPEND _said "was not read${_as}")
    endif()
    set(${out_var} "${_said}" PARENT_SCOPE)
endfunction()

# _qb_check_openssl() -- after _qb_find_openssl() found OpenSSL: its headers, OPENSSL_SSL_LIBRARY and
# OPENSSL_CRYPTO_LIBRARY must be one major, or the configure stops with the remedy (Huly QB-1190). FindOpenSSL caches
# the three apart, so a cache cleared in part re-finds one and keeps the others. Measured on macOS with QUIC off: the
# headers of 3.5.9 with the libssl of 4.1 and the libcrypto of 3.5.9 configured and built green, and both TLS test
# programs crashed (SIGSEGV); the libssl of 3.5.9 with the libcrypto of 4.1 crashed them too (SIGBUS); the headers of
# 4.1 over the libraries of 3.5.9 failed to link. Not a missing dependency but a cache that contradicts itself, which
# one command repairs: an error, not a build without TLS that nobody asked for.
#
# The major compared is the number a library's file name carries (libssl.so.3, libssl.4.dylib, libssl.so.1.1): each
# library's is read as _qb_decide_quic() reads it -- FindNgtcp2.cmake's reader: the dependency list of the file the
# loader opens, its own install name or SONAME first -- and the headers' is the one they name their libraries with,
# OPENSSL_SHLIB_VERSION (3.0 on) or SHLIB_VERSION_NUMBER (1.1), whose first number a build may set apart from the
# release's (quictls names 3.1's libssl.so.81.3); the release's major only when neither is there. A static archive and a
# library no tool reads take their headers' major: no answer, never a guess. Compared on Linux and macOS, where a
# library's file name carries that number; not elsewhere, where a base system may name it otherwise (FreeBSD's
# libssl.so.30 holds OpenSSL 3, with headers that say 3), not on Windows (MSVC's FindOpenSSL can hand optimized/debug
# lists, and nothing is read there), and not for LibreSSL, which numbers its libssl and libcrypto apart. The files read
# -- on macOS the install name each library records and its file -- become inputs of the configure beside those
# _qb_find_openssl() made ones: `cmake --build` checks again when an update moves one. Leaves in the caller's scope
# _qb_openssl_compared, _qb_openssl_headers_major, _qb_openssl_ssl_read and _qb_openssl_crypto_read (the majors read,
# empty when nothing was) and _qb_openssl_check_inputs, which tests/io/unit/CMakeLists.txt hands to
# qb-io-test-unit-ngtcp2-openssl-listing.
macro(_qb_check_openssl)
    string(REGEX MATCH "^[0-9]+" _qb_openssl_headers_major "${OPENSSL_VERSION}")
    set(_qb_openssl_ssl_read "")
    set(_qb_openssl_crypto_read "")
    set(_qb_openssl_check_inputs "")
    set(_qb_openssl_compared FALSE)
    set(_qb_openssl_libressl FALSE)
    set(_qb_openssl_shlib "")
    if(EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h")
        file(STRINGS "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h" _qb_openssl_defines REGEX
             "^[ \t]*#[ \t]*define[ \t]+(LIBRESSL_VERSION_NUMBER|OPENSSL_SHLIB_VERSION|SHLIB_VERSION_NUMBER)[ \t]")
        foreach(_qb_openssl_define IN LISTS _qb_openssl_defines)
            if(_qb_openssl_define MATCHES "LIBRESSL_VERSION_NUMBER")
                set(_qb_openssl_libressl TRUE)
            elseif(_qb_openssl_define MATCHES "OPENSSL_SHLIB_VERSION[ \t]+\"?([0-9]+)")
                set(_qb_openssl_shlib "${CMAKE_MATCH_1}")
            elseif(_qb_openssl_define MATCHES "SHLIB_VERSION_NUMBER[ \t]+\"([0-9]+)")
                if("${_qb_openssl_shlib}" STREQUAL "") # 1.1's; OPENSSL_SHLIB_VERSION wins when both are there
                    set(_qb_openssl_shlib "${CMAKE_MATCH_1}")
                endif()
            endif()
        endforeach()
    endif()
    if(NOT "${_qb_openssl_shlib}" STREQUAL "")
        set(_qb_openssl_headers_major "${_qb_openssl_shlib}")
    endif()
    if(WIN32)
        # nothing is read on Windows
    elseif(NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
        string(CONCAT _qb_openssl_not_compared
               "OpenSSL majors not compared on ${CMAKE_SYSTEM_NAME}: a library's file name need not carry its "
               "release's major there")
        qb_status_message("${_qb_openssl_not_compared}")
    elseif(_qb_openssl_libressl)
        qb_status_message("OpenSSL majors not compared: LibreSSL numbers its libssl and libcrypto apart")
    else()
        set(_qb_openssl_compared TRUE)
        _ngtcp2_linked_openssl("${OPENSSL_SSL_LIBRARY}" _qb_openssl_ssl_read _qb_openssl_unused _qb_openssl_unused
                               _qb_openssl_unused)
        _ngtcp2_linked_openssl("${OPENSSL_CRYPTO_LIBRARY}" _qb_openssl_unused _qb_openssl_unused _qb_openssl_crypto_read
                               _qb_openssl_unused)
        foreach(_qb_openssl_library IN ITEMS "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}")
            if(IS_ABSOLUTE "${_qb_openssl_library}" AND EXISTS "${_qb_openssl_library}")
                _ngtcp2_loaded_file("${_qb_openssl_library}" _qb_openssl_loaded)
                file(REAL_PATH "${_qb_openssl_loaded}" _qb_openssl_loaded_real)
                list(APPEND _qb_openssl_check_inputs "${_qb_openssl_loaded}" "${_qb_openssl_loaded_real}")
            endif()
        endforeach()
        if(_qb_openssl_check_inputs)
            list(REMOVE_DUPLICATES _qb_openssl_check_inputs)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_qb_openssl_check_inputs})
        endif()
    endif()
    set(_qb_openssl_majors "")
    foreach(_qb_openssl_major IN ITEMS "${_qb_openssl_headers_major}" "${_qb_openssl_ssl_read}"
                                       "${_qb_openssl_crypto_read}")
        if(NOT "${_qb_openssl_major}" STREQUAL "")
            list(APPEND _qb_openssl_majors "${_qb_openssl_major}")
        endif()
    endforeach()
    if(_qb_openssl_majors)
        list(REMOVE_DUPLICATES _qb_openssl_majors)
    endif()
    list(LENGTH _qb_openssl_majors _qb_openssl_count)
    if(_qb_openssl_count GREATER 1)
        set(_qb_openssl_headers_said "the headers of OpenSSL ${OPENSSL_VERSION}")
        if("${OPENSSL_VERSION}" STREQUAL "")
            set(_qb_openssl_headers_said "OpenSSL headers whose version FindOpenSSL could not read")
        endif()
        if(NOT "${_qb_openssl_headers_major}" STREQUAL "")
            string(APPEND _qb_openssl_headers_said ", for libraries of major ${_qb_openssl_headers_major}")
        endif()
        _qb_openssl_library_said(OPENSSL_SSL_LIBRARY ssl "${_qb_openssl_ssl_read}" _qb_openssl_ssl_said)
        _qb_openssl_library_said(OPENSSL_CRYPTO_LIBRARY crypto "${_qb_openssl_crypto_read}" _qb_openssl_crypto_said)
        string(CONCAT _qb_openssl_split
               "qb's OpenSSL is not one major: OPENSSL_INCLUDE_DIR (${OPENSSL_INCLUDE_DIR}) holds "
               "${_qb_openssl_headers_said}; ${_qb_openssl_ssl_said}; and ${_qb_openssl_crypto_said}. A program built "
               "on them loads two OpenSSL majors -- measured, qb's TLS crashed on that -- or fails to link (Huly "
               "QB-1190). FindOpenSSL caches the three apart: one removed from the cache, with OPENSSL_ROOT_DIR "
               "pointed at another OpenSSL, is found there while the other two are kept. Remove all three -- cmake "
               "-U OPENSSL_INCLUDE_DIR -U OPENSSL_SSL_LIBRARY -U OPENSSL_CRYPTO_LIBRARY [-DOPENSSL_ROOT_DIR=<one "
               "OpenSSL>] <build directory> -- or configure with --fresh")
        qb_error_message("${_qb_openssl_split}")
    endif()
endmacro()

# _qb_openssl_makes_certificate(<command> <out_var>) -- TRUE in <out_var> when <command> makes a self-signed
# certificate here: an RSA key and a `req -x509`, the core of the test certificates' generator (which adds a
# subjectAltName where the command's version allows one). Answering `version` proves less: an OpenSSL without its
# openssl.cnf answers it, then fails `req`, and a target's binary does not run here at all.
function(_qb_openssl_makes_certificate command out_var)
    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/qb-openssl-command")
    file(REMOVE_RECURSE "${_dir}")
    file(MAKE_DIRECTORY "${_dir}")
    execute_process(COMMAND "${command}" req -x509 -newkey rsa:2048 -keyout "${_dir}/key.pem" -out "${_dir}/cert.pem"
                            -days 1 -nodes -subj "/CN=localhost"
                    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET TIMEOUT 60)
    file(REMOVE_RECURSE "${_dir}")
    set(_makes FALSE)
    if("${_rc}" STREQUAL "0")
        set(_makes TRUE)
    endif()
    set(${out_var} ${_makes} PARENT_SCOPE)
endfunction()

# _qb_openssl_command(<out_var> <named_var>) -- the openssl command that makes qb's test certificates, into <out_var>.
# An OPENSSL_EXECUTABLE already set is taken as given, as find_program(OPENSSL_EXECUTABLE) takes it, <named_var> then
# TRUE: named by hand (-DOPENSSL_EXECUTABLE=..., as qb's CI lanes pass it), by a parent project's or a toolchain's
# variable, or found by someone's find_program() -- who set a cache entry cannot be told, since find_program() gives the
# one it keeps its own help string, whoever named it; an empty value makes no certificates, and NOTFOUND or
# <name>-NOTFOUND is no value, as for find_program(). An entry with find_program()'s help string is said once: one an
# older qb configure left holds that configure's search (CMAKE_PREFIX_PATH's and CMAKE_PROGRAM_PATH's before PATH's).
# Else the first of these that makes a certificate here: the one in the bin directory beside the found OpenSSL's include
# directory -- not when cross-compiling, where that one is the target's -- the default search's (not re-rooted into the
# target's root when cross-compiling, whatever CMAKE_FIND_ROOT_PATH_MODE_PROGRAM says), then PATH's. The default search
# takes CMAKE_PREFIX_PATH's, CMAKE_PROGRAM_PATH's, then PATH's, and one given that directory as a hint still puts those
# two before it, which can hold another installation's (Homebrew links openssl@3's while qb builds against openssl@4;
# macOS's /usr/bin/openssl is LibreSSL) or, cross-compiling, the target's, which is taken only if it makes a certificate
# here (Huly QB-1175). Searched at every configure, so that it follows qb onto another OpenSSL; empty when none makes a
# certificate. qb's choice is kept as the cache entry, under _QB_OPENSSL_COMMAND_HELP, where there is none or qb's own:
# a find_program(OPENSSL_EXECUTABLE) run after qb in the same build takes it, as it took the entry qb's own
# find_program() left -- an empty one too, which find_program() keeps, where a removed or NOTFOUND one would have it
# search -- and the next configure chooses again. The entry is qb's own while it holds the value qb wrote
# (_QB_OPENSSL_COMMAND_CHOSEN): edited in place (ccmake, cmake-gui and an edited CMakeCache.txt keep the help string),
# it is a command named.
set(_QB_OPENSSL_COMMAND_HELP "The openssl command qb chose for its test certificates; set it to name one")
function(_qb_openssl_command out_var named_var)
    set(${named_var} FALSE PARENT_SCOPE)
    set(_mine FALSE) # the cache entry is qb's own last choice, as qb wrote it
    set(_help "")
    if(DEFINED CACHE{OPENSSL_EXECUTABLE})
        get_property(_help CACHE OPENSSL_EXECUTABLE PROPERTY HELPSTRING)
        if(_help STREQUAL _QB_OPENSSL_COMMAND_HELP AND DEFINED CACHE{_QB_OPENSSL_COMMAND_CHOSEN}
           AND "$CACHE{OPENSSL_EXECUTABLE}" STREQUAL "$CACHE{_QB_OPENSSL_COMMAND_CHOSEN}")
            set(_mine TRUE)
        endif()
    endif()
    if(DEFINED OPENSSL_EXECUTABLE AND NOT "${OPENSSL_EXECUTABLE}" MATCHES "(^|-)NOTFOUND$"
       AND NOT ("${OPENSSL_EXECUTABLE}" STREQUAL "$CACHE{OPENSSL_EXECUTABLE}" AND _mine))
        if("${OPENSSL_EXECUTABLE}" STREQUAL "$CACHE{OPENSSL_EXECUTABLE}" AND _help STREQUAL "Path to a program."
           AND NOT "${OPENSSL_EXECUTABLE}" STREQUAL "$CACHE{_QB_OPENSSL_SEARCH_SAID}")
            string(CONCAT _said "OPENSSL_EXECUTABLE (${OPENSSL_EXECUTABLE}) is a find_program() entry -- a parent "
                                "project's search, or an older qb configure's -- taken as given, as find_program() "
                                "takes it; one an older qb left, -U OPENSSL_EXECUTABLE lets qb take the found "
                                "OpenSSL's command (Huly QB-1175)")
            qb_status_message("${_said}") # one argument: several come back joined by ";"
            set(_QB_OPENSSL_SEARCH_SAID "${OPENSSL_EXECUTABLE}" CACHE INTERNAL "")
        endif()
        set(${out_var} "${OPENSSL_EXECUTABLE}" PARENT_SCOPE)
        set(${named_var} TRUE PARENT_SCOPE)
        return()
    endif()
    set(_candidates "")
    if(IS_ABSOLUTE "${OPENSSL_INCLUDE_DIR}" AND NOT CMAKE_CROSSCOMPILING)
        get_filename_component(_bin "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)
        find_program(_qb_openssl_beside openssl HINTS "${_bin}" NO_DEFAULT_PATH NO_CACHE)
        list(APPEND _candidates "${_qb_openssl_beside}")
    endif()
    set(_where "")
    if(CMAKE_CROSSCOMPILING)
        set(_where NO_CMAKE_FIND_ROOT_PATH) # not re-rooted into the target's root
    endif()
    find_program(_qb_openssl_default openssl ${_where} NO_CACHE)
    find_program(_qb_openssl_path openssl NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH
                 NO_CMAKE_FIND_ROOT_PATH NO_CACHE) # PATH's alone, of this host
    list(APPEND _candidates "${_qb_openssl_default}" "${_qb_openssl_path}")
    set(_command "")
    foreach(_candidate IN LISTS _candidates)
        if(IS_ABSOLUTE "${_candidate}")
            _qb_openssl_makes_certificate("${_candidate}" _makes)
            if(_makes)
                set(_command "${_candidate}")
                break()
            endif()
        endif()
    endforeach()
    if(NOT DEFINED CACHE{OPENSSL_EXECUTABLE} OR _mine) # never over another's entry, a NOTFOUND one included
        set(OPENSSL_EXECUTABLE "${_command}" CACHE FILEPATH "${_QB_OPENSSL_COMMAND_HELP}" FORCE)
        set(_QB_OPENSSL_COMMAND_CHOSEN "${_command}" CACHE INTERNAL "")
    endif()
    set(${out_var} "${_command}" PARENT_SCOPE)
endfunction()

# One side's OpenSSL, for a message, into <out_var>: "OpenSSL <major> (<path>)" when its libssl and libcrypto are one
# major, else the libssl and the libcrypto each with its own.
function(_qb_quic_openssl_side ssl_major ssl_path crypto_major crypto_path out_var)
    if("${ssl_major}" STREQUAL "" AND "${crypto_major}" STREQUAL "")
        set(_side "an OpenSSL no tool could read")
    elseif("${crypto_major}" STREQUAL "" OR "${crypto_major}" STREQUAL "${ssl_major}")
        set(_side "OpenSSL ${ssl_major}")
        if(NOT "${ssl_path}" STREQUAL "")
            string(APPEND _side " (${ssl_path})")
        endif()
    elseif("${ssl_major}" STREQUAL "")
        set(_side "OpenSSL ${crypto_major}")
        if(NOT "${crypto_path}" STREQUAL "")
            string(APPEND _side " (${crypto_path})")
        endif()
    else()
        set(_side "the libssl of OpenSSL ${ssl_major}")
        if(NOT "${ssl_path}" STREQUAL "")
            string(APPEND _side " (${ssl_path})")
        endif()
        string(APPEND _side " with the libcrypto of OpenSSL ${crypto_major}")
        if(NOT "${crypto_path}" STREQUAL "")
            string(APPEND _side " (${crypto_path})")
        endif()
    endif()
    set(${out_var} "${_side}" PARENT_SCOPE)
endfunction()

# _qb_decide_quic() -- after find_package(Ngtcp2) in the QUIC block, with SSL found: reads, compares and reports, then
# sets QB_HAS_QUIC and appends the libngtcp2 targets to QB_EXTERNAL_LIBRARIES when QUIC is enabled; stops when a
# libngtcp2 this build directory found is gone and found nowhere else (Ngtcp2_VANISHED). Reads Ngtcp2_FOUND and the
# module's Ngtcp2_OPENSSL_* readings, OPENSSL_SSL_LIBRARY, OPENSSL_CRYPTO_LIBRARY, OPENSSL_VERSION and
# OPENSSL_INCLUDE_DIR, and _qb_quic_required (QB_WITH_QUIC=ON rather than AUTO).
#
# libngtcp2_crypto_ossl calls the OpenSSL it was built against, qb the one it found. Two majors in one process crash at
# the first QUIC handshake (a Homebrew update moved libngtcp2 onto openssl@4 while qb found openssl@3: SSL_set0_rbio of
# libssl.4 on an SSL of libssl.3, Huly QB-1179), so they disable QUIC -- a libcrypto of another major than its libssl
# included (QB-1185: a cache cleared in part leaves qb's so, which _qb_check_openssl() has refused already where it
# compares); two copies of one major -- a static libssl in qb, or two prefixes where Mach-O names them -- are reported
# and kept. qb's own libssl and libcrypto are read the same way (their headers' major when nothing reads them), unless
# a parent's FindNgtcp2 module won the search: only qb's module defines its readings (Ngtcp2_OPENSSL_MAJOR, empty when
# it read nothing), and nothing is compared without them.
#
# The files read become inputs of the configure (CMAKE_CONFIGURE_DEPENDS): `cmake --build` re-runs it when a package
# update rewrites one, removes one, or moves the link that names one (QB-1185), instead of keeping a verdict about
# libraries that are no longer the ones a binary loads -- each library as found, the file it resolves to, on Mach-O the
# install name dyld loads and its file, and qb's opensslv.h, whose major stands in for a library nothing reads. Only
# files that exist: a missing input re-runs every build.
macro(_qb_decide_quic)
    set(_qb_quic_conflict "")
    set(_qb_quic_inputs "")
    if(Ngtcp2_FOUND AND DEFINED Ngtcp2_OPENSSL_MAJOR)
        set(_qb_quic_compared TRUE)
        _ngtcp2_linked_openssl("${OPENSSL_SSL_LIBRARY}" _qb_ssl_major _qb_ssl_path _qb_ssl_crypto_major
                               _qb_ssl_crypto_path)
        _ngtcp2_linked_openssl("${OPENSSL_CRYPTO_LIBRARY}" _qb_crypto_ssl_major _qb_crypto_ssl_path _qb_crypto_major
                               _qb_crypto_path)
        set(_qb_headers_major "${_qb_openssl_headers_major}") # the check's: the number they name libraries with
        if("${_qb_ssl_major}" STREQUAL "")
            set(_qb_ssl_major "${_qb_headers_major}")
        endif()
        if("${_qb_crypto_major}" STREQUAL "")
            set(_qb_crypto_major "${_qb_headers_major}")
        endif()
        set(_qb_ssl_static FALSE)
        if(NOT WIN32 AND "${OPENSSL_SSL_LIBRARY}" MATCHES "\\.a$")
            set(_qb_ssl_static TRUE)
        endif()
        _ngtcp2_openssl_conflict("${Ngtcp2_OPENSSL_MAJOR}" "${Ngtcp2_OPENSSL_LIBRARY}"
                                 "${Ngtcp2_OPENSSL_CRYPTO_MAJOR}" "${Ngtcp2_OPENSSL_CRYPTO_LIBRARY}"
                                 "${_qb_ssl_major}" "${_qb_ssl_path}" "${_qb_crypto_major}" "${_qb_crypto_path}"
                                 "${_qb_ssl_static}" _qb_quic_conflict)
        if(NOT WIN32)
            foreach(_qb_quic_read IN ITEMS "${NGTCP2_CRYPTO_OSSL_LIBRARY}" "${OPENSSL_SSL_LIBRARY}"
                                           "${OPENSSL_CRYPTO_LIBRARY}")
                if(IS_ABSOLUTE "${_qb_quic_read}" AND EXISTS "${_qb_quic_read}")
                    _ngtcp2_loaded_file("${_qb_quic_read}" _qb_quic_loaded)
                    file(REAL_PATH "${_qb_quic_read}" _qb_quic_read_real)
                    file(REAL_PATH "${_qb_quic_loaded}" _qb_quic_loaded_real)
                    list(APPEND _qb_quic_inputs "${_qb_quic_read}" "${_qb_quic_read_real}" "${_qb_quic_loaded}"
                         "${_qb_quic_loaded_real}")
                endif()
            endforeach()
            set(_qb_quic_read "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h")
            if(IS_ABSOLUTE "${OPENSSL_INCLUDE_DIR}" AND EXISTS "${_qb_quic_read}")
                file(REAL_PATH "${_qb_quic_read}" _qb_quic_read_real)
                list(APPEND _qb_quic_inputs "${_qb_quic_read}" "${_qb_quic_read_real}")
            endif()
            # libngtcp2's core library and headers, where its search produced them: their removal re-runs the
            # configure too, which searches them again (Huly QB-1192). qb's own _qb_held_file(): a parent's FindNgtcp2
            # may have won the search, and FindNgtcp2.cmake's functions-only include defines no _ngtcp2_held_file().
            foreach(_qb_quic_spec IN ITEMS "NGTCP2_LIBRARY|" "NGTCP2_INCLUDE_DIR|/ngtcp2/ngtcp2.h"
                                           "NGTCP2_CRYPTO_INCLUDE_DIR|/ngtcp2/ngtcp2_crypto.h"
                                           "NGTCP2_CRYPTO_OSSL_INCLUDE_DIR|/ngtcp2/ngtcp2_crypto_ossl.h")
                string(REPLACE "|" ";" _qb_quic_spec "${_qb_quic_spec}")
                list(GET _qb_quic_spec 0 _qb_quic_entry)
                list(GET _qb_quic_spec 1 _qb_quic_probe)
                _qb_held_file("$CACHE{${_qb_quic_entry}}" "${_qb_quic_probe}" _qb_quic_read)
                if("$CACHE{${_qb_quic_entry}}" STREQUAL "$CACHE{_NGTCP2_FOUND_${_qb_quic_entry}}"
                   AND IS_ABSOLUTE "${_qb_quic_read}" AND EXISTS "${_qb_quic_read}"
                   AND NOT IS_DIRECTORY "${_qb_quic_read}")
                    list(APPEND _qb_quic_inputs "${_qb_quic_read}")
                endif()
            endforeach()
            if(_qb_quic_inputs)
                list(REMOVE_DUPLICATES _qb_quic_inputs)
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_qb_quic_inputs})
            endif()
        endif()
    endif()
    if(_qb_quic_conflict)
        set(_qb_quic_remedy "the OpenSSL libngtcp2 uses")
        if("${Ngtcp2_OPENSSL_MAJOR}${Ngtcp2_OPENSSL_CRYPTO_MAJOR}" STREQUAL "")
            set(_qb_quic_remedy "one OpenSSL") # libngtcp2's was not read: qb's own libssl and libcrypto disagree
        endif()
        if("${Ngtcp2_OPENSSL_LIBRARY}" MATCHES "^(/.+)/lib/[^/]+$")
            string(APPEND _qb_quic_remedy " (-DOPENSSL_ROOT_DIR=${CMAKE_MATCH_1})")
        endif()
        _qb_quic_openssl_side("${Ngtcp2_OPENSSL_MAJOR}" "${Ngtcp2_OPENSSL_LIBRARY}" "${Ngtcp2_OPENSSL_CRYPTO_MAJOR}"
                              "${Ngtcp2_OPENSSL_CRYPTO_LIBRARY}" _qb_ngtcp2_ssl)
        _qb_quic_openssl_side("${_qb_ssl_major}" "${OPENSSL_SSL_LIBRARY}" "${_qb_crypto_major}"
                              "${OPENSSL_CRYPTO_LIBRARY}" _qb_own_ssl)
        if(_qb_ssl_static)
            string(APPEND _qb_own_ssl " statically")
            string(APPEND _qb_quic_remedy ", shared (OPENSSL_USE_STATIC_LIBS off),")
        endif()
        string(CONCAT _qb_quic_pair
               "libngtcp2 (${NGTCP2_CRYPTO_OSSL_LIBRARY}) links ${_qb_ngtcp2_ssl} and qb links ${_qb_own_ssl} "
               "(headers ${OPENSSL_VERSION} in ${OPENSSL_INCLUDE_DIR})")
        string(CONCAT _qb_quic_fix
               "qb against ${_qb_quic_remedy} in a new build directory (an existing one keeps OPENSSL_INCLUDE_DIR, "
               "OPENSSL_SSL_LIBRARY and OPENSSL_CRYPTO_LIBRARY cached: remove all three, or configure with --fresh)")
    endif()
    if(_qb_quic_conflict STREQUAL "COPIES")
        string(CONCAT _qb_quic_copies
               "${_qb_quic_pair}: two copies of OpenSSL ${_qb_ssl_major} in one process. QUIC stays enabled -- one "
               "release in two copies was measured to work, on macOS -- but OpenSSL's ABI promise covers its "
               "public API, not an object one copy hands the other: to load one, build ${_qb_quic_fix}")
        qb_warning_message("${_qb_quic_copies}")
    endif()
    if(_qb_quic_conflict STREQUAL "MAJORS")
        string(CONCAT _qb_quic_majors
               "${_qb_quic_pair}: two OpenSSL majors in one process crash at the first QUIC handshake - QUIC "
               "support disabled. Build ${_qb_quic_fix}, or set QB_WITH_QUIC=OFF")
        qb_feature_degraded("${_qb_quic_majors}")
        set(QB_HAS_QUIC FALSE)
        set(_QB_HAD_QUIC FALSE CACHE INTERNAL "")
    elseif(Ngtcp2_FOUND)
        qb_status_message("Found libngtcp2 QUIC transport stack")
        list(APPEND QB_EXTERNAL_LIBRARIES
            Ngtcp2::ngtcp2
            Ngtcp2::crypto_ossl
        )
        set(QB_HAS_QUIC TRUE)
        set(_QB_HAD_QUIC TRUE CACHE INTERNAL "")
    elseif(Ngtcp2_VANISHED AND (NOT DEFINED CACHE{_QB_HAD_QUIC} OR "$CACHE{_QB_HAD_QUIC}"))
        # Gone from a build whose QUIC linked it (Huly QB-1192): one QUIC was off for -- two OpenSSL majors -- loses
        # nothing, and stops nothing. A build directory QB-1185's code configured recorded no _QB_HAD_QUIC: it stops.
        string(REPLACE ";" ", " _qb_quic_gone "${Ngtcp2_VANISHED}")
        string(CONCAT _qb_quic_gone
               "the libngtcp2 this build directory found is gone and was found nowhere else -- ${_qb_quic_gone}: "
               "install it, point CMAKE_PREFIX_PATH at one, or configure with -DQB_WITH_QUIC=OFF")
        qb_error_message("${_qb_quic_gone}")
    elseif(_qb_quic_required)
        qb_feature_degraded("libngtcp2 not found - QUIC support disabled")
        set(QB_HAS_QUIC FALSE)
        set(_QB_HAD_QUIC FALSE CACHE INTERNAL "")
    else()
        qb_status_message("libngtcp2 not found - QUIC transport auto-disabled")
        set(QB_HAS_QUIC FALSE)
        set(_QB_HAD_QUIC FALSE CACHE INTERNAL "")
    endif()
endmacro()
