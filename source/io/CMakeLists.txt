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

project(${QB_PREFIX}-io LANGUAGES CXX VERSION ${QB_VERSION} DESCRIPTION "A simple io library")

find_package(OpenSSL)
if (OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
    cxx_library(NAME ${PROJECT_NAME}
            FLAGS ${cxx_default_lib}
            SOURCES "src/io-all.cpp"
            DEPENDENCIES "ev " OpenSSL::SSL)
    target_compile_definitions(${PROJECT_NAME} PUBLIC QB_IO_WITH_SSL)
else()
    cxx_library(NAME ${PROJECT_NAME}
            FLAGS ${cxx_default_lib}
            SOURCES "src/io-all.cpp"
            DEPENDENCIES ev)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${QB_VERSION})
target_include_directories(${PROJECT_NAME} INTERFACE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

#cxx_shared_library(NAME ${PROJECT_NAME}-shared
#        FLAGS ${cxx_default_lib}
#        SOURCES "src/io-all.cpp"
#        DEPENDENCIES ev-shared)
#set_target_properties(${PROJECT_NAME}-shared PROPERTIES VERSION ${QB_VERSION} SOVERSION ${QB_VERSION_MAJOR})
#target_include_directories(${PROJECT_NAME}-shared INTERFACE
#        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules>"
#        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

if (WIN32)
#    target_link_libraries(${PROJECT_NAME} ws2_32)
#    target_link_libraries(${PROJECT_NAME}-shared ws2_32)
endif()

if (${QB_PREFIX_UPPER}_BUILD_TEST)
    add_subdirectory(tests)
endif()