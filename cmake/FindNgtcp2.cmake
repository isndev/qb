#
# Find libngtcp2 and its OpenSSL crypto helper.
#
# Result variables:
#   Ngtcp2_FOUND
#   Ngtcp2_VERSION
#
# Imported targets:
#   Ngtcp2::ngtcp2
#   Ngtcp2::crypto_ossl
#

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

set(Ngtcp2_VERSION "${PC_NGTCP2_VERSION}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ngtcp2
    REQUIRED_VARS NGTCP2_INCLUDE_DIR NGTCP2_LIBRARY NGTCP2_CRYPTO_OSSL_LIBRARY
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
        add_library(Ngtcp2::crypto_ossl UNKNOWN IMPORTED)
        set_target_properties(Ngtcp2::crypto_ossl PROPERTIES
            IMPORTED_LOCATION "${NGTCP2_CRYPTO_OSSL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NGTCP2_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(
    NGTCP2_INCLUDE_DIR
    NGTCP2_LIBRARY
    NGTCP2_CRYPTO_OSSL_LIBRARY
)
