#
# Find libngtcp2 and its OpenSSL crypto helper.
#
# Result variables:
#   Ngtcp2_FOUND
#   Ngtcp2_VERSION
#   Ngtcp2_OPENSSL_MAJOR    the OpenSSL major libngtcp2_crypto_ossl links (3, 4, ...), read from the shared
#                           library's own dependency list; empty for a static archive, on Windows, or when no
#                           tool can read it (Huly QB-1179)
#   Ngtcp2_OPENSSL_LIBRARY  the libssl that list names, where it gives a path (Mach-O); empty otherwise
#   Ngtcp2_OPENSSL_CRYPTO_MAJOR, Ngtcp2_OPENSSL_CRYPTO_LIBRARY
#                           the same two for the libcrypto it links (Huly QB-1185)
#   Ngtcp2_VANISHED         when libngtcp2 is not found: "<entry> (<path>)" per path an earlier search of this build
#                           directory found that is gone -- a library it had, not one never installed
#
# Imported targets:
#   Ngtcp2::ngtcp2
#   Ngtcp2::crypto_ossl
#
# A path the search found, which existed then and is gone now -- a package manager removed the version this build
# directory found, as a Homebrew cleanup removes the old keg -- is searched again (Huly QB-1185); a path set by hand or
# by a parent project is kept. Both are said, QUIET or not: they change what the cache holds, or keep a stale path.
#
# With NGTCP2_FIND_FUNCTIONS_ONLY set, including this file defines its functions and finds nothing: the dependency-list
# reader's test (qb-io-test-unit-ngtcp2-openssl-listing) reads it that way.
#

# The OpenSSL library lib<name> -- libssl or libcrypto -- a dependency listing names: `otool -L` (Mach-O), `readelf -d`
# or `objdump -p` (ELF), of a library that links it or of that library itself. The major its first lib<name> entry
# carries in its file name (libssl.4.dylib, libcrypto.so.3; libssl.so.1.1 is 1) goes into <major_var>, and the path when
# the listing gives one (Mach-O) into <path_var>; both empty when it names no versioned lib<name>. Read line by line,
# and not as a CMake list, where an unbalanced '[' in a path would join lines: an ELF listing's NEEDED and SONAME
# entries only, a Mach-O one's entries without their "(compatibility version ...)", so neither a heading (it names the
# file read) nor a path with spaces misleads it.
function(_ngtcp2_openssl_in_listing listing name major_var path_var)
    set(_major "")
    set(_path "")
    set(_rest "${listing}")
    while(NOT "${_rest}" STREQUAL "")
        string(FIND "${_rest}" "\n" _end)
        if(_end EQUAL -1)
            set(_line "${_rest}")
            set(_rest "")
        else()
            string(SUBSTRING "${_rest}" 0 ${_end} _line)
            math(EXPR _end "${_end} + 1")
            string(SUBSTRING "${_rest}" ${_end} -1 _rest)
        endif()
        if("${_line}" MATCHES "(NEEDED|SONAME).*[[ \t/]lib${name}\\.so\\.([0-9]+)")
            set(_major "${CMAKE_MATCH_2}")
            break()
        endif()
        string(STRIP "${_line}" _entry)
        string(REGEX REPLACE " \\(compatibility version.*$" "" _entry "${_entry}")
        if("${_entry}" MATCHES "(^|/)lib${name}\\.([0-9]+)(\\.[0-9]+)*\\.dylib$")
            set(_major "${CMAKE_MATCH_2}")
            set(_path "${_entry}")
            break()
        endif()
    endwhile()
    set(${major_var} "${_major}" PARENT_SCOPE)
    set(${path_var} "${_path}" PARENT_SCOPE)
endfunction()

# The file the dynamic loader opens for the shared library <library>, into <out_var>. A Mach-O binary records the install
# name of the library it linked (`otool -D`) and dyld loads that file: Homebrew's search finds a Cellar path whose install
# name is the opt/ link, so after an upgrade that keeps the old keg the two are different files, and the install name is
# the one that runs. Elsewhere, and when the install name is not an absolute path to a file (@rpath), the library itself.
function(_ngtcp2_loaded_file library out_var)
    set(_loaded "${library}")
    if(APPLE AND NOT WIN32 AND EXISTS "${library}" AND NOT "${library}" MATCHES "\\.a$")
        find_program(NGTCP2_OTOOL_EXECUTABLE otool)
        mark_as_advanced(NGTCP2_OTOOL_EXECUTABLE)
        if(NGTCP2_OTOOL_EXECUTABLE)
            execute_process(COMMAND "${NGTCP2_OTOOL_EXECUTABLE}" -D "${library}"
                            OUTPUT_VARIABLE _id ERROR_QUIET RESULT_VARIABLE _rc)
            # `otool -D` prints the file it read, then the install name on a line of its own when there is one.
            if(_rc EQUAL 0 AND "${_id}" MATCHES "\n(/[^\n]+)\n*$")
                if(EXISTS "${CMAKE_MATCH_1}") # its own if(): an argument is expanded before MATCHES sets it
                    set(_loaded "${CMAKE_MATCH_1}")
                endif()
            endif()
        endif()
    endif()
    set(${out_var} "${_loaded}" PARENT_SCOPE)
endfunction()

# The OpenSSL the shared library <library> links -- or is, for a libssl or a libcrypto: the majors and paths of the
# first libssl and libcrypto entries of the dependency list of the file the loader opens for it (_ngtcp2_loaded_file),
# read with the platform's tool (`otool -L`; `readelf -d`, then `objdump -p` when readelf is absent or fails). A static
# archive has no such list, Windows is not read (a vcpkg triplet carries one OpenSSL; MSYS2 or an installer's DLLs
# beside it are not checked), and a missing or failing tool yields nothing: no answer, never a guess.
function(_ngtcp2_linked_openssl library ssl_major_var ssl_path_var crypto_major_var crypto_path_var)
    set(_listing "")
    set(_rc 1)
    if(WIN32 OR "${library}" MATCHES "\\.(a|lib)$" OR NOT EXISTS "${library}")
        # nothing to read
    elseif(APPLE)
        _ngtcp2_loaded_file("${library}" _loaded)
        find_program(NGTCP2_OTOOL_EXECUTABLE otool)
        mark_as_advanced(NGTCP2_OTOOL_EXECUTABLE)
        if(NGTCP2_OTOOL_EXECUTABLE)
            execute_process(COMMAND "${NGTCP2_OTOOL_EXECUTABLE}" -L "${_loaded}"
                            OUTPUT_VARIABLE _listing ERROR_QUIET RESULT_VARIABLE _rc)
        endif()
    else()
        if(CMAKE_READELF)
            execute_process(COMMAND "${CMAKE_READELF}" -d "${library}" OUTPUT_VARIABLE _listing ERROR_QUIET RESULT_VARIABLE _rc)
        endif()
        if(NOT _rc EQUAL 0 AND CMAKE_OBJDUMP)
            execute_process(COMMAND "${CMAKE_OBJDUMP}" -p "${library}" OUTPUT_VARIABLE _listing ERROR_QUIET RESULT_VARIABLE _rc)
        endif()
    endif()
    if(NOT _rc EQUAL 0)
        set(_listing "")
    endif()
    _ngtcp2_openssl_in_listing("${_listing}" ssl _ssl_major _ssl_path)
    _ngtcp2_openssl_in_listing("${_listing}" crypto _crypto_major _crypto_path)
    set(${ssl_major_var} "${_ssl_major}" PARENT_SCOPE)
    set(${ssl_path_var} "${_ssl_path}" PARENT_SCOPE)
    set(${crypto_major_var} "${_crypto_major}" PARENT_SCOPE)
    set(${crypto_path_var} "${_crypto_path}" PARENT_SCOPE)
endfunction()

# Whether <first> and <second> name two files, into <out_var>: TRUE when both are absolute paths (an @rpath name resolves
# only at run time) that do not resolve to one file.
function(_ngtcp2_two_files first second out_var)
    set(_two FALSE)
    if("${first}" MATCHES "^/" AND "${second}" MATCHES "^/")
        set(_first_real "${first}")
        set(_second_real "${second}")
        if(EXISTS "${first}")
            file(REAL_PATH "${first}" _first_real)
        endif()
        if(EXISTS "${second}")
            file(REAL_PATH "${second}" _second_real)
        endif()
        if(NOT "${_first_real}" STREQUAL "${_second_real}")
            set(_two TRUE)
        endif()
    endif()
    set(${out_var} ${_two} PARENT_SCOPE)
endfunction()

# What libngtcp2's OpenSSL and qb's are to each other in one process, from the majors and paths of the libssl and the
# libcrypto read on each side, into <out_var>: MAJORS when the majors read are not all one -- two OpenSSL majors were
# measured to crash at the first QUIC handshake (Huly QB-1179), and a libcrypto left on another major than its libssl,
# as a half-cleared cache leaves qb's, is two majors too (QB-1185); COPIES for two copies of one major -- a static libssl
# in qb beside the shared one the helper loads, or two absolute paths that resolve to two files (Mach-O lists them);
# empty for one library, or when a side is unknown: no answer, never a guess.
function(_ngtcp2_openssl_conflict ngtcp2_ssl ngtcp2_ssl_path ngtcp2_crypto ngtcp2_crypto_path
                                  qb_ssl qb_ssl_path qb_crypto qb_crypto_path qb_static out_var)
    set(_verdict "")
    set(_majors "")
    foreach(_major IN ITEMS "${ngtcp2_ssl}" "${ngtcp2_crypto}" "${qb_ssl}" "${qb_crypto}")
        if(NOT "${_major}" STREQUAL "")
            list(APPEND _majors "${_major}")
        endif()
    endforeach()
    if(_majors)
        list(REMOVE_DUPLICATES _majors)
    endif()
    list(LENGTH _majors _count)
    if(_count GREATER 1)
        set(_verdict MAJORS)
    elseif(qb_static)
        if(NOT "${ngtcp2_ssl}" STREQUAL "")
            set(_verdict COPIES)
        endif()
    else()
        _ngtcp2_two_files("${ngtcp2_ssl_path}" "${qb_ssl_path}" _two_ssl)
        _ngtcp2_two_files("${ngtcp2_crypto_path}" "${qb_crypto_path}" _two_crypto)
        if(_two_ssl OR _two_crypto)
            set(_verdict COPIES)
        endif()
    endif()
    set(${out_var} "${_verdict}" PARENT_SCOPE)
endfunction()

if(NGTCP2_FIND_FUNCTIONS_ONLY)
    return()
endif()

# A path the search below found, which existed then and is gone now, is searched again: kept, the build would link a
# file that no longer exists, and the configure its removal re-runs (qb makes the libraries it reads inputs of the
# configure, cmake/qbOpenSSL.cmake) would read nothing; found nowhere else, libngtcp2 is gone. pkg-config's answer,
# which FindPkgConfig keeps cached until __pkg_config_checked_<prefix> is gone, is asked again with it. A path the
# search did not produce -- set by hand or by a parent project, as a superbuild names a library it builds, or cached
# before this module recorded its searches -- is never recorded, so kept, as before.
set(_ngtcp2_cache_entries NGTCP2_INCLUDE_DIR NGTCP2_LIBRARY NGTCP2_CRYPTO_OSSL_LIBRARY NGTCP2_CRYPTO_INCLUDE_DIR
                          NGTCP2_CRYPTO_OSSL_INCLUDE_DIR)
set(Ngtcp2_VANISHED "")
foreach(_ngtcp2_cached IN LISTS _ngtcp2_cache_entries)
    set(_ngtcp2_path "$CACHE{${_ngtcp2_cached}}")
    set(_ngtcp2_found "$CACHE{_NGTCP2_FOUND_${_ngtcp2_cached}}")
    if(("${_ngtcp2_path}" STREQUAL "" OR "${_ngtcp2_path}" MATCHES "(^|-)NOTFOUND$") AND IS_ABSOLUTE "${_ngtcp2_found}"
       AND NOT EXISTS "${_ngtcp2_found}")
        # searched again by an earlier configure, which found nothing: still gone, until a search finds it
        list(APPEND Ngtcp2_VANISHED "${_ngtcp2_cached} (${_ngtcp2_found})")
        unset(__pkg_config_checked_PC_NGTCP2 CACHE)
        unset(__pkg_config_checked_PC_NGTCP2_CRYPTO_OSSL CACHE)
    elseif(IS_ABSOLUTE "${_ngtcp2_path}" AND NOT EXISTS "${_ngtcp2_path}")
        if("${_ngtcp2_path}" STREQUAL "${_ngtcp2_found}")
            message(STATUS "${_ngtcp2_cached} (${_ngtcp2_path}) no longer exists: searched again")
            list(APPEND Ngtcp2_VANISHED "${_ngtcp2_cached} (${_ngtcp2_path})")
            unset(${_ngtcp2_cached} CACHE)
            unset(__pkg_config_checked_PC_NGTCP2 CACHE)
            unset(__pkg_config_checked_PC_NGTCP2_CRYPTO_OSSL CACHE)
        else()
            message(STATUS "${_ngtcp2_cached} (${_ngtcp2_path}) does not exist; this build directory's search did not "
                           "find it (set by hand, by a parent project, or before this module recorded its searches): "
                           "kept as given")
        endif()
    endif()
    set(_ngtcp2_before_${_ngtcp2_cached} "$CACHE{${_ngtcp2_cached}}")
endforeach()

find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_NGTCP2 QUIET libngtcp2)
    pkg_check_modules(PC_NGTCP2_CRYPTO_OSSL QUIET libngtcp2_crypto_ossl)
endif()

find_path(NGTCP2_INCLUDE_DIR
    NAMES ngtcp2/ngtcp2.h
    HINTS ${PC_NGTCP2_INCLUDEDIR} ${PC_NGTCP2_INCLUDE_DIRS}
    PATHS /opt/homebrew/opt/libngtcp2 /usr/local/opt/libngtcp2
    PATH_SUFFIXES include
)

find_library(NGTCP2_LIBRARY
    NAMES ngtcp2
    HINTS ${PC_NGTCP2_LIBDIR} ${PC_NGTCP2_LIBRARY_DIRS}
    PATHS /opt/homebrew/opt/libngtcp2 /usr/local/opt/libngtcp2
    PATH_SUFFIXES lib
)

find_library(NGTCP2_CRYPTO_OSSL_LIBRARY
    NAMES ngtcp2_crypto_ossl
    HINTS ${PC_NGTCP2_CRYPTO_OSSL_LIBDIR} ${PC_NGTCP2_CRYPTO_OSSL_LIBRARY_DIRS}
    PATHS /opt/homebrew/opt/libngtcp2 /usr/local/opt/libngtcp2
    PATH_SUFFIXES lib
)

# The crypto helper HEADERS that src/qb/io/quic.cpp actually includes:
#   <ngtcp2/ngtcp2_crypto.h>      (generic crypto API)
#   <ngtcp2/ngtcp2_crypto_ossl.h> (OpenSSL backend)
# These ship separately from the core dev package and from each other on some
# distros (e.g. Debian splits libngtcp2-dev / libngtcp2-crypto-ossl-dev, and a
# version-mismatched pair can leave the generic ngtcp2_crypto.h missing). Validate
# them explicitly so AUTO-detection matches what the code needs: this keeps the
# single preset set working on every platform — QUIC ON where ngtcp2 is complete
# (vcpkg on Windows, a coherent system install on Linux/macOS), and a clean
# fallback-OFF (rather than a hard compile error) where the install is partial.
find_path(NGTCP2_CRYPTO_INCLUDE_DIR
    NAMES ngtcp2/ngtcp2_crypto.h
    HINTS ${PC_NGTCP2_CRYPTO_OSSL_INCLUDEDIR} ${PC_NGTCP2_CRYPTO_OSSL_INCLUDE_DIRS} ${NGTCP2_INCLUDE_DIR}
    PATHS /opt/homebrew/opt/libngtcp2 /usr/local/opt/libngtcp2
    PATH_SUFFIXES include
)

find_path(NGTCP2_CRYPTO_OSSL_INCLUDE_DIR
    NAMES ngtcp2/ngtcp2_crypto_ossl.h
    HINTS ${PC_NGTCP2_CRYPTO_OSSL_INCLUDEDIR} ${PC_NGTCP2_CRYPTO_OSSL_INCLUDE_DIRS} ${NGTCP2_INCLUDE_DIR}
    PATHS /opt/homebrew/opt/libngtcp2 /usr/local/opt/libngtcp2
    PATH_SUFFIXES include
)

set(Ngtcp2_VERSION "${PC_NGTCP2_VERSION}")
set(Ngtcp2_OPENSSL_MAJOR "")
set(Ngtcp2_OPENSSL_LIBRARY "")
set(Ngtcp2_OPENSSL_CRYPTO_MAJOR "")
set(Ngtcp2_OPENSSL_CRYPTO_LIBRARY "")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ngtcp2
    REQUIRED_VARS
        NGTCP2_INCLUDE_DIR
        NGTCP2_LIBRARY
        NGTCP2_CRYPTO_OSSL_LIBRARY
        NGTCP2_CRYPTO_INCLUDE_DIR
        NGTCP2_CRYPTO_OSSL_INCLUDE_DIR
    VERSION_VAR Ngtcp2_VERSION
)

# What was searched again and found -- an upgrade, not a removal -- is no longer reported, even beside another miss.
set(_ngtcp2_vanished "${Ngtcp2_VANISHED}")
set(Ngtcp2_VANISHED "")
foreach(_ngtcp2_entry IN LISTS _ngtcp2_vanished)
    string(REGEX MATCH "^[A-Z0-9_]+" _ngtcp2_name "${_ngtcp2_entry}")
    if(NOT Ngtcp2_FOUND AND NOT (IS_ABSOLUTE "$CACHE{${_ngtcp2_name}}" AND EXISTS "$CACHE{${_ngtcp2_name}}"))
        list(APPEND Ngtcp2_VANISHED "${_ngtcp2_entry}")
    endif()
endforeach()
unset(_ngtcp2_vanished)
unset(_ngtcp2_entry)
unset(_ngtcp2_name)

if(Ngtcp2_FOUND)
    if(NOT TARGET Ngtcp2::ngtcp2)
        add_library(Ngtcp2::ngtcp2 UNKNOWN IMPORTED)
        set_target_properties(Ngtcp2::ngtcp2 PROPERTIES
            IMPORTED_LOCATION "${NGTCP2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NGTCP2_INCLUDE_DIR}"
        )
    endif()

    if(NOT TARGET Ngtcp2::crypto_ossl)
        # Static archives need their prerequisites after the crypto helper on the link line.
        add_library(Ngtcp2::crypto_ossl UNKNOWN IMPORTED)
        set_target_properties(Ngtcp2::crypto_ossl PROPERTIES
            IMPORTED_LOCATION "${NGTCP2_CRYPTO_OSSL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NGTCP2_CRYPTO_INCLUDE_DIR};${NGTCP2_CRYPTO_OSSL_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "Ngtcp2::ngtcp2;OpenSSL::SSL;OpenSSL::Crypto"
        )
    endif()

    _ngtcp2_linked_openssl("${NGTCP2_CRYPTO_OSSL_LIBRARY}" Ngtcp2_OPENSSL_MAJOR Ngtcp2_OPENSSL_LIBRARY
                           Ngtcp2_OPENSSL_CRYPTO_MAJOR Ngtcp2_OPENSSL_CRYPTO_LIBRARY)
endif()

mark_as_advanced(
    NGTCP2_INCLUDE_DIR
    NGTCP2_LIBRARY
    NGTCP2_CRYPTO_OSSL_LIBRARY
    NGTCP2_CRYPTO_INCLUDE_DIR
    NGTCP2_CRYPTO_OSSL_INCLUDE_DIR
)

# What the search itself found, while it exists: the next search re-runs it only for those.
foreach(_ngtcp2_cached IN LISTS _ngtcp2_cache_entries)
    set(_ngtcp2_path "$CACHE{${_ngtcp2_cached}}")
    if(NOT "${_ngtcp2_path}" STREQUAL "${_ngtcp2_before_${_ngtcp2_cached}}" AND IS_ABSOLUTE "${_ngtcp2_path}"
       AND EXISTS "${_ngtcp2_path}")
        set(_NGTCP2_FOUND_${_ngtcp2_cached} "${_ngtcp2_path}" CACHE INTERNAL "")
    endif()
    unset(_ngtcp2_before_${_ngtcp2_cached})
endforeach()
unset(_ngtcp2_cache_entries)
unset(_ngtcp2_cached)
unset(_ngtcp2_path)
unset(_ngtcp2_found)
