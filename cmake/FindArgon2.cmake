# FindArgon2.cmake
# Fully portable, supports common packaging systems
#
# Result variables:
#   ARGON2_FOUND
#   ARGON2_VERSION_STRING   library release version (from pkg-config; see the note below)
#   ARGON2_FORMAT_VERSION   argon2 HASH FORMAT version, e.g. "1.3" (from argon2.h)
#
# Imported targets:
#   Argon2::Argon2

# pkg-config first, for two reasons, and both of them were defects here:
#
#   1. VERSION. argon2.h declares no library version at all -- what it does declare is
#      ARGON2_VERSION_NUMBER, the argon2 HASH FORMAT version (0x13 = format v1.3), as an enum
#      member. The library's real release version (e.g. 20190702) exists only in libargon2.pc.
#   2. PATHS. This module used to search bare hard-coded prefixes (/opt/homebrew, /usr/local, ...)
#      with no hints, so inside a sandboxed package build (a brew formula, a vcpkg port) it could
#      bind an UNDECLARED system argon2 sitting at one of those paths -- and QB_HAS_ARGON2=1 is a
#      PUBLIC compile definition, so that silently changes the package's public surface.
#      pkg-config resolves to whatever the sandbox actually declared. The hard-coded PATHS are
#      kept below as a last resort (find_path consults HINTS before PATHS), because they are still
#      what makes this work on a Windows/vcpkg host with no pkg-config.
#
# FindNgtcp2.cmake and FindNghttp3.cmake already resolve this way; this module was the holdout.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_ARGON2 QUIET libargon2)
endif()

find_path(ARGON2_INCLUDE_DIR
        NAMES argon2.h
        HINTS ${PC_ARGON2_INCLUDEDIR} ${PC_ARGON2_INCLUDE_DIRS}
        PATHS
        # Linux / BSD / WSL
        /usr/include
        /usr/local/include
        /opt/local/include
        # macOS - Homebrew (Intel + ARM)
        /usr/local/opt/argon2/include
        /opt/homebrew/include
        /opt/homebrew/opt/argon2/include
        # Windows (vcpkg or manual install)
        $ENV{ARGON2_ROOT}/include
        PATH_SUFFIXES include
        DOC "Path to argon2.h"
)

find_library(ARGON2_LIBRARY
        NAMES argon2 libargon2
        HINTS ${PC_ARGON2_LIBDIR} ${PC_ARGON2_LIBRARY_DIRS}
        PATHS
        /usr/lib
        /usr/local/lib
        /opt/local/lib
        /usr/local/opt/argon2/lib
        /opt/homebrew/lib
        /opt/homebrew/opt/argon2/lib
        $ENV{ARGON2_ROOT}/lib
        PATH_SUFFIXES lib
        DOC "Path to argon2 static or shared library"
)

# -- version ----------------------------------------------------------------------------------
#
# Two DIFFERENT numbers live here, and conflating them is what this block used to do.
#
#   ARGON2_FORMAT_VERSION  the argon2 hash FORMAT version, from argon2.h. Upstream declares it as
#                          an ENUM MEMBER, not a #define:
#                              argon2.h:  ARGON2_VERSION_NUMBER = ARGON2_VERSION_13
#                          0x13 means format v1.3. It says nothing about which release of the
#                          library is installed -- 0x13 has been the value since 2016.
#   ARGON2_VERSION_STRING  the library RELEASE version (e.g. 20190702), which appears nowhere in
#                          any header. pkg-config is the only place it exists.
#
# History, because both spellings were wrong in a way that looked right:
#   * The original `#define ARGON2_VERSION_NUMBER +[0-9]+` regex matched nothing on any real
#     installation (the enum form above is what ships), so ARGON2_VERSION_STRING was always empty
#     and `-- Found Argon2: ` printed a blank version. An empty VERSION_VAR is not benign:
#     find_package_handle_standard_args treats it as "unsuitable" for ANY version request, so a
#     consumer of the INSTALLED FindArgon2.cmake could never satisfy find_package(Argon2 <n>).
#   * Matching the enum and then decoding 0x13 through the packed-hex major/minor/patch arithmetic
#     that was already here yielded "0.0.19" -- a triple that corresponds to nothing, and which
#     still failed find_package(Argon2 20190702), just with a fabricated number in the message.
#
# So: take the release version from pkg-config, and expose the format version separately under a
# name that says what it is. When pkg-config is absent (typically Windows) the release version is
# genuinely unknown; leave it empty rather than substituting the format version, so a versioned
# request fails loudly instead of being answered with the wrong number.
set(ARGON2_FORMAT_VERSION "")
if(ARGON2_INCLUDE_DIR AND EXISTS "${ARGON2_INCLUDE_DIR}/argon2.h")
    file(STRINGS "${ARGON2_INCLUDE_DIR}/argon2.h" _argon2_version_lines
         REGEX "(#[ \t]*define[ \t]+ARGON2_VERSION_NUMBER|ARGON2_VERSION_NUMBER[ \t]*=)")
    if(_argon2_version_lines)
        list(GET _argon2_version_lines 0 _argon2_version_line)
        # "... = ARGON2_VERSION_13" -> 13 ; "#define ARGON2_VERSION_NUMBER 0x13" -> 13
        if(_argon2_version_line MATCHES "ARGON2_VERSION_([0-9]+)[ \t]*,?[ \t]*$")
            set(_argon2_fmt_digits "${CMAKE_MATCH_1}")
        elseif(_argon2_version_line MATCHES "0x0*([0-9A-Fa-f]+)[ \t]*,?[ \t]*$")
            set(_argon2_fmt_digits "${CMAKE_MATCH_1}")
        endif()
        # The digits are read as hex nibbles: 13 -> "1.3", 10 -> "1.0".
        if(_argon2_fmt_digits MATCHES "^([0-9A-Fa-f])([0-9A-Fa-f])$")
            set(ARGON2_FORMAT_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
        endif()
        unset(_argon2_fmt_digits)
        unset(_argon2_version_line)
    endif()
    unset(_argon2_version_lines)
endif()

set(ARGON2_VERSION_STRING "${PC_ARGON2_VERSION}")
if(NOT ARGON2_VERSION_STRING AND ARGON2_INCLUDE_DIR AND Argon2_FIND_VERSION)
    # Only worth saying when someone actually asked for a version and we cannot answer.
    message(STATUS
        "[FindArgon2] argon2 was located but its release version is unknown: no pkg-config, and "
        "argon2.h carries only the hash-format version (ARGON2_FORMAT_VERSION="
        "${ARGON2_FORMAT_VERSION}). The find_package(Argon2 ${Argon2_FIND_VERSION}) request below "
        "cannot be satisfied -- install pkg-config, or drop the version from the request.")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Argon2
        REQUIRED_VARS ARGON2_LIBRARY ARGON2_INCLUDE_DIR
        VERSION_VAR ARGON2_VERSION_STRING
)

if(ARGON2_FOUND)
    set(ARGON2_INCLUDE_DIRS ${ARGON2_INCLUDE_DIR})
    set(ARGON2_LIBRARIES ${ARGON2_LIBRARY})

    # Create a modern imported target so consumers automatically inherit include dirs
    # when linked via target_link_libraries(... Argon2::Argon2).
    if(NOT TARGET Argon2::Argon2)
        add_library(Argon2::Argon2 UNKNOWN IMPORTED)
        set_target_properties(Argon2::Argon2 PROPERTIES
            IMPORTED_LOCATION             "${ARGON2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ARGON2_INCLUDE_DIR}"
        )
    endif()
endif()
