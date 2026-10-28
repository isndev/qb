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
#
# Imported targets:
#   Ngtcp2::ngtcp2
#   Ngtcp2::crypto_ossl
#
# With NGTCP2_FIND_FUNCTIONS_ONLY set, including this file defines its functions and finds nothing: the dependency-list
# reader's test (qb-io-test-unit-ngtcp2-openssl-listing) reads it that way.
#

# The OpenSSL a dependency listing names -- `otool -L` (Mach-O), `readelf -d` or `objdump -p` (ELF), of a library that
# links libssl or of libssl itself: the major its first libssl carries in its file name (libssl.4.dylib, libssl.so.3;
# libssl.so.1.1 is 1) into <major_var>, and the path when the listing gives one (Mach-O) into <path_var>; both empty
# when it names no versioned libssl. Read line by line, and not as a CMake list, where an unbalanced '[' in a path
# would join lines: an ELF listing's NEEDED and SONAME entries only, a Mach-O one's entries without their
# "(compatibility version ...)", so neither a heading (it names the file read) nor a path with spaces misleads it.
function(_ngtcp2_openssl_in_listing listing major_var path_var)
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
        if("${_line}" MATCHES "(NEEDED|SONAME).*[[ \t/]libssl\\.so\\.([0-9]+)")
            set(_major "${CMAKE_MATCH_2}")
            break()
        endif()
        string(STRIP "${_line}" _entry)
        string(REGEX REPLACE " \\(compatibility version.*$" "" _entry "${_entry}")
        if("${_entry}" MATCHES "(^|/)libssl\\.([0-9]+)(\\.[0-9]+)*\\.dylib$")
            set(_major "${CMAKE_MATCH_2}")
            set(_path "${_entry}")
            break()
        endif()
    endwhile()
    set(${major_var} "${_major}" PARENT_SCOPE)
    set(${path_var} "${_path}" PARENT_SCOPE)
endfunction()

# The OpenSSL the shared library <library> links -- or is, for a libssl -- from its dependency list read with the
# platform's tool (`readelf -d`, then `objdump -p` when readelf is absent or fails). A static archive has no such list,
# Windows is not read (a vcpkg triplet carries one OpenSSL; MSYS2 or an installer's DLLs beside it are not checked),
# and a missing or failing tool yields nothing: no answer, never a guess.
function(_ngtcp2_linked_openssl library major_var path_var)
    set(_listing "")
    set(_rc 1)
    if(WIN32 OR "${library}" MATCHES "\\.(a|lib)$" OR NOT EXISTS "${library}")
        # nothing to read
    elseif(APPLE)
        find_program(NGTCP2_OTOOL_EXECUTABLE otool)
        mark_as_advanced(NGTCP2_OTOOL_EXECUTABLE)
        if(NGTCP2_OTOOL_EXECUTABLE)
            execute_process(COMMAND "${NGTCP2_OTOOL_EXECUTABLE}" -L "${library}"
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
    _ngtcp2_openssl_in_listing("${_listing}" _major _path)
    set(${major_var} "${_major}" PARENT_SCOPE)
    set(${path_var} "${_path}" PARENT_SCOPE)
endfunction()

# What libngtcp2's OpenSSL and qb's are to each other in one process, into <out_var>: MAJORS for two OpenSSL majors
# (measured to crash at the first QUIC handshake, Huly QB-1179); COPIES for two copies of one major -- a static libssl
# in qb beside the shared one the helper loads, or two absolute paths that resolve to two files (Mach-O lists them; an
# @rpath name resolves only at run time); empty for one library, or when a side is unknown: no answer, never a guess.
function(_ngtcp2_openssl_conflict ngtcp2_major ngtcp2_path qb_major qb_path qb_static out_var)
    set(_verdict "")
    if(NOT "${ngtcp2_major}" STREQUAL "" AND NOT "${qb_major}" STREQUAL "" AND NOT "${ngtcp2_major}" STREQUAL "${qb_major}")
        set(_verdict MAJORS)
    elseif(qb_static)
        if(NOT "${ngtcp2_major}" STREQUAL "")
            set(_verdict COPIES)
        endif()
    elseif("${ngtcp2_path}" MATCHES "^/" AND "${qb_path}" MATCHES "^/")
        set(_ngtcp2_real "${ngtcp2_path}")
        set(_qb_real "${qb_path}")
        if(EXISTS "${ngtcp2_path}")
            file(REAL_PATH "${ngtcp2_path}" _ngtcp2_real)
        endif()
        if(EXISTS "${qb_path}")
            file(REAL_PATH "${qb_path}" _qb_real)
        endif()
        if(NOT "${_ngtcp2_real}" STREQUAL "${_qb_real}")
            set(_verdict COPIES)
        endif()
    endif()
    set(${out_var} "${_verdict}" PARENT_SCOPE)
endfunction()

if(NGTCP2_FIND_FUNCTIONS_ONLY)
    return()
endif()

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

    _ngtcp2_linked_openssl("${NGTCP2_CRYPTO_OSSL_LIBRARY}" Ngtcp2_OPENSSL_MAJOR Ngtcp2_OPENSSL_LIBRARY)
endif()

mark_as_advanced(
    NGTCP2_INCLUDE_DIR
    NGTCP2_LIBRARY
    NGTCP2_CRYPTO_OSSL_LIBRARY
    NGTCP2_CRYPTO_INCLUDE_DIR
    NGTCP2_CRYPTO_OSSL_INCLUDE_DIR
)
