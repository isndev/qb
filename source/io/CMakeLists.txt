#
# qb - C++ Actor Framework
# Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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
#         limitations under the License.
#

project(${QB_PREFIX}-io LANGUAGES CXX VERSION ${QB_VERSION} DESCRIPTION "A simple io library")

set(CURRENT_DEPENDENCIES ev stduuid)
set(CURRENT_DEFINITIONS QB_IO_VERSION=${QB_VERSION})
set(CURRENT_INCLUDES)

if (${QB_PREFIX_UPPER}_WITH_LOG)
    list(APPEND CURRENT_DEFINITIONS QB_LOGGER=1)
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries with logger")
else()
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries without logger")
    if(${QB_PREFIX_UPPER}_STDOUT_LOG)
        list(APPEND CURRENT_DEFINITIONS QB_STDOUT_LOG=1)
        message(STATUS "INFO: Will build ${QB_PREFIX} print logs")
    endif()
endif()

if (WIN32)
    list(APPEND CURRENT_DEPENDENCIES ws2_32)
endif()
if(ZLIB_FOUND)
    list(APPEND CURRENT_DEPENDENCIES ZLIB::ZLIB)
    list(APPEND CURRENT_DEFINITIONS QB_IO_WITH_ZLIB=1)
    list(APPEND ${ZLIB_INCLUDE_DIRS})
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries with zlib")
endif()
if (OPENSSL_FOUND)
    list(APPEND CURRENT_DEPENDENCIES OpenSSL::SSL)
    list(APPEND CURRENT_DEFINITIONS QB_IO_WITH_SSL=1)
    list(APPEND ${OPENSSL_INCLUDE_DIR})
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries with openssl")
endif ()

if (${CURRENT_INCLUDES})
    include_directories(${CURRENT_INCLUDES})
endif ()

cxx_library(NAME ${PROJECT_NAME}
        FLAGS ${cxx_default_lib}
        SOURCES src/io.cpp
        DEPENDENCIES ${CURRENT_DEPENDENCIES})

set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${QB_VERSION})
foreach (def ${CURRENT_DEFINITIONS})
    target_compile_definitions(${PROJECT_NAME} PUBLIC ${def})
endforeach ()
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (${QB_PREFIX_UPPER}_BUILD_TEST)
    add_subdirectory(tests)
endif()
