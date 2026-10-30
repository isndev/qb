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
# qb Framework - the system libraries a build directory found, searched again when they are gone
#
# A package manager's update can remove the version a build directory found: Homebrew's search caches versioned
# Cellar paths (pkg-config gives them), and its cleanup removes the old keg. Kept in the cache, such a path makes the
# next build stop for good -- Ninja "missing and no known rule to make it", Make "No rule to make target" -- and a
# reconfigure keeps it too (a find_* call never searches a cached path again) and fails at generate. The helpers below
# search again what the search itself found, and stop the configure when it is found nowhere else (Huly QB-1185 for
# OpenSSL in qbOpenSSL.cmake, QB-1192 for the rest); _qb_find_argon2(), _qb_find_zlib(), _qb_find_zstd() and
# _qb_find_brotli() are qbDependencies.cmake's find_package() calls for the other four. A file of its own so that a
# fixture drives them with planted libraries (qb-io-test-unit-found-libraries); macros so that what they find stays in
# the caller's scope. libngtcp2's search does the same inside FindNgtcp2.cmake, which an installed qb ships.
# -----------------------------------------------------------------------------

# _qb_forget_vanished(<out_var> <entry>...) before a search, _qb_record_found(<entry>...) after it. A path the search
# itself found -- recorded then -- whose file is gone now (an update removed the version this build directory found, as
# a Homebrew cleanup removes the old keg) is searched again: kept, the build would link a file that no longer exists,
# and the configure its removal re-runs would read nothing. <out_var> gets "<entry> (<path>)" per path searched again,
# for the caller to stop the configure when that search finds nothing: a library this build directory had is gone,
# which a build must not answer by silently building without it. The stop holds: the failed configure leaves the entry
# NOTFOUND beside its record, which counts as gone again, until a search finds it, the feature is turned off or the
# build directory starts afresh. A path the search did not produce -- set by hand or by a parent project (a superbuild
# names a library it builds later), or cached before this build directory recorded its searches -- is kept as given.
# What it says, it says at once -- unless the caller set _qb_forget_quiet, not knowing yet whether a search will look:
# then _qb_forget_tell(<out_var>) says it, once one did.
#
# An <entry> is a cache entry's name, or <name>/<file> for a directory the search found because it held <file> (an
# include directory and the header its find_path() looked for): that path is gone when <file> is, though the directory
# stays -- a shared /usr/include from which a package's removal took its headers (Huly QB-1192). Where find_path() found
# <file> in an Apple framework, the path is the framework, and <file> is under its Headers (_qb_held_file()).
function(_qb_found_entry spec name_var probe_var)
    string(FIND "${spec}" "/" _slash)
    if(_slash EQUAL -1)
        set(${name_var} "${spec}" PARENT_SCOPE)
        set(${probe_var} "" PARENT_SCOPE)
    else()
        string(SUBSTRING "${spec}" 0 ${_slash} _name)
        math(EXPR _slash "${_slash} + 1")
        string(SUBSTRING "${spec}" ${_slash} -1 _probe)
        set(${name_var} "${_name}" PARENT_SCOPE)
        set(${probe_var} "/${_probe}" PARENT_SCOPE)
    endif()
endfunction()
# _qb_held_file(<path> <probe> <out_var>) -- the file the path an entry holds is checked by: <path><probe>, or, <path>
# being the framework find_path() returns for <dir>/<file> (<dir>.framework, any case), <path>/Headers/<file>.
function(_qb_held_file path probe out_var)
    set(_file "${path}${probe}")
    if("${probe}" MATCHES "^/([^/]+)/(.+)$")
        set(_rest "${CMAKE_MATCH_2}")
        string(TOLOWER "${CMAKE_MATCH_1}.framework" _framework)
        get_filename_component(_last "${path}" NAME)
        string(TOLOWER "${_last}" _last)
        if(_last STREQUAL _framework)
            set(_file "${path}/Headers/${_rest}")
        endif()
    endif()
    set(${out_var} "${_file}" PARENT_SCOPE)
endfunction()
# in _qb_forget_vanished(): the text <var> holds said now, or kept for _qb_forget_tell() -- a variable's name, not its
# text: a macro reads escapes in its arguments again, and a Windows path's backslashes would stop the configure
macro(_qb_forget_say var)
    if(_qb_forget_quiet)
        math(EXPR _said "${_said} + 1")
        set(_said_${_said} "${${var}}")
    else()
        qb_status_message("${${var}}")
    endif()
endmacro()
function(_qb_forget_vanished out_var)
    set(_forgotten "")
    set(_said 0)
    foreach(_spec IN LISTS ARGN)
        _qb_found_entry("${_spec}" _entry _probe)
        set(_path "$CACHE{${_entry}}")
        set(_found "$CACHE{_QB_FOUND_${_entry}}")
        set(_lost "no longer exists")
        set(_kept "does not exist")
        if(NOT "${_probe}" STREQUAL "" AND IS_DIRECTORY "${_path}")
            string(SUBSTRING "${_probe}" 1 -1 _file)
            set(_lost "no longer holds ${_file}")
            set(_kept "holds no ${_file}")
        endif()
        _qb_held_file("${_found}" "${_probe}" _found_file)
        _qb_held_file("${_path}" "${_probe}" _path_file)
        if(("${_path}" STREQUAL "" OR "${_path}" MATCHES "(^|-)NOTFOUND$") AND IS_ABSOLUTE "${_found}"
           AND NOT EXISTS "${_found_file}")
            list(APPEND _forgotten "${_entry} (${_found})") # searched again by a configure that stopped: still gone
        elseif(IS_ABSOLUTE "${_path}" AND NOT EXISTS "${_path_file}")
            if("${_path}" STREQUAL "${_found}")
                set(_text "${_entry} (${_path}) ${_lost}: searched again")
                _qb_forget_say(_text)
                unset(${_entry} CACHE)
                list(APPEND _forgotten "${_entry} (${_path})")
            elseif(NOT "${_qb_kept_said_${_entry}}" STREQUAL "${_path}") # once per configure: two searches may ask
                string(CONCAT _kept "${_entry} (${_path}) ${_kept}; this build directory's search did not find it (set "
                                    "by hand, by a parent project, or before qb recorded its searches): kept as given "
                                    "-- set it again, or configure with --fresh to search for it")
                _qb_forget_say(_kept)
                set(_qb_kept_said_${_entry} "${_path}" PARENT_SCOPE)
            endif()
        endif()
        set(_qb_before_${_entry} "$CACHE{${_entry}}" PARENT_SCOPE)
    endforeach()
    set(${out_var} "${_forgotten}" PARENT_SCOPE)
    set(${out_var}_said ${_said} PARENT_SCOPE)
    if(_said GREATER 0)
        foreach(_index RANGE 1 ${_said})
            set(${out_var}_said_${_index} "${_said_${_index}}" PARENT_SCOPE)
        endforeach()
    endif()
endfunction()
# _qb_forget_tell(<out_var>) -- what a quiet _qb_forget_vanished(<out_var> ...) kept, said.
macro(_qb_forget_tell out_var)
    if(${out_var}_said GREATER 0)
        foreach(_qb_tell RANGE 1 ${${out_var}_said})
            qb_status_message("${${out_var}_said_${_qb_tell}}")
        endforeach()
    endif()
endmacro()
function(_qb_record_found)
    foreach(_spec IN LISTS ARGN)
        _qb_found_entry("${_spec}" _entry _probe)
        set(_path "$CACHE{${_entry}}")
        _qb_held_file("${_path}" "${_probe}" _path_file)
        if(NOT "${_path}" STREQUAL "${_qb_before_${_entry}}" AND IS_ABSOLUTE "${_path}" AND EXISTS "${_path_file}")
            set(_QB_FOUND_${_entry} "${_path}" CACHE INTERNAL "")
        endif()
    endforeach()
endfunction()

# _qb_stop_if_gone(<forgotten> <what> <remedy>) -- after the search _qb_forget_vanished() asked for: what was searched
# again and is not found now -- not one found again beside another missing -- stops the configure, naming each entry and
# the path it had, then <remedy>. Whether to ask is the caller's: OpenSSL's search asks on the entries alone, whatever
# OpenSSL_FOUND says (FindOpenSSL reports OpenSSL found without libssl); _qb_find_again() when its package is lost.
function(_qb_stop_if_gone forgotten what remedy)
    set(_gone "")
    foreach(_entry IN LISTS forgotten)
        string(REGEX MATCH "^[A-Z0-9_]+" _name "${_entry}")
        if(NOT (IS_ABSOLUTE "$CACHE{${_name}}" AND EXISTS "$CACHE{${_name}}"))
            list(APPEND _gone "${_entry}")
        endif()
    endforeach()
    if(_gone)
        string(REPLACE ";" ", " _gone "${_gone}")
        # One string: qb_error_message() interpolates ${ARGN}, and several arguments come back joined by ";".
        string(CONCAT _gone "the ${what} this build directory found is gone and was found nowhere else -- ${_gone}: "
                            "${remedy}")
        qb_error_message("${_gone}")
    endif()
endfunction()

# _qb_find_again(<Package> <what> <remedy> ENTRIES <entry>... [PKG_CONFIG <prefix>...] [ARGS <argument>...]
#                [FALLBACK <bool>]) --
# find_package(<Package> <argument>...) with the entries its search caches searched again when that search found them
# and they are gone, pkg-config asked again with them (FindPkgConfig keeps its answer, which names the removed
# directory, until __pkg_config_checked_<prefix> is gone). The configure stops when <Package> is not found now, this
# build directory having found it before (_QB_HAD_<Package>), and an entry searched again is found nowhere else: a
# package it never had -- a stray header without its library -- stops nothing, nor does one found again whatever the
# entries say (FindZLIB takes a ZLIB_LIBRARY set by hand without searching ZLIB_LIBRARY_RELEASE again; a debug zlib gone
# beside the release one loses nothing), nor one the caller provides otherwise (FALLBACK true: zlib built from source,
# or an existing ZLIB::ZLIB: a parent project's, or the one FindOpenSSL creates for a zlib it found), nor one turned off
# (CMAKE_DISABLE_FIND_PACKAGE_<Package>: find_package() then searches nothing, and the configure goes on as for a
# package it never found). Each file the search produced -- a library, and for a <name>/<file> entry that file in the
# directory -- becomes an input of the configure (CMAKE_CONFIGURE_DEPENDS), so that `cmake --build` re-runs it when an
# update removes one; not a path set by hand or by a parent project, which is never searched again (a superbuild's own
# library would re-run the configure at each of its rebuilds), only existing files, and never a directory (its time
# moves with every entry added). That holds for a file the configure only searched for; one it READS is an input
# whoever set it, at that price: OpenSSL's libssl, libcrypto and opensslv.h, and libngtcp2's crypto helper
# (qbOpenSSL.cmake), whose majors decide whether a process loads two OpenSSL majors -- rebuilt onto another major, a
# superbuild's own must be read again. A pure move -- a link the search found pointing at a new version -- keeps a valid
# path, and the configure re-run it may cause changes nothing.
macro(_qb_find_again package what remedy)
    cmake_parse_arguments(_qb_again "" "FALLBACK" "ENTRIES;PKG_CONFIG;ARGS" ${ARGN})
    if(CMAKE_DISABLE_FIND_PACKAGE_${package})
        find_package(${package} ${_qb_again_ARGS}) # turned off: nothing searched, so nothing searched again or said
    else()
        _qb_find_again_searched(${package} "${what}" "${remedy}")
    endif()
endmacro()
macro(_qb_find_again_searched package what remedy)
    set(_qb_searched_${package} TRUE) # searched through its wrapper (qb-io-test-unit-found-libraries reads it)
    _qb_forget_vanished(_qb_again_forgot ${_qb_again_ENTRIES})
    if(_qb_again_forgot)
        foreach(_qb_again_prefix IN LISTS _qb_again_PKG_CONFIG)
            unset(__pkg_config_checked_${_qb_again_prefix} CACHE)
        endforeach()
    endif()
    find_package(${package} ${_qb_again_ARGS})
    _qb_record_found(${_qb_again_ENTRIES}) # each entry found, the package found or not: a partial search counts
    if(${package}_FOUND)
        set(_QB_HAD_${package} TRUE CACHE INTERNAL "")
    elseif("$CACHE{_QB_HAD_${package}}" AND NOT _qb_again_FALLBACK)
        _qb_stop_if_gone("${_qb_again_forgot}" "${what}" "${remedy}")
    endif()
    _qb_found_inputs(${_qb_again_ENTRIES})
endmacro()

# _qb_found_inputs(<entry>...) -- each file a search produced -- the entry's value is its record -- an input of the
# configure (CMAKE_CONFIGURE_DEPENDS): existing files only, never a directory.
function(_qb_found_inputs)
    set(_inputs "")
    foreach(_spec IN LISTS ARGN)
        _qb_found_entry("${_spec}" _entry _probe)
        set(_path "$CACHE{${_entry}}")
        _qb_held_file("${_path}" "${_probe}" _file)
        if("${_path}" STREQUAL "$CACHE{_QB_FOUND_${_entry}}" AND IS_ABSOLUTE "${_path}" AND EXISTS "${_file}"
           AND NOT IS_DIRECTORY "${_file}")
            list(APPEND _inputs "${_file}")
        endif()
    endforeach()
    if(_inputs)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
    endif()
endfunction()

# qbDependencies.cmake's searches for Argon2 (with OpenSSL), zlib (compression), zstd and brotli (the codecs), each with
# the entries its module caches. Argon2 has no switch of its own: a build directory that had it turns its search off
# (-DCMAKE_DISABLE_FIND_PACKAGE_Argon2=ON) to build with qb's fallback -- QB_HAS_ARGON2 is a public compile
# definition, so losing it silently would change qb's surface. A zlib lost is no loss when qbDependencies.cmake builds
# one from source (QB_DEPS_FETCH_FALLBACK, in a build that installs nothing: an installable qb refuses a zlib built
# from source) or a parent project provides ZLIB::ZLIB.
macro(_qb_find_argon2)
    string(CONCAT _qb_argon2_remedy "install it, point CMAKE_PREFIX_PATH at one, or configure with "
                                    "-DCMAKE_DISABLE_FIND_PACKAGE_Argon2=ON to build without it")
    _qb_find_again(Argon2 Argon2 "${_qb_argon2_remedy}"
                   ENTRIES ARGON2_INCLUDE_DIR/argon2.h ARGON2_LIBRARY PKG_CONFIG PC_ARGON2 ARGS QUIET)
endmacro()
macro(_qb_find_zlib)
    string(CONCAT _qb_zlib_remedy "install one, point CMAKE_PREFIX_PATH (or ZLIB_ROOT) at one, let qb build one from "
                                  "source (QB_DEPS_FETCH_FALLBACK=ON, QB_INSTALL=OFF), or set QB_WITH_COMPRESSION=OFF "
                                  "(and QB_WITH_ZSTD, QB_WITH_BROTLI: the codecs need it)")
    if(QB_WITH_SSL AND _qb_openssl_links_zlib) # _qb_find_openssl()'s: compression off, OpenSSL would still link it
        string(APPEND _qb_zlib_remedy " with QB_WITH_SSL=OFF: the OpenSSL found links zlib too")
    endif()
    set(_qb_zlib_fallback FALSE)
    if((QB_DEPS_FETCH_FALLBACK AND NOT QB_INSTALL) OR TARGET ZLIB::ZLIB)
        set(_qb_zlib_fallback TRUE)
    endif()
    if(NOT _qb_openssl_links_zlib AND NOT CMAKE_DISABLE_FIND_PACKAGE_ZLIB)
        _qb_forget_tell(_qb_openssl_zlib_forgot) # what _qb_find_openssl() searched again quietly, said now
    endif()
    _qb_find_again(ZLIB zlib "${_qb_zlib_remedy}"
                   ENTRIES ZLIB_INCLUDE_DIR/zlib.h ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG ARGS QUIET
                   FALLBACK ${_qb_zlib_fallback})
    # Lost beside a parent project's ZLIB::ZLIB: qb links that one, as it did while the lost path was still cached.
    if(NOT ZLIB_FOUND AND TARGET ZLIB::ZLIB AND NOT CMAKE_DISABLE_FIND_PACKAGE_ZLIB AND _qb_again_forgot)
        set(ZLIB_FOUND TRUE)
        set(ZLIB_VERSION_STRING "a parent project's ZLIB::ZLIB")
    endif()
endmacro()
macro(_qb_find_zstd)
    _qb_find_again(Zstd zstd "install one, point CMAKE_PREFIX_PATH at one, or configure with -DQB_WITH_ZSTD=OFF"
                   ENTRIES ZSTD_INCLUDE_DIR/zstd.h ZSTD_LIBRARY PKG_CONFIG PC_ZSTD)
endmacro()
macro(_qb_find_brotli)
    _qb_find_again(Brotli brotli "install one, point CMAKE_PREFIX_PATH at one, or configure with -DQB_WITH_BROTLI=OFF"
                   ENTRIES BROTLI_INCLUDE_DIR/brotli/encode.h BROTLI_ENCODER_LIBRARY BROTLI_DECODER_LIBRARY
                           BROTLI_COMMON_LIBRARY
                   PKG_CONFIG PC_BROTLIENC PC_BROTLIDEC PC_BROTLICOMMON)
endmacro()
