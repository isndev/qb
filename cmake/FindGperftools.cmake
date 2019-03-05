# -------------
#
# Find a Google gperftools installation.
#
# This module finds if Google gperftools is installed and selects a default
# configuration to use.
#
#   find_package(GPERFTOOLS COMPONENTS ...)
#
# Valid components are:
#
#   TCMALLOC
#   PROFILER
#   TCMALLOC_MINIMAL
#   TCMALLOC_AND_PROFILER
#
# The following variables control which libraries are found::
#
#   GPERFTOOLS_USE_STATIC_LIBS  - Set to ON to force use of static libraries.
#
# The following are set after the configuration is done:
#
# ::
#
#   GPERFTOOLS_FOUND            - Set to TRUE if gperftools was found
#   GPERFTOOLS_INCLUDE_DIRS     - Include directories
#   GPERFTOOLS_LIBRARIES        - Path to the gperftools libraries
#   GPERFTOOLS_LIBRARY_DIRS     - Compile time link directories
#   GPERFTOOLS_<component>      - Path to specified component
#
#
# Sample usage:
#
# ::
#
#   find_package(GPERFTOOLS)
#   if(GPERFTOOLS_FOUND)
#     target_link_libraries(<YourTarget> ${GPERFTOOLS_LIBRARIES})
#   endif()

if(GPERFTOOLS_USE_STATIC_LIBS)
    set(_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

macro(_find_library libvar libname)
    find_library(${libvar}
            NAMES ${libname}
            HINTS ENV LD_LIBRARY_PATH
            HINTS ENV DYLD_LIBRARY_PATH
            PATHS
            /usr/lib
            /usr/local/lib
            /usr/local/homebrew/lib
            /opt/local/lib
            )
    if(NOT ${libvar}_NOTFOUND)
        set(${libvar}_FOUND TRUE)
    endif()
endmacro()

_find_library(GPERFTOOLS_TCMALLOC tcmalloc)
_find_library(GPERFTOOLS_PROFILER profiler)
_find_library(GPERFTOOLS_TCMALLOC_MINIMAL tcmalloc_minimal)
_find_library(GPERFTOOLS_TCMALLOC_AND_PROFILER tcmalloc_and_profiler)

find_path(GPERFTOOLS_INCLUDE_DIR
        NAMES gperftools/heap-profiler.h
        HINTS ${GPERFTOOLS_LIBRARY}/../../include
        PATHS
        /usr/include
        /usr/local/include
        /usr/local/homebrew/include
        /opt/local/include
        )

get_filename_component(GPERFTOOLS_LIBRARY_DIR ${GPERFTOOLS_TCMALLOC} DIRECTORY)
# Set standard CMake FindPackage variables if found.
set(GPERFTOOLS_LIBRARIES
        ${GPERFTOOLS_TCMALLOC}
        ${GPERFTOOLS_PROFILER}
        )

set(GPERFTOOLS_INCLUDE_DIRS ${GPERFTOOLS_INCLUDE_DIR})
set(GPERFTOOLS_LIBRARY_DIRS ${GPERFTOOLS_LIBRARY_DIR})

if(GPERFTOOLS_USE_STATIC_LIBS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
        GPERFTOOLS
        REQUIRED_VARS
        GPERFTOOLS_INCLUDE_DIR
        HANDLE_COMPONENTS
)