cmake_minimum_required(VERSION 3.13)
project(libev C)

# ----------------------------------------------------------------------------
# Versioning
# ----------------------------------------------------------------------------
set(LIBEV_VERSION 4.35)
set(LIBEV_VERSION_CURRENT 4)
set(LIBEV_VERSION_AGE 0)
set(LIBEV_VERSION_REVISION 0)

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
option(BUILD_STATIC_LIBS "Build libev as a static library" ON)
option(BUILD_PIC_STATIC_LIBS "Build static lib as PIC" OFF)

# ----------------------------------------------------------------------------
# Feature Detection
# ----------------------------------------------------------------------------
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckCSourceCompiles)

# ------------------------------
# Header Checks
# ------------------------------
check_include_file(dlfcn.h           HAVE_DLFCN_H)
check_include_file(inttypes.h        HAVE_INTTYPES_H)
check_include_file(linux/aio_abi.h   HAVE_LINUX_AIO_ABI_H)
check_include_file(linux/fs.h        HAVE_LINUX_FS_H)
check_include_file(memory.h          HAVE_MEMORY_H)
check_include_file(poll.h            HAVE_POLL_H)
check_include_file(port.h            HAVE_PORT_H)
check_include_file(signal.h          HAVE_SIGNAL_H)
check_include_file(stdint.h          HAVE_STDINT_H)
check_include_file(stdlib.h          HAVE_STDLIB_H)
check_include_file(strings.h         HAVE_STRINGS_H)
check_include_file(string.h          HAVE_STRING_H)
check_include_file(sys/epoll.h       HAVE_SYS_EPOLL_H)
check_include_file(sys/event.h       HAVE_SYS_EVENT_H)
check_include_file(sys/eventfd.h     HAVE_SYS_EVENTFD_H)
check_include_file(sys/inotify.h     HAVE_SYS_INOTIFY_H)
check_include_file(sys/select.h      HAVE_SYS_SELECT_H)
check_include_file(sys/signalfd.h    HAVE_SYS_SIGNALFD_H)
check_include_file(sys/stat.h        HAVE_SYS_STAT_H)
check_include_file(sys/timerfd.h     HAVE_SYS_TIMERFD_H)
check_include_file(sys/types.h       HAVE_SYS_TYPES_H)
check_include_file(unistd.h          HAVE_UNISTD_H)

# ------------------------------
# Function/Symbol Checks
# ------------------------------
check_symbol_exists(clock_gettime    time.h              HAVE_CLOCK_GETTIME)
check_symbol_exists(nanosleep        time.h              HAVE_NANOSLEEP)
check_symbol_exists(select           sys/select.h        HAVE_SELECT)
check_symbol_exists(poll             poll.h              HAVE_POLL)
check_symbol_exists(epoll_ctl        sys/epoll.h         HAVE_EPOLL_CTL)
check_symbol_exists(eventfd          sys/eventfd.h       HAVE_EVENTFD)
check_symbol_exists(signalfd         sys/signalfd.h      HAVE_SIGNALFD)
check_symbol_exists(inotify_init     sys/inotify.h       HAVE_INOTIFY_INIT)
check_symbol_exists(port_create      port.h              HAVE_PORT_CREATE)
check_symbol_exists(kqueue           sys/event.h         HAVE_KQUEUE)
check_symbol_exists(floor            math.h              HAVE_FLOOR)

# ------------------------------
# Kernel Types / Special Syscalls
# ------------------------------
check_c_source_compiles("
  #include <time.h>
  #include <unistd.h>
  #include <sys/syscall.h>
  int main() {
    struct timespec ts;
    return syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
  }
" HAVE_CLOCK_SYSCALL)

check_c_source_compiles("
  #include <linux/fs.h>
  int main() {
    int x = RWF_DSYNC;
    return x;
  }
" HAVE_KERNEL_RWF_T)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Reset all backends
set(EV_USE_EPOLL 0)
set(EV_USE_KQUEUE 0)
set(EV_USE_IOURING 0)
set(EV_USE_WSAPOLL 0)

# Detect backend support based on platform
if(WIN32)
    set(HAVE_EPOLL_CTL 1)
    set(HAVE_SYS_EPOLL_H 1)
    set(EV_USE_EPOLL 1)  # We use wepoll on Windows as epoll backend
    list(APPEND PLATFORM_LIBS ws2_32)
elseif(APPLE)
    check_include_file(sys/event.h HAVE_SYS_EVENT_H)
    check_symbol_exists(kqueue sys/event.h HAVE_KQUEUE)
    if(HAVE_KQUEUE)
        set(EV_USE_KQUEUE 1)
    else()
        message(FATAL_ERROR "kqueue is required on macOS")
    endif()
elseif(UNIX)
    # Check for io_uring
    check_include_file(linux/io_uring.h HAVE_LINUX_IO_URING_H)
    if(HAVE_LINUX_IO_URING_H)
    check_c_source_compiles("
        #include <unistd.h>
        #include <sys/syscall.h>
        #include <linux/io_uring.h>
        int main() {
            struct io_uring_params p = {0};
            int fd = syscall(SYS_io_uring_setup, 1, &p);
            return fd >= 0 ? 0 : 1;
        }" HAVE_IO_URING_SYSCALL)
    endif()
    set(EV_USE_EPOLL 1)
    # if(HAVE_LINUX_IO_URING_H AND HAVE_IO_URING_SYSCALL)
    #     message(STATUS "io_uring detected and supported")
    #     set(EV_USE_IOURING 1)
    # else()
    #     message(STATUS "Falling back to epoll (no usable io_uring)")
    # endif()
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# ----------------------------------------------------------------------------
# Generate configuration header (public)
# ----------------------------------------------------------------------------
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmakein
  ${CMAKE_CURRENT_BINARY_DIR}/ev_config.h
  @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ev_config.h DESTINATION include)

# ----------------------------------------------------------------------------
# Build library
# ----------------------------------------------------------------------------
set(LIBEV_SRCS ev.c event.c)

if(BUILD_STATIC_LIBS)
  add_library(ev STATIC ${LIBEV_SRCS})

#   target_compile_definitions(ev PRIVATE
#     EV_USE_EPOLL=${EV_USE_EPOLL}
#     EV_USE_KQUEUE=${EV_USE_KQUEUE}
#     EV_USE_IOURING=${EV_USE_IOURING}
#     EV_USE_WSAPOLL=${EV_USE_WSAPOLL}
#     EV_MULTIPLICITY=1
#     EV_FEATURES=0x7f
#   )

  target_include_directories(ev
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )

  target_link_libraries(ev PUBLIC ${PLATFORM_LIBS})

  set_target_properties(ev PROPERTIES
    OUTPUT_NAME ev
    POSITION_INDEPENDENT_CODE ${BUILD_PIC_STATIC_LIBS}
    VERSION ${LIBEV_VERSION_CURRENT}.${LIBEV_VERSION_AGE}.${LIBEV_VERSION_REVISION}
    SOVERSION ${LIBEV_VERSION_CURRENT}
  )

  install(TARGETS ev
    EXPORT evTargets
    ARCHIVE DESTINATION lib
    INCLUDES DESTINATION include
  )

  install(FILES ev.h DESTINATION include)

  install(EXPORT evTargets
    FILE evTargets.cmake
    NAMESPACE ev::
    DESTINATION lib/cmake/ev
  )
endif()
