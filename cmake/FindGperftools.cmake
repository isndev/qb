# FindGperftools.cmake
#
# Find a Google gperftools installation and expose modern IMPORTED targets.
#
# Usage:
#   find_package(Gperftools [QUIET] [COMPONENTS TCMALLOC PROFILER ...])
#
# Valid COMPONENTS:
#   TCMALLOC               - full tcmalloc
#   PROFILER               - CPU profiler
#   TCMALLOC_MINIMAL       - lightweight tcmalloc (no heap profiler)
#   TCMALLOC_AND_PROFILER  - combined tcmalloc + profiler
#
# Result variables (set after find_package):
#   Gperftools_FOUND          - TRUE if at least PROFILER or TCMALLOC was found
#   GPERFTOOLS_INCLUDE_DIRS   - Include directories
#   GPERFTOOLS_LIBRARIES      - Link libraries (only found components, no -NOTFOUND entries)
#   GPERFTOOLS_LIBRARY_DIRS   - Library directory
#
# Imported targets (created when the corresponding library is found):
#   Gperftools::Profiler      - CPU profiler library
#   Gperftools::TCMalloc      - Full TCMalloc library
#   Gperftools::TCMalloc_Minimal
#   Gperftools::TCMalloc_And_Profiler
#
# Hints:
#   GPERFTOOLS_USE_STATIC_LIBS - Set ON to prefer static libraries.
#   GPERFTOOLS_ROOT_DIR        - Root of a custom gperftools installation.

if(GPERFTOOLS_USE_STATIC_LIBS)
    set(_gperf_ORIG_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

# ── Helper: find one library component ───────────────────────────────────────
macro(_gperftools_find_lib var libname)
    find_library(${var}
        NAMES ${libname}
        HINTS
            ENV LD_LIBRARY_PATH
            ENV DYLD_LIBRARY_PATH
            "${GPERFTOOLS_ROOT_DIR}/lib"
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/local/homebrew/lib
            /opt/homebrew/lib
            /opt/local/lib
    )
endmacro()

_gperftools_find_lib(GPERFTOOLS_TCMALLOC               tcmalloc)
_gperftools_find_lib(GPERFTOOLS_PROFILER                profiler)
_gperftools_find_lib(GPERFTOOLS_TCMALLOC_MINIMAL        tcmalloc_minimal)
_gperftools_find_lib(GPERFTOOLS_TCMALLOC_AND_PROFILER   tcmalloc_and_profiler)

find_path(GPERFTOOLS_INCLUDE_DIR
    NAMES gperftools/heap-profiler.h
    HINTS "${GPERFTOOLS_ROOT_DIR}/include"
    PATHS
        /usr/include
        /usr/local/include
        /usr/local/homebrew/include
        /opt/homebrew/include
        /opt/local/include
)

# ── Build clean library list (drop any -NOTFOUND entries) ────────────────────
set(GPERFTOOLS_LIBRARIES)
foreach(_lib
    GPERFTOOLS_TCMALLOC
    GPERFTOOLS_PROFILER
    GPERFTOOLS_TCMALLOC_MINIMAL
    GPERFTOOLS_TCMALLOC_AND_PROFILER)
    if(${_lib} AND NOT ${_lib} MATCHES "NOTFOUND")
        list(APPEND GPERFTOOLS_LIBRARIES ${${_lib}})
    endif()
endforeach()

set(GPERFTOOLS_INCLUDE_DIRS ${GPERFTOOLS_INCLUDE_DIR})

# Library directory (only when at least one component was found)
if(GPERFTOOLS_TCMALLOC AND NOT GPERFTOOLS_TCMALLOC MATCHES "NOTFOUND")
    get_filename_component(GPERFTOOLS_LIBRARY_DIRS ${GPERFTOOLS_TCMALLOC} DIRECTORY)
elseif(GPERFTOOLS_PROFILER AND NOT GPERFTOOLS_PROFILER MATCHES "NOTFOUND")
    get_filename_component(GPERFTOOLS_LIBRARY_DIRS ${GPERFTOOLS_PROFILER} DIRECTORY)
endif()

if(GPERFTOOLS_USE_STATIC_LIBS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_gperf_ORIG_SUFFIXES})
endif()

# ── Standard result handling ──────────────────────────────────────────────────
# NOTE: package name MUST match what find_package() was called with so that
# find_package_handle_standard_args sets the right <PackageName>_FOUND variable.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Gperftools
    REQUIRED_VARS GPERFTOOLS_INCLUDE_DIR
    HANDLE_COMPONENTS
)

# ── Create IMPORTED targets ───────────────────────────────────────────────────
if(Gperftools_FOUND)
    macro(_gperftools_make_target target lib)
        if(${lib} AND NOT ${lib} MATCHES "NOTFOUND" AND NOT TARGET ${target})
            add_library(${target} UNKNOWN IMPORTED)
            set_target_properties(${target} PROPERTIES
                IMPORTED_LOCATION             "${${lib}}"
                INTERFACE_INCLUDE_DIRECTORIES "${GPERFTOOLS_INCLUDE_DIR}"
            )
        endif()
    endmacro()

    _gperftools_make_target(Gperftools::TCMalloc             GPERFTOOLS_TCMALLOC)
    _gperftools_make_target(Gperftools::Profiler              GPERFTOOLS_PROFILER)
    _gperftools_make_target(Gperftools::TCMalloc_Minimal      GPERFTOOLS_TCMALLOC_MINIMAL)
    _gperftools_make_target(Gperftools::TCMalloc_And_Profiler GPERFTOOLS_TCMALLOC_AND_PROFILER)
endif()
