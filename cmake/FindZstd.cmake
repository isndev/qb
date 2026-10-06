# FindZstd.cmake
# Locates the zstd compression library (Huly QB-79: QB_WITH_ZSTD).
#
# Result variables:
#   Zstd_FOUND
#   Zstd_VERSION        from pkg-config when available, else parsed from zstd.h
#
# Imported target:
#   Zstd::Zstd
#
# pkg-config first, then find_path/find_library, as FindArgon2.cmake and FindNgtcp2.cmake resolve: pkg-config
# names what a sandboxed package build (a brew formula, a vcpkg port) actually declared, and the bare search is
# what still works on a Windows/vcpkg host without pkg-config. zstd's own CMake package (zstdConfig.cmake) is NOT
# used on purpose: its target names changed across releases (zstd::libzstd_shared / _static, then zstd::libzstd)
# and a distribution may ship the library without it, while this module gives every host one target name --
# which an installed qb must be able to recreate for its consumers (qbConfig.cmake.in calls this module).
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_ZSTD QUIET libzstd)
endif()

find_path(ZSTD_INCLUDE_DIR
    NAMES zstd.h
    HINTS ${PC_ZSTD_INCLUDEDIR} ${PC_ZSTD_INCLUDE_DIRS}
    PATHS /opt/homebrew/opt/zstd /usr/local/opt/zstd
    PATH_SUFFIXES include
)

find_library(ZSTD_LIBRARY
    NAMES zstd zstd_static libzstd
    HINTS ${PC_ZSTD_LIBDIR} ${PC_ZSTD_LIBRARY_DIRS}
    PATHS /opt/homebrew/opt/zstd /usr/local/opt/zstd
    PATH_SUFFIXES lib
)

set(Zstd_VERSION "${PC_ZSTD_VERSION}")
if(NOT Zstd_VERSION AND ZSTD_INCLUDE_DIR AND EXISTS "${ZSTD_INCLUDE_DIR}/zstd.h")
    file(STRINGS "${ZSTD_INCLUDE_DIR}/zstd.h" _zstd_version_lines REGEX "^#define ZSTD_VERSION_(MAJOR|MINOR|RELEASE) +[0-9]+")
    foreach(_part MAJOR MINOR RELEASE)
        string(REGEX REPLACE ".*#define ZSTD_VERSION_${_part} +([0-9]+).*" "\\1" _zstd_${_part} "${_zstd_version_lines}")
    endforeach()
    set(Zstd_VERSION "${_zstd_MAJOR}.${_zstd_MINOR}.${_zstd_RELEASE}")
    unset(_zstd_version_lines)
    unset(_zstd_MAJOR)
    unset(_zstd_MINOR)
    unset(_zstd_RELEASE)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Zstd
    REQUIRED_VARS ZSTD_LIBRARY ZSTD_INCLUDE_DIR
    VERSION_VAR Zstd_VERSION
)

if(Zstd_FOUND AND NOT TARGET Zstd::Zstd)
    add_library(Zstd::Zstd UNKNOWN IMPORTED)
    set_target_properties(Zstd::Zstd PROPERTIES
        IMPORTED_LOCATION "${ZSTD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(ZSTD_INCLUDE_DIR ZSTD_LIBRARY)
