#
# qb - C++ Actor Framework
# Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

project(${QB_PREFIX}-core LANGUAGES CXX VERSION ${QB_VERSION} DESCRIPTION "A simple actor library")

cxx_library(${PROJECT_NAME} ${cxx_default_lib} core.cpp)
set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${QB_VERSION} SOVERSION ${QB_VERSION_MAJOR})
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (${QB_PREFIX_UPPER}_WITH_LOG)
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries with logger")
    target_compile_definitions(${QB_PREFIX}-core PUBLIC QB_LOGGER=1)
else()
    message(STATUS "INFO: Will build ${QB_PREFIX} libraries without logger")
endif()

if (${QB_PREFIX_UPPER}_BUILD_TEST)
    add_subdirectory(tests)
endif()