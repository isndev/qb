# FindArgon2.cmake
# Fully portable, supports common packaging systems

find_path(ARGON2_INCLUDE_DIR
        NAMES argon2.h
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

# Optional version extraction
if(ARGON2_INCLUDE_DIR AND EXISTS "${ARGON2_INCLUDE_DIR}/argon2.h")
    file(STRINGS "${ARGON2_INCLUDE_DIR}/argon2.h" _version_line REGEX "#define ARGON2_VERSION_NUMBER +[0-9]+")
    string(REGEX REPLACE ".* ([0-9]+)" "\\1" ARGON2_VERSION_RAW "${_version_line}")
    if(ARGON2_VERSION_RAW)
        math(EXPR ARGON2_VERSION_MAJOR "${ARGON2_VERSION_RAW} / 0x10000")
        math(EXPR ARGON2_VERSION_MINOR "(${ARGON2_VERSION_RAW} / 0x100) % 0x100")
        math(EXPR ARGON2_VERSION_PATCH "${ARGON2_VERSION_RAW} % 0x100")
        set(ARGON2_VERSION_STRING "${ARGON2_VERSION_MAJOR}.${ARGON2_VERSION_MINOR}.${ARGON2_VERSION_PATCH}")
    endif()
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
