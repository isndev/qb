# Tries to find Gperftools.
#
# Usage of this module as follows:
#
#     find_package(Gperftools)
#
# Variables used by this module, they can change the default behaviour and need
# to be set before calling find_package:
#
#  Gperftools_ROOT_DIR  Set this variable to the root installation of
#                       Gperftools if the module has problems finding
#                       the proper installation path.
#
# Variables defined by this module:
#
#  GPERFTOOLS_FOUND              System has Gperftools libs/headers
#  GPERFTOOLS_LIBRARIES          The Gperftools libraries (tcmalloc & profiler)
#  GPERFTOOLS_INCLUDE_DIR        The location of Gperftools headers

find_library(GPERFTOOLS_TCMALLOC
        NAMES tcmalloc
        HINTS ${Gperftools_ROOT_DIR}/lib)

find_library(GPERFTOOLS_PROFILER
        NAMES profiler
        HINTS ${Gperftools_ROOT_DIR}/lib)

find_library(GPERFTOOLS_TCMALLOC_AND_PROFILER
        NAMES tcmalloc_and_profiler
        HINTS ${Gperftools_ROOT_DIR}/lib)

find_path(GPERFTOOLS_INCLUDE_DIR
        NAMES gperftools/heap-profiler.h
        HINTS ${Gperftools_ROOT_DIR}/include)

set(GPERFTOOLS_LIBRARIES ${GPERFTOOLS_TCMALLOC_AND_PROFILER})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
        Gperftools
        DEFAULT_MSG
        GPERFTOOLS_LIBRARIES
        GPERFTOOLS_INCLUDE_DIR)

mark_as_advanced(
        Gperftools_ROOT_DIR
        GPERFTOOLS_TCMALLOC
        GPERFTOOLS_PROFILER
        GPERFTOOLS_TCMALLOC_AND_PROFILER
        GPERFTOOLS_LIBRARIES
        GPERFTOOLS_INCLUDE_DIR)

#if (WIN32)
#
#    find_library(
#            TCMALLOC_RELEASE
#            NAMES libtcmalloc_minimal
#            PATHS ${CMAKE_CURRENT_SOURCE_DIR}/external/tcmalloc/win32/release
#    )
#
#    find_library(
#            TCMALLOC_DEBUG
#            NAMES libtcmalloc_minimal-debug
#            PATHS ${CMAKE_CURRENT_SOURCE_DIR}/external/tcmalloc/win32/debug
#    )
#
#    if (TCMALLOC_RELEASE)
#        message(STATUS "INFO: will use tcmalloc")
#        if (GNU)
#            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
#        elseif (MSVC)
#            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /INCLUDE:\"__tcmalloc\"")
#        endif()
#    elseif (USE_TC_MALLOC)
#        message(STATUS "INFO: tcmalloc not found")
#        set(USE_TC_MALLOC OFF)
#    endif()
#
#elseif(UNIX)
#    set(USE_TC_MALLOC OFF)
#endif()