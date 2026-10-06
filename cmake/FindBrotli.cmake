# FindBrotli.cmake
# Locates the brotli compression libraries (Huly QB-79: QB_WITH_BROTLI).
#
# Result variables:
#   Brotli_FOUND
#   Brotli_VERSION      from pkg-config when available
#
# Imported targets:
#   Brotli::Encoder     libbrotlienc (links Brotli::Common)
#   Brotli::Decoder     libbrotlidec (links Brotli::Common)
#   Brotli::Common      libbrotlicommon
#
# pkg-config first, then find_path/find_library, as FindArgon2.cmake and FindNgtcp2.cmake resolve. Brotli ships
# no CMake package upstream (vcpkg's is `unofficial-brotli`), so this module gives every host the same target
# names -- which an installed qb must be able to recreate for its consumers (qbConfig.cmake.in calls this module).
# The common library is linked explicitly: a static brotlienc/brotlidec does not carry it.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_BROTLIENC QUIET libbrotlienc)
    pkg_check_modules(PC_BROTLIDEC QUIET libbrotlidec)
    pkg_check_modules(PC_BROTLICOMMON QUIET libbrotlicommon)
endif()

find_path(BROTLI_INCLUDE_DIR
    NAMES brotli/encode.h
    HINTS ${PC_BROTLIENC_INCLUDEDIR} ${PC_BROTLIENC_INCLUDE_DIRS}
    PATHS /opt/homebrew/opt/brotli /usr/local/opt/brotli
    PATH_SUFFIXES include
)

find_library(BROTLI_ENCODER_LIBRARY
    NAMES brotlienc brotlienc-static
    HINTS ${PC_BROTLIENC_LIBDIR} ${PC_BROTLIENC_LIBRARY_DIRS}
    PATHS /opt/homebrew/opt/brotli /usr/local/opt/brotli
    PATH_SUFFIXES lib
)

find_library(BROTLI_DECODER_LIBRARY
    NAMES brotlidec brotlidec-static
    HINTS ${PC_BROTLIDEC_LIBDIR} ${PC_BROTLIDEC_LIBRARY_DIRS}
    PATHS /opt/homebrew/opt/brotli /usr/local/opt/brotli
    PATH_SUFFIXES lib
)

find_library(BROTLI_COMMON_LIBRARY
    NAMES brotlicommon brotlicommon-static
    HINTS ${PC_BROTLICOMMON_LIBDIR} ${PC_BROTLICOMMON_LIBRARY_DIRS}
    PATHS /opt/homebrew/opt/brotli /usr/local/opt/brotli
    PATH_SUFFIXES lib
)

set(Brotli_VERSION "${PC_BROTLIENC_VERSION}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Brotli
    REQUIRED_VARS BROTLI_ENCODER_LIBRARY BROTLI_DECODER_LIBRARY BROTLI_COMMON_LIBRARY BROTLI_INCLUDE_DIR
    VERSION_VAR Brotli_VERSION
)

if(Brotli_FOUND)
    if(NOT TARGET Brotli::Common)
        add_library(Brotli::Common UNKNOWN IMPORTED)
        set_target_properties(Brotli::Common PROPERTIES
            IMPORTED_LOCATION "${BROTLI_COMMON_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${BROTLI_INCLUDE_DIR}"
        )
    endif()
    if(NOT TARGET Brotli::Encoder)
        add_library(Brotli::Encoder UNKNOWN IMPORTED)
        set_target_properties(Brotli::Encoder PROPERTIES
            IMPORTED_LOCATION "${BROTLI_ENCODER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${BROTLI_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES Brotli::Common
        )
    endif()
    if(NOT TARGET Brotli::Decoder)
        add_library(Brotli::Decoder UNKNOWN IMPORTED)
        set_target_properties(Brotli::Decoder PROPERTIES
            IMPORTED_LOCATION "${BROTLI_DECODER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${BROTLI_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES Brotli::Common
        )
    endif()
endif()

mark_as_advanced(BROTLI_INCLUDE_DIR BROTLI_ENCODER_LIBRARY BROTLI_DECODER_LIBRARY BROTLI_COMMON_LIBRARY)
