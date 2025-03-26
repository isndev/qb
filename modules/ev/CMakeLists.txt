# -----------------------------------------------------------------------------
# libev - Event Library CMake Configuration
#
# This file configures the build system for the libev library, which provides
# a high-performance event loop implementation used by the qb framework.
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.13)
project(libev C)

# -----------------------------------------------------------------------------
# Library versioning
# -----------------------------------------------------------------------------
set(LIBEV_VERSION 4.35)
set(LIBEV_VERSION_CURRENT 4)
set(LIBEV_VERSION_AGE 0)
set(LIBEV_VERSION_REVISION 0)

# -----------------------------------------------------------------------------
# Configuration and build setup
# -----------------------------------------------------------------------------
# Include libtool compatibility macros
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeMacroLibtoolFile.cmake)
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckCSourceCompiles)

# Configure compiler with minimal warnings for this third-party library
config_compiler_with_no_warning()

# Build options
#if(NOT DEFINED BUILD_SHARED_LIBS)
#    option(BUILD_SHARED_LIBS "Build libev as a shared library" ON)
#endif()
if(NOT DEFINED BUILD_STATIC_LIBS)
    option(BUILD_STATIC_LIBS "Build libev as a static library" ON)
endif()
if(NOT DEFINED BUILD_PIC_STATIC_LIBS AND NOT DEFINED CMAKE_POSITION_INDEPENDENT_CODE)
    option(BUILD_PIC_STATIC_LIBS "Build the libev static library position independent" ON)
endif()

# -----------------------------------------------------------------------------
# Feature detection - platform-specific event mechanisms
# -----------------------------------------------------------------------------
# Check for various event notification mechanisms
check_include_file(sys/inotify.h  HAVE_SYS_INOTIFY_H)
check_include_file(sys/epoll.h    HAVE_SYS_EPOLL_H)
check_include_file(sys/event.h    HAVE_SYS_EVENT_H)
check_include_file(port.h         HAVE_PORT_H)
check_include_file(poll.h         HAVE_POLL_H)
check_include_file(sys/select.h   HAVE_SYS_SELECT_H)
check_include_file(sys/eventfd.h  HAVE_SYS_EVENTFD_H)
check_include_file(sys/signalfd.h HAVE_SYS_SIGNALFD_H)

# Check for available notification mechanisms functionality
if(HAVE_SYS_INOTIFY_H)
    check_symbol_exists(inotify_init sys/inotify.h  HAVE_INOTIFY_INIT)
endif()
if(HAVE_SYS_EPOLL_H)
    check_symbol_exists(epoll_ctl    sys/epoll.h    HAVE_EPOLL_CTL)
endif()
if(HAVE_SYS_EVENT_H)
    check_symbol_exists(kqueue       sys/event.h    HAVE_KQUEUE)
endif()
if(HAVE_PORT_H)
    check_symbol_exists(port_create  port.h         HAVE_PORT_CREATE)
endif()
if(HAVE_POLL_H)
    check_symbol_exists(poll         poll.h         HAVE_POLL)
endif()
if(HAVE_SYS_SELECT_H)
    check_symbol_exists(select       sys/select.h   HAVE_SELECT)
endif()
if(HAVE_SYS_EVENTFD_H)
    check_symbol_exists(eventfd      sys/eventfd.h  HAVE_EVENTFD)
endif()
if(HAVE_SYS_SIGNALFD_H)
    check_symbol_exists(signalfd     sys/signalfd.h HAVE_SIGNALFD)
endif()

# Windows-specific configuration
if(WIN32)
    set(HAVE_SELECT 0)
    set(EV_USE_SELECT 0)
    set(HAVE_SYS_EPOLL_H 1)
    set(HAVE_EPOLL_CTL 1)
    set(EV_USE_EPOLL 1)
    set(EV_SELECT_IS_WINSOCKET 0)
endif()

# -----------------------------------------------------------------------------
# Feature detection - timing mechanisms
# -----------------------------------------------------------------------------
# Check for clock_gettime availability
check_symbol_exists(clock_gettime time.h HAVE_CLOCK_GETTIME)
if(NOT HAVE_CLOCK_GETTIME)
    # On Linux, try to use syscall if regular function is not available
    if(CMAKE_SYSTEM_NAME STREQUAL Linux)
        message(STATUS "Checking for clock_gettime syscall")
        check_c_source_compiles("\
            #include <unistd.h>\n\
            #include <sys/syscall.h>\n\
            #include <time.h>\n\
            int main(int argc, char *argv[])\n\
            {\n\
                (void) argc;\n\
                (void) argv;\n\
                struct timespec ts;\n\
                int status = syscall (SYS_clock_gettime, CLOCK_REALTIME, &ts);\n\
                return status;\n\
            }\
            "
                HAVE_CLOCK_SYSCALL
                )
    endif()

    # If still not available, try linking with rt library
    if(NOT AVOID_LIBRT AND NOT HAVE_CLOCK_SYSCALL)
        set(CMAKE_REQUIRED_LIBRARIES rt)
        check_symbol_exists(clock_gettime time.h _HAVE_CLOCK_GETTIME_2)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(_HAVE_CLOCK_GETTIME_2)
            set(LIBRT_NAME rt CACHE STRING "Name of the realtime library to explicitly link in")
            set(HAVE_CLOCK_GETTIME ${_HAVE_CLOCK_GETTIME_2})
            unset(_HAVE_CLOCK_GETTIME_2)
        endif()
    endif()
endif()

# Check for nanosleep availability
check_symbol_exists(nanosleep time.h HAVE_NANOSLEEP)
if(NOT AVOID_LIBRT AND NOT HAVE_NANOSLEEP)
    set(CMAKE_REQUIRED_LIBRARIES rt)
    check_symbol_exists(nanosleep time.h _HAVE_NANOSLEEP_2)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(_HAVE_NANOSLEEP_2)
        set(LIBRT_NAME rt CACHE STRING "Name of the realtime library to explicitly link in")
        set(HAVE_NANOSLEEP ${_HAVE_NANOSLEEP_2})
        unset(_HAVE_NANOSLEEP_2)
    endif()
endif()

# Check for math library functions
check_symbol_exists(floor math.h HAVE_FLOOR)
if(NOT AVOID_LIBM AND NOT HAVE_FLOOR)
    set(CMAKE_REQUIRED_LIBRARIES m)
    check_symbol_exists(floor math.h _HAVE_FLOOR_2)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(_HAVE_FLOOR_2)
        set(LIBM_NAME m CACHE STRING "Name of the math library to explicitly link in")
        set(HAVE_FLOOR ${_HAVE_FLOOR_2})
        unset(_HAVE_FLOOR_2)
    endif()
endif()

# -----------------------------------------------------------------------------
# Generate configuration header
# -----------------------------------------------------------------------------
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmakein
        ${CMAKE_CURRENT_BINARY_DIR}/config.h
        @ONLY
)

# -----------------------------------------------------------------------------
# Documentation (man page) generation
# -----------------------------------------------------------------------------
if(NOT DEFINED SKIP_MAN_PAGE)
    if(NOT POD2MAN_COMMAND)
        message(STATUS "Checking for pod2man")
        find_program(
                POD2MAN_COMMAND
                NAMES pod2man
                DOC "POD data to formatted *roff converter"
        )

        if(POD2MAN_COMMAND STREQUAL POD2MAN_COMMAND-NOTFOUND)
            message(STATUS "Checking for pod2man - not found")
            unset(POD2MAN_COMMAND)
        else()
            message(STATUS "Checking for pod2man - found (${POD2MAN_COMMAND})")
        endif()
    endif()

    # Generate man page if pod2man is available
    if(POD2MAN_COMMAND)
        add_custom_command(
                OUTPUT ev.3
                COMMAND ${POD2MAN_COMMAND}
                -n LIBEV
                -r libev-${LIBEV_VERSION}
                -c "libev - high performance full featured event loop"
                -s3 ${CMAKE_CURRENT_SOURCE_DIR}/ev.pod
                >ev.3
                MAIN_DEPENDENCY ev.pod
                COMMENT "Building the libev man page"
        )
        add_custom_target(man ALL DEPENDS ev.3)

        # Install man page
        install(
                FILES ${CMAKE_CURRENT_BINARY_DIR}/ev.3
                DESTINATION share/man/man3
        )
    endif()
endif()

# -----------------------------------------------------------------------------
# Include paths and source files
# -----------------------------------------------------------------------------
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    include_directories(${CMAKE_CURRENT_BINARY_DIR})
endif()

# Define source files
list(APPEND LIBEV_SRCS
        ev.c
        event.c
        )

# -----------------------------------------------------------------------------
# Library targets
# -----------------------------------------------------------------------------
# Static library build
if(BUILD_STATIC_LIBS)
    add_library(ev STATIC ${LIBEV_SRCS})
    
    # Windows-specific dependencies
    if(WIN32)
        target_link_libraries(ev ws2_32)
    endif()
    
    # Configure library properties
    set_target_properties(ev PROPERTIES
            OUTPUT_NAME ev
            VERSION   ${LIBEV_VERSION_CURRENT}.${LIBEV_VERSION_AGE}.${LIBEV_VERSION_REVISION}
            SOVERSION ${LIBEV_VERSION_CURRENT}
            POSITION_INDEPENDENT_CODE ON
            )
            
    # Enable position-independent code if requested
    if(BUILD_PIC_STATIC_LIBS)
        set_target_properties(ev PROPERTIES
                POSITION_INDEPENDENT_CODE ON
                )
    endif()

    # Mark ev target for installation if QB installation is enabled
    if(${QB_PREFIX_UPPER}_INSTALL)
        install(TARGETS ev
            EXPORT evTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        
        # Install the export file
        install(EXPORT evTargets
            FILE evTargets.cmake
            NAMESPACE qb::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
        )
    endif()
endif()

# Shared library build is commented out in the original file
#if(BUILD_SHARED_LIBS)
#    add_library(ev-shared SHARED ${LIBEV_SRCS})
#    if(WIN32)
#        target_link_libraries(ev-shared ws2_32)
#    endif()
#    set_target_properties(ev-shared PROPERTIES
#            LT_VERSION_CURRENT  ${LIBEV_VERSION_CURRENT}
#            LT_VERSION_AGE      ${LIBEV_VERSION_AGE}
#            LT_VERSION_REVISION ${LIBEV_VERSION_REVISION}
#            VERSION   ${LIBEV_VERSION_CURRENT}.${LIBEV_VERSION_AGE}.${LIBEV_VERSION_REVISION}
#            SOVERSION ${LIBEV_VERSION_CURRENT}
#            )
#
#    if(LIBRT_NAME OR LIBM_NAME)
#        target_link_libraries(ev-shared ${LIBRT_NAME} ${LIBM_NAME})
#    endif()
#
##    create_libtool_file(ev /lib)
#
##    install(TARGETS ev-shared LIBRARY DESTINATION lib)
#endif()

# Installation of headers is commented out in the original file
#if((BUILD_STATIC_LIBS OR BUILD_SHARED_LIBS) AND NOT SKIP_INSTALL_HEADERS)
#    install(
#            FILES ev.h ev++.h event.h
#            DESTINATION include
#    )
#endif()
