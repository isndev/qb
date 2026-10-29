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
# _qb_find_openssl() finds the OpenSSL qb links, searching again a path its own search found that is gone, and
# _qb_decide_quic(), once find_package(Ngtcp2) has run, decides whether the OpenSSL libngtcp2_crypto_ossl links and
# qb's can share one process, and so whether QUIC is enabled. A file of its own so that a fixture drives both with
# planted libraries (qb-io-test-unit-quic-openssl-verdict, Huly QB-1185); macros so that what they find and read stays
# in the caller's scope -- qbDependencies.cmake's, where tests/io/unit/CMakeLists.txt hands it to
# qb-io-test-unit-ngtcp2-openssl-listing.
# -----------------------------------------------------------------------------

# _qb_forget_vanished(<out_var> <cache entry>...) before a search, _qb_record_found(<cache entry>...) after it. A path
# the search itself found -- recorded then -- whose file is gone now (an update removed the version this build directory
# found, as a Homebrew cleanup removes the old keg) is searched again: kept, the build would link a file that no longer
# exists, and the configure its removal re-runs would read nothing. <out_var> gets "<entry> (<path>)" per path searched
# again, for the caller to stop the configure when that search finds nothing: a library this build directory had is
# gone, which a build must not answer by silently building without it. The stop holds: the failed configure leaves the
# entry NOTFOUND beside its record, which counts as gone again, until a search finds it, the feature is turned off or
# the build directory starts afresh. A path the search did not produce -- set by hand or by a parent project (a
# superbuild names a library it builds later), or cached before this build directory recorded its searches -- is kept
# as given.
function(_qb_forget_vanished out_var)
    set(_forgotten "")
    foreach(_entry IN LISTS ARGN)
        set(_path "$CACHE{${_entry}}")
        set(_found "$CACHE{_QB_FOUND_${_entry}}")
        if(("${_path}" STREQUAL "" OR "${_path}" MATCHES "(^|-)NOTFOUND$") AND IS_ABSOLUTE "${_found}"
           AND NOT EXISTS "${_found}")
            list(APPEND _forgotten "${_entry} (${_found})") # searched again by a configure that stopped: still gone
        elseif(IS_ABSOLUTE "${_path}" AND NOT EXISTS "${_path}")
            if("${_path}" STREQUAL "${_found}")
                qb_status_message("${_entry} (${_path}) no longer exists: searched again")
                unset(${_entry} CACHE)
                list(APPEND _forgotten "${_entry} (${_path})")
            else()
                string(CONCAT _kept "${_entry} (${_path}) does not exist; this build directory's search did not find it "
                                    "(set by hand, by a parent project, or before qb recorded its searches): kept as given")
                qb_status_message("${_kept}")
            endif()
        endif()
        set(_qb_before_${_entry} "$CACHE{${_entry}}" PARENT_SCOPE)
    endforeach()
    set(${out_var} "${_forgotten}" PARENT_SCOPE)
endfunction()
function(_qb_record_found)
    foreach(_entry IN LISTS ARGN)
        set(_path "$CACHE{${_entry}}")
        if(NOT "${_path}" STREQUAL "${_qb_before_${_entry}}" AND IS_ABSOLUTE "${_path}" AND EXISTS "${_path}")
            set(_QB_FOUND_${_entry} "${_path}" CACHE INTERNAL "")
        endif()
    endforeach()
endfunction()

# _qb_find_openssl() -- qbDependencies.cmake's find_package(OpenSSL), with the three entries it caches searched again
# when its own search found them and they are gone (above), and stopping when that search finds nothing. FindOpenSSL
# takes its hints from pkg-config, whose answer FindPkgConfig keeps cached until __pkg_config_checked__OPENSSL is gone:
# it is cleared with them, or the hints would still name the removed directory. The files found -- libssl and libcrypto
# as found and as resolved, and opensslv.h, whose version the configure reads -- become inputs of the configure, so
# that `cmake --build` re-runs it after an OpenSSL update, QUIC or not. Not on Windows: MSVC's FindOpenSSL caches its
# libraries under other names, and nothing is read there.
macro(_qb_find_openssl)
    set(_qb_openssl_entries OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
    set(_qb_openssl_forgot "")
    if(NOT WIN32)
        _qb_forget_vanished(_qb_openssl_forgot ${_qb_openssl_entries})
        if(_qb_openssl_forgot)
            unset(__pkg_config_checked__OPENSSL CACHE)
        endif()
    endif()
    find_package(OpenSSL QUIET)
    if(NOT WIN32)
        _qb_record_found(${_qb_openssl_entries}) # each entry found, OpenSSL found or not: a partial search counts
    endif()
    # What was searched again and is not found now -- not one found again beside another missing -- stops the configure,
    # whatever OpenSSL_FOUND says: FindOpenSSL requires libcrypto and the headers only, and finds OpenSSL without libssl.
    set(_qb_openssl_gone "")
    foreach(_qb_openssl_entry IN LISTS _qb_openssl_forgot)
        string(REGEX MATCH "^[A-Z0-9_]+" _qb_openssl_name "${_qb_openssl_entry}")
        if(NOT (IS_ABSOLUTE "$CACHE{${_qb_openssl_name}}" AND EXISTS "$CACHE{${_qb_openssl_name}}"))
            list(APPEND _qb_openssl_gone "${_qb_openssl_entry}")
        endif()
    endforeach()
    if(_qb_openssl_gone)
        string(REPLACE ";" ", " _qb_openssl_gone "${_qb_openssl_gone}")
        string(CONCAT _qb_openssl_gone
               "the OpenSSL this build directory found is gone and was found nowhere else -- ${_qb_openssl_gone}: "
               "install one, point OPENSSL_ROOT_DIR at one, or configure with -DQB_WITH_SSL=OFF")
        qb_error_message("${_qb_openssl_gone}")
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
# included, as a half-cleared cache leaves qb's (QB-1185); two copies of one major -- a static libssl in qb, or two
# prefixes where Mach-O names them -- are reported and kept. qb's own libssl and libcrypto are read the same way (their
# headers' major when nothing reads them), unless a parent's FindNgtcp2 module won.
#
# The files read become inputs of the configure (CMAKE_CONFIGURE_DEPENDS): `cmake --build` re-runs it when a package
# update rewrites one, removes one, or moves the link that names one (QB-1185), instead of keeping a verdict about
# libraries that are no longer the ones a binary loads -- each library as found, the file it resolves to, on Mach-O the
# install name dyld loads and its file, and qb's opensslv.h, whose major stands in for a library nothing reads. Only
# files that exist: a missing input re-runs every build.
macro(_qb_decide_quic)
    set(_qb_quic_conflict "")
    set(_qb_quic_inputs "")
    if(Ngtcp2_FOUND AND COMMAND _ngtcp2_linked_openssl AND COMMAND _ngtcp2_openssl_conflict
       AND COMMAND _ngtcp2_loaded_file)
        set(_qb_quic_compared TRUE)
        _ngtcp2_linked_openssl("${OPENSSL_SSL_LIBRARY}" _qb_ssl_major _qb_ssl_path _qb_ssl_crypto_major
                               _qb_ssl_crypto_path)
        _ngtcp2_linked_openssl("${OPENSSL_CRYPTO_LIBRARY}" _qb_crypto_ssl_major _qb_crypto_ssl_path _qb_crypto_major
                               _qb_crypto_path)
        string(REGEX MATCH "^[0-9]+" _qb_headers_major "${OPENSSL_VERSION}")
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
               "support disabled.")
        if(NOT "${_qb_ssl_major}" STREQUAL "${_qb_crypto_major}")
            string(APPEND _qb_quic_majors " qb's own libssl and libcrypto are two majors: its TLS loads both as well, "
                                          "QUIC or not (Huly QB-1190).")
        endif()
        string(APPEND _qb_quic_majors " Build ${_qb_quic_fix}, or set QB_WITH_QUIC=OFF")
        qb_feature_degraded("${_qb_quic_majors}")
        set(QB_HAS_QUIC FALSE)
    elseif(Ngtcp2_FOUND)
        qb_status_message("Found libngtcp2 QUIC transport stack")
        list(APPEND QB_EXTERNAL_LIBRARIES
            Ngtcp2::ngtcp2
            Ngtcp2::crypto_ossl
        )
        set(QB_HAS_QUIC TRUE)
    elseif(Ngtcp2_VANISHED)
        string(REPLACE ";" ", " _qb_quic_gone "${Ngtcp2_VANISHED}")
        string(CONCAT _qb_quic_gone
               "the libngtcp2 this build directory found is gone and was found nowhere else -- ${_qb_quic_gone}: "
               "install it, point CMAKE_PREFIX_PATH at one, or configure with -DQB_WITH_QUIC=OFF")
        qb_error_message("${_qb_quic_gone}")
    elseif(_qb_quic_required)
        qb_feature_degraded("libngtcp2 not found - QUIC support disabled")
        set(QB_HAS_QUIC FALSE)
    else()
        qb_status_message("libngtcp2 not found - QUIC transport auto-disabled")
        set(QB_HAS_QUIC FALSE)
    endif()
endmacro()
