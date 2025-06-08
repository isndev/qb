#
# qb - C++ Actor Framework
# Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
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
option(QB_BUILD_DOC "Build Documentation" OFF)

find_package(Doxygen)
if(DOXYGEN_FOUND)
    set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}) # This will be qb/docs
    set(DOXYGEN_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/build/docs) # CMAKE_SOURCE_DIR is qb/
    set(BUILD_DOC_DIR ${CMAKE_SOURCE_DIR}/build/docs)
    if(NOT EXISTS ${BUILD_DOC_DIR})
        file(MAKE_DIRECTORY ${BUILD_DOC_DIR})
    endif()


    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in) # qb/docs/Doxyfile.in
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile) # e.g. qb/build/docs_cmake_build/Doxyfile

    # Variables needed for Doxyfile.in
    # set(QB_DIRECTORY ${CMAKE_SOURCE_DIR}) # Root of the qb project

    if(NOT DEFINED QB_PREFIX)
        set(QB_PREFIX "QB Actor Framework") # Default project name
        message(STATUS "QB_PREFIX not set by parent, using default: ${QB_PREFIX}")
    endif()
    if(NOT DEFINED QB_VERSION)
        set(QB_VERSION "0.0.0-dev") # Default version
        message(STATUS "QB_VERSION not set by parent, using default: ${QB_VERSION}")
    endif()

    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

    if (QB_BUILD_DOC)
        message(STATUS "INFO: Doxygen build target '${QB_PREFIX}-docs' added to solution")
        add_custom_target(${QB_PREFIX}-docs ALL
                COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                COMMENT "Generating API documentation with Doxygen for ${QB_PREFIX} v${QB_VERSION}"
                VERBATIM)
    endif()
else(DOXYGEN_FOUND)
    message(WARNING "Doxygen executable NOT found. Documentation target will not be added.")
endif(DOXYGEN_FOUND)