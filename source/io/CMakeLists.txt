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

# -----------------------------------------------------------------------------
# qb IO Library - CMake Configuration
#
# This file configures the build system for the qb IO component, which provides
# network I/O, socket handling, and core communication functionality.
# -----------------------------------------------------------------------------

# Define project with same version as parent project
project(${QB_PREFIX}-io 
    LANGUAGES CXX 
    VERSION ${QB_VERSION} 
    DESCRIPTION "I/O and networking library for the qb framework"
)

# -----------------------------------------------------------------------------
# Dependencies and compile definitions configuration
# -----------------------------------------------------------------------------
# Core dependencies required by the IO library
set(CURRENT_DEPENDENCIES ev stduuid)
set(CURRENT_DEFINITIONS QB_IO_VERSION=${QB_VERSION})
set(CURRENT_INCLUDES)

# -----------------------------------------------------------------------------
# Library source files
# -----------------------------------------------------------------------------
set(IO_SOURCES
    src/io.cpp
)

# -----------------------------------------------------------------------------
# Logging configuration
# -----------------------------------------------------------------------------
# Configure logging based on parent project settings
if (${QB_PREFIX_UPPER}_WITH_LOG)
    list(APPEND CURRENT_DEFINITIONS QB_LOGGER=1)
    message(STATUS "[${PROJECT_NAME}] Building with logger support")
else ()
    message(STATUS "[${PROJECT_NAME}] Building without logger support")
    if (${QB_PREFIX_UPPER}_STDOUT_LOG)
        list(APPEND CURRENT_DEFINITIONS QB_STDOUT_LOG=1)
        message(STATUS "[${PROJECT_NAME}] Enabling stdout logging")
    endif ()
endif ()

# -----------------------------------------------------------------------------
# Platform-specific dependencies
# -----------------------------------------------------------------------------
# Windows requires the winsock library
if (WIN32)
    list(APPEND CURRENT_DEPENDENCIES ws2_32)
    message(STATUS "[${PROJECT_NAME}] Adding Windows-specific dependency: ws2_32")
endif ()

# -----------------------------------------------------------------------------
# Optional compression support (ZLIB)
# -----------------------------------------------------------------------------
if (ZLIB_FOUND)
    list(APPEND CURRENT_DEPENDENCIES ZLIB::ZLIB)
    list(APPEND CURRENT_DEFINITIONS QB_IO_WITH_ZLIB=1)
    
    # Add ZLIB include directories if not using imported targets
    if(NOT TARGET ZLIB::ZLIB)
        list(APPEND CURRENT_INCLUDES ${ZLIB_INCLUDE_DIRS})
    endif()
    
    message(STATUS "[${PROJECT_NAME}] Building with ZLIB compression support")
endif ()

# -----------------------------------------------------------------------------
# Optional SSL/TLS support (OpenSSL)
# -----------------------------------------------------------------------------
if (OPENSSL_FOUND)
    list(APPEND CURRENT_DEPENDENCIES OpenSSL::SSL)
    list(APPEND CURRENT_DEFINITIONS QB_IO_WITH_SSL=1)
    
    # Add OpenSSL include directories if not using imported targets
    if(NOT TARGET OpenSSL::SSL)
        list(APPEND CURRENT_INCLUDES ${OPENSSL_INCLUDE_DIR})
    endif()
    
    # Option for Argon2 (can be disabled)
    option(QB_WITH_ARGON2 "Enable Argon2 support for password hashing" ON)
    
    # Add Argon2 library for advanced cryptographic functionality
    if(QB_WITH_ARGON2)
        find_library(ARGON2_LIBRARY NAMES argon2 libargon2)
        if(ARGON2_LIBRARY)
            list(APPEND CURRENT_DEPENDENCIES ${ARGON2_LIBRARY})
            list(APPEND CURRENT_DEFINITIONS QB_IO_WITH_ARGON2=1)
            message(STATUS "[${PROJECT_NAME}] Building with Argon2 support for advanced crypto")
        else()
            message(WARNING "[${PROJECT_NAME}] Argon2 library not found - advanced crypto functions will use fallback methods")
        endif()
    else()
        message(STATUS "[${PROJECT_NAME}] Argon2 support disabled - advanced crypto functions will use fallback methods")
    endif()
    
    # Add nlohmann_json for metadata functions
    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND)
        list(APPEND CURRENT_DEPENDENCIES nlohmann_json::nlohmann_json)
        message(STATUS "[${PROJECT_NAME}] Building with nlohmann_json support")
    else()
        message(WARNING "[${PROJECT_NAME}] nlohmann_json library not found - metadata functions may not work")
    endif()
    
    message(STATUS "[${PROJECT_NAME}] Building with OpenSSL support")
endif ()

# Add any additional include directories if specified
if (CURRENT_INCLUDES)
    include_directories(${CURRENT_INCLUDES})
endif ()

# -----------------------------------------------------------------------------
# Library target configuration
# -----------------------------------------------------------------------------
# Define the IO library using the custom cxx_library macro from parent project
cxx_library(
    NAME ${PROJECT_NAME}
    FLAGS ${cxx_default_lib}
    SOURCES ${IO_SOURCES}
    DEPENDENCIES ${CURRENT_DEPENDENCIES}
)

# Configure library properties and interface includes
set_target_properties(${PROJECT_NAME} PROPERTIES 
    VERSION ${QB_VERSION}
    SOVERSION ${QB_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Apply public compile definitions
foreach (def ${CURRENT_DEFINITIONS})
    target_compile_definitions(${PROJECT_NAME} PUBLIC ${def})
endforeach ()

# Setup include directories for both build and install interfaces
target_include_directories(${PROJECT_NAME} 
    PUBLIC
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include>"
    PRIVATE
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/source/io/include>"
    INTERFACE
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# Add module directories to the include path
target_include_directories(${PROJECT_NAME} 
    PUBLIC "$<BUILD_INTERFACE:${QB_DIRECTORY}/modules>"
)

# -----------------------------------------------------------------------------
# Installation configuration
# -----------------------------------------------------------------------------
if(${QB_PREFIX_UPPER}_INSTALL)
    include(GNUInstallDirs)
    
    install(TARGETS ${PROJECT_NAME}
        EXPORT ${PROJECT_NAME}Targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    
    # Generate and install export file
    install(EXPORT ${PROJECT_NAME}Targets
        FILE ${PROJECT_NAME}Targets.cmake
        NAMESPACE qb::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
    )
endif()

# -----------------------------------------------------------------------------
# Tests configuration
# -----------------------------------------------------------------------------
# Add tests if enabled in the parent project
if (${QB_PREFIX_UPPER}_BUILD_TEST)
    message(STATUS "[${PROJECT_NAME}] Building tests")
    add_subdirectory(tests)
endif ()
