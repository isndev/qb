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
find_package(Doxygen)
if (DOXYGEN_FOUND)
    set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}) # This will be qb/docs
    set(DOXYGEN_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/build/docs) # CMAKE_SOURCE_DIR is qb/
    set(BUILD_DOC_DIR ${CMAKE_SOURCE_DIR}/build/docs)
    if (NOT EXISTS ${BUILD_DOC_DIR})
        file(MAKE_DIRECTORY ${BUILD_DOC_DIR})
    endif ()


    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in) # qb/docs/Doxyfile.in
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile) # e.g. qb/build/docs_cmake_build/Doxyfile

    # Variables needed for Doxyfile.in
    # set(QB_DIRECTORY ${CMAKE_SOURCE_DIR}) # Root of the qb project

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

    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

    # Target name is a fixed, valid identifier. It used to be "${QB_PREFIX}-docs", which
    # expanded to "QB Actor Framework-docs" -- CMake rejects that outright ("The target name
    # ... is reserved or not valid"), so the one configuration that did reach this line failed.
    # QB_PREFIX still names the project inside the generated documentation, via Doxyfile.in.
    message(STATUS "INFO: Doxygen build target 'qb-docs' added (${QB_PREFIX} v${QB_VERSION})")
    add_custom_target(qb-docs ALL
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen for ${QB_PREFIX} v${QB_VERSION}"
            VERBATIM)
else (DOXYGEN_FOUND)
    message(WARNING "Doxygen executable NOT found. Documentation target will not be added.")
endif (DOXYGEN_FOUND)