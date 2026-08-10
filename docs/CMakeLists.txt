#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Doxygen Build
#
# There is deliberately NO option() here. The documented switch is QB_BUILD_DOCS
# (cmake/qbConfig.cmake:110, readme/7_reference/cmake_options.md, building.md), and
# qb/CMakeLists.txt:225 already gates `add_subdirectory(docs)` on it -- so reaching this
# file at all means the user asked for documentation.
#
# This file used to declare a SECOND option, QB_BUILD_DOC (singular), defaulting OFF, and
# gate the target on that instead. Nothing anywhere set it, so `-DQB_BUILD_DOCS=ON` entered
# this directory, found Doxygen, configured the Doxyfile -- and created no target at all.
# The documented option was inert, silently, and a configure log carrying "Doxygen build
# target added" never appeared to say otherwise.
# OPTIONAL_COMPONENTS dot, because Doxyfile.in's HAVE_DOT is SUBSTITUTED from what this
# find_package actually resolves. It used to be the unconditional literal `HAVE_DOT = YES`
# next to an empty `DOT_PATH`, i.e. a promise that Graphviz is installed and on PATH, made by
# a file that had never asked. On a host without it Doxygen emits a warning for every graph it
# was told to draw and draws none, so the generated book quietly loses its class,
# collaboration, include and directory diagrams while the run still exits 0.
#
# `dot` is OPTIONAL, not REQUIRED: documentation without diagrams is worth building, and a
# hard failure would make Graphviz a dependency of `-DQB_BUILD_DOCS=ON` on every host.
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if (DOXYGEN_FOUND)
    set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}) # This will be qb/docs

    # Output into the BINARY tree, one directory per build.
    #
    # This used to be ${CMAKE_SOURCE_DIR}/build/docs, with a trailing comment asserting
    # "CMAKE_SOURCE_DIR is qb/". It is not, whenever qb is not the top-level project: the
    # superproject reaches it through add_subdirectory(), so CMAKE_SOURCE_DIR is the qb-dev
    # workspace root and an ALL target was writing generated HTML into the SOURCE checkout.
    # Two build directories over one checkout -- which is the normal state here, CMakePresets
    # gives every preset its own binaryDir under the same source tree -- then had two Doxygen
    # runs writing one output directory with nothing ordering them, and a `cmake --build` of
    # any preset dirtied the working tree.
    # CMAKE_CURRENT_BINARY_DIR is per build dir AND per configuration, so both problems go
    # away without the target having to know whether qb is embedded.
    #
    # DOXYGEN_OUTPUT_DIR was also DEAD before this: Doxyfile.in hard-coded
    # OUTPUT_DIRECTORY = "@CMAKE_SOURCE_DIR@/build/docs" and never referenced the variable,
    # so setting it here changed nothing. It is now what the template substitutes.
    # Doxyfile.in's HTML_OUTPUT is the relative "html", so this is the parent of the HTML.
    set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
    set(BUILD_DOC_DIR ${DOXYGEN_OUTPUT_DIR}/html)
    # Created at BUILD time, not only at configure time: a configure-time-only mkdir leaves
    # the ALL target writing into a missing directory if anything removes it in between.
    file(MAKE_DIRECTORY ${BUILD_DOC_DIR})


    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in) # qb/docs/Doxyfile.in
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile) # e.g. qb/build/docs_cmake_build/Doxyfile

    # Variables needed for Doxyfile.in
    #
    # QB_DIRECTORY is the root of the qb CHECKOUT and it is not optional: Doxyfile.in builds
    # INPUT, EXCLUDE, EXAMPLE_PATH and STRIP_FROM_PATH out of it (lines 51, 146-149, 207-208,
    # 212). It was commented out, so @QB_DIRECTORY@ substituted to the empty string and the
    # generated Doxyfile read `INPUT = "/docs/doxygen_groups.h" "/src/" "/readme"
    # "/docs/mainpage.h"` -- four absolute paths at the filesystem ROOT. Doxygen would have
    # documented nothing (or, on a host that happens to have /src, something else entirely).
    # Nothing caught it because no preset sets QB_BUILD_DOCS, so the target is never built.
    #
    # QB_ROOT_DIR, not CMAKE_SOURCE_DIR: the latter is the top-level project, which is the
    # qb-dev workspace whenever qb is embedded -- the same wrong assumption the output
    # directory above carried.
    if (NOT DEFINED QB_DIRECTORY)
        if (DEFINED QB_ROOT_DIR)
            set(QB_DIRECTORY "${QB_ROOT_DIR}")
        else ()
            set(QB_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
        endif ()
    endif ()

    # QB_PREFIX is the HUMAN-READABLE project name stamped into Doxyfile.in's PROJECT_NAME.
    # It legitimately contains spaces, which is why it must never be used to build a target
    # name -- see the add_custom_target below.
    if (NOT DEFINED QB_PREFIX)
        set(QB_PREFIX "QB Actor Framework") # Default project name
        message(STATUS "QB_PREFIX not set by parent, using default: ${QB_PREFIX}")
    endif ()
    # Version source of truth is cmake/qbConfig.cmake QB_FRAMEWORK_VERSION (VERSIONING.md).
    # It is already set when this directory is added from qb/CMakeLists.txt; the fallback is
    # for a standalone `add_subdirectory(docs)`. It used to fall back to a hand-written
    # "0.0.0-dev", which is exactly the stale-version-literal shape VERSIONING.md forbids --
    # generated documentation was stamped 0.0.0-dev whenever the parent had not set QB_VERSION,
    # which was always.
    if (NOT DEFINED QB_VERSION)
        if (DEFINED QB_FRAMEWORK_VERSION)
            set(QB_VERSION "${QB_FRAMEWORK_VERSION}")
        else ()
            set(QB_VERSION "${PROJECT_VERSION}")
        endif ()
        message(STATUS "QB_VERSION not set by parent, derived: ${QB_VERSION}")
    endif ()

    # HAVE_DOT / DOT_PATH: report what the configure FOUND, never what we hope is installed.
    #
    # DOT_PATH is the DIRECTORY holding the tool, not the tool itself -- Doxygen appends the
    # executable name to it -- so a full path here silently disables dot again. FindDoxygen
    # gives the executable; the directory is derived from it.
    #
    # Leaving DOT_PATH empty when dot IS found would also work on a host where it is on PATH,
    # and would break the one where CMake located it somewhere else (a Homebrew or vcpkg
    # prefix that the build shell exports but a desktop-launched IDE does not). Naming the
    # resolved directory costs nothing and removes the difference.
    if (TARGET Doxygen::dot AND DOXYGEN_DOT_EXECUTABLE)
        set(QB_DOXYGEN_HAVE_DOT "YES")
        get_filename_component(QB_DOXYGEN_DOT_PATH "${DOXYGEN_DOT_EXECUTABLE}" DIRECTORY)
        message(STATUS "INFO: Graphviz dot found (${DOXYGEN_DOT_EXECUTABLE}); Doxygen graphs enabled")
    else ()
        set(QB_DOXYGEN_HAVE_DOT "NO")
        set(QB_DOXYGEN_DOT_PATH "")
        message(STATUS "INFO: Graphviz dot NOT found; Doxygen graphs disabled (HAVE_DOT=NO). "
                       "Install Graphviz to get the class/collaboration/include diagrams.")
    endif ()

    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

    # Target name is a fixed, valid identifier. It used to be "${QB_PREFIX}-docs", which
    # expanded to "QB Actor Framework-docs" -- CMake rejects that outright ("The target name
    # ... is reserved or not valid"), so the one configuration that did reach this line failed.
    # QB_PREFIX still names the project inside the generated documentation, via Doxyfile.in.
    message(STATUS "INFO: Doxygen build target 'qb-docs' added (${QB_PREFIX} v${QB_VERSION})")
    add_custom_target(qb-docs ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DOC_DIR}
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen for ${QB_PREFIX} v${QB_VERSION}"
            VERBATIM)
else (DOXYGEN_FOUND)
    message(WARNING "Doxygen executable NOT found. Documentation target will not be added.")
endif (DOXYGEN_FOUND)