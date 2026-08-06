cmake_minimum_required(VERSION 3.22)
project(qev
        VERSION 5.0.0
        DESCRIPTION "A modernized, cross-platform fork of libev (qev_* API, NOT drop-in)"
        HOMEPAGE_URL "https://github.com/isndev/qb-ev"
        LANGUAGES C)

# Detect whether we are the top-level project (standalone) or consumed as a
# subdirectory (e.g. by qb-io via add_subdirectory). Used to gate standalone-only
# conveniences without changing behaviour for embedding projects.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(QB_EV_STANDALONE ON)
else ()
    set(QB_EV_STANDALONE OFF)
endif ()

# ----------------------------------------------------------------------------
# Header namespace
# ----------------------------------------------------------------------------
# An embedding project sets QB_VENDOR_INCLUDE_PREFIX (qb sets it to "qb/vendor") to declare that
# this fork's headers are reached through ITS include root under that prefix, i.e.
# <qb/vendor/qev/qev++.h>. In that mode this project publishes NO include directory of its own for
# its headers and installs NO loose qev.h / qev++.h / event.h / event_compat.h: dropping those four
# generic names into a consumer's include root is exactly the shadowing this prefix exists to stop
# (a consumer with its own event.h had qb's silently win).
#
# Unset/empty (standalone qev, or add_subdirectory() by a project that did not opt in) keeps the
# historical layout byte-for-byte: headers at the include root, reached as <qev.h> / <qev++.h>.
if (DEFINED QB_VENDOR_INCLUDE_PREFIX AND NOT QB_VENDOR_INCLUDE_PREFIX STREQUAL "")
    set(QB_EV_HEADER_DIR "${QB_VENDOR_INCLUDE_PREFIX}/qev")
    set(QB_EV_EMBEDDED ON)
else ()
    set(QB_EV_HEADER_DIR "")
    set(QB_EV_EMBEDDED OFF)
endif ()

# ----------------------------------------------------------------------------
# Versioning (keep in sync with qev.h EV_VERSION_MAJOR / EV_VERSION_MINOR)
# ----------------------------------------------------------------------------
set(LIBEV_VERSION ${PROJECT_VERSION})
set(LIBEV_VERSION_CURRENT ${PROJECT_VERSION_MAJOR})
set(LIBEV_VERSION_AGE 0)
set(LIBEV_VERSION_REVISION 0)

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
option(BUILD_STATIC_LIBS "Build qev as a static library" ON)
# Default ON: the bundled qev is consumed by qb-io, which may itself be built as a
# shared library (QB_BUILD_SHARED_LIBS=ON). A non-PIC static qev cannot be linked
# into a shared object on Linux. This also keeps the explicit target property
# below from overriding the project-wide CMAKE_POSITION_INDEPENDENT_CODE.
option(BUILD_PIC_STATIC_LIBS "Build static lib as PIC" ON)

# Library kind. When embedded (e.g. by qb-io via add_subdirectory) the `qev` target
# MUST be static: qb-io DEPENDS on it and links it directly. When built standalone
# we honour the conventional BUILD_SHARED_LIBS so packagers can produce a shared
# libqev. (BUILD_STATIC_LIBS is retained for backward compatibility.)
if (QB_EV_STANDALONE AND BUILD_SHARED_LIBS)
    set(EV_LIBRARY_TYPE SHARED)
else ()
    set(EV_LIBRARY_TYPE STATIC)
    # Force the static target to exist even if a parent set BUILD_STATIC_LIBS=OFF.
    set(BUILD_STATIC_LIBS ON)
endif ()
# Default OFF: matches legacy qev_config.h (#define EV_USE_TIMERFD 0). Enabling makes
# qev_run use MAX_BLOCKTIME2 when timerfd is active and timercnt==0, which stalls
# epoll_wait (regression #1 on event I/O tests). Opt in with -D QB_EV_USE_TIMERFD=ON.
option(QB_EV_USE_TIMERFD "Enable qev timerfd (time-jump detection; affects idle waittime)" OFF)
option(QB_EV_STRICT_WARNINGS "Build bundled qev with strict compiler warnings" ON)
# Default OFF, and that default is a fix, not a taste. event.c is libev's *libevent
# compatibility* layer, and it exports 24 upstream libevent C symbols under their real,
# unprefixed names -- event_base_new, event_add, event_del, event_dispatch, event_set, ... The
# ev->qev rename covered libev's own API (58 qev_*, 0 ev_*) and stopped there, so those 24 were
# still in the installed libqev.a, which sits on EVERY consumer's link line (qb::io names
# qb::qev in its INTERFACE_LINK_LIBRARIES). A consumer that also links the real libevent then
# got whichever implementation the archive order happened to pick, with two unrelated
# `struct event_base` layouts and no diagnostic from anyone. Measured, against a stand-in for
# libevent:
#     ORDER 1  -lfakeevent then libqev.a  -> real libevent ran
#     ORDER 2  libqev.a then -lfakeevent  -> qb's fork ran,  rc=0, silently
# Nothing in qb calls any of them (`nm -u libqb-io.a | grep -c '^ *_event_'` -> 0), so OFF costs
# qb nothing and makes the collision impossible rather than merely unlikely. The layer is kept,
# not deleted: turn this ON to build and install it, and own the 24 names deliberately.
option(QB_EV_LIBEVENT_COMPAT
       "Build and install libev's libevent compatibility layer (exports 24 unprefixed libevent C symbols)"
       OFF)
option(QB_EV_WARNING_AUDIT "Enable extra noisy qev warning diagnostics for modernization audits" OFF)

# The audit diagnostics layer on top of the strict baseline (the strict block
# below is what applies them). Enabling the audit without the baseline would
# silently apply no warnings at all, so make the dependency explicit.
if (QB_EV_WARNING_AUDIT AND NOT QB_EV_STRICT_WARNINGS)
    message(STATUS "[qb-ev] QB_EV_WARNING_AUDIT=ON forces QB_EV_STRICT_WARNINGS=ON")
    set(QB_EV_STRICT_WARNINGS ON)
endif ()

# ----------------------------------------------------------------------------
# Feature Detection
# ----------------------------------------------------------------------------
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckCSourceCompiles)

# ------------------------------
# Header Checks
# ------------------------------
check_include_file(dlfcn.h HAVE_DLFCN_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(linux/aio_abi.h HAVE_LINUX_AIO_ABI_H)
check_include_file(linux/fs.h HAVE_LINUX_FS_H)
check_include_file(memory.h HAVE_MEMORY_H)
check_include_file(poll.h HAVE_POLL_H)
check_include_file(port.h HAVE_PORT_H)
check_include_file(signal.h HAVE_SIGNAL_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(stdlib.h HAVE_STDLIB_H)
check_include_file(strings.h HAVE_STRINGS_H)
check_include_file(string.h HAVE_STRING_H)
check_include_file(sys/epoll.h HAVE_SYS_EPOLL_H)
check_include_file(sys/event.h HAVE_SYS_EVENT_H)
check_include_file(sys/eventfd.h HAVE_SYS_EVENTFD_H)
check_include_file(sys/inotify.h HAVE_SYS_INOTIFY_H)
check_include_file(sys/select.h HAVE_SYS_SELECT_H)
check_include_file(sys/signalfd.h HAVE_SYS_SIGNALFD_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)
check_include_file(sys/timerfd.h HAVE_SYS_TIMERFD_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(unistd.h HAVE_UNISTD_H)

# ------------------------------
# Function/Symbol Checks
# ------------------------------
check_symbol_exists(clock_gettime time.h HAVE_CLOCK_GETTIME)
check_symbol_exists(nanosleep time.h HAVE_NANOSLEEP)
check_symbol_exists(select sys/select.h HAVE_SELECT)
check_symbol_exists(poll poll.h HAVE_POLL)
check_symbol_exists(epoll_ctl sys/epoll.h HAVE_EPOLL_CTL)
check_symbol_exists(eventfd sys/eventfd.h HAVE_EVENTFD)
check_symbol_exists(signalfd sys/signalfd.h HAVE_SIGNALFD)
check_symbol_exists(inotify_init sys/inotify.h HAVE_INOTIFY_INIT)
check_symbol_exists(port_create port.h HAVE_PORT_CREATE)
check_symbol_exists(kqueue sys/event.h HAVE_KQUEUE)
check_symbol_exists(floor math.h HAVE_FLOOR)

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

# Reset all backends/helpers
set(EV_USE_SELECT 0)
set(EV_USE_POLL 0)
set(EV_USE_EPOLL 0)
set(EV_USE_KQUEUE 0)
set(EV_USE_PORT 0)
set(EV_USE_LINUXAIO 0)
set(EV_USE_IOURING 0)
set(EV_USE_WSAPOLL 0)
set(EV_USE_INOTIFY 0)
set(EV_USE_EVENTFD 0)
set(EV_USE_SIGNALFD 0)
set(EV_USE_TIMERFD 0)

# Detect backend support based on platform
if (WIN32)
    set(HAVE_EPOLL_CTL 1)
    set(HAVE_SYS_EPOLL_H 1)
    set(EV_USE_EPOLL 1)  # We use wepoll on Windows as epoll backend
    set(EV_USE_SELECT 1) # Keep a low-performance fallback available for forced/debug loops.
    list(APPEND PLATFORM_LIBS ws2_32)
elseif (APPLE)
    if (HAVE_KQUEUE)
        set(EV_USE_KQUEUE 1)
    endif ()
    if (HAVE_SELECT AND HAVE_SYS_SELECT_H)
        set(EV_USE_SELECT 1)
    endif ()
    if (HAVE_POLL AND HAVE_POLL_H)
        set(EV_USE_POLL 1)
    endif ()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (HAVE_SELECT AND HAVE_SYS_SELECT_H)
        set(EV_USE_SELECT 1)
    endif ()
    if (HAVE_POLL AND HAVE_POLL_H)
        set(EV_USE_POLL 1)
    endif ()
    if (HAVE_EPOLL_CTL AND HAVE_SYS_EPOLL_H)
        set(EV_USE_EPOLL 1)
    endif ()
    if (HAVE_LINUX_AIO_ABI_H AND HAVE_LINUX_FS_H AND HAVE_EPOLL_CTL AND HAVE_SYS_EPOLL_H)
        set(EV_USE_LINUXAIO 1)
    endif ()
    if (HAVE_INOTIFY_INIT AND HAVE_SYS_INOTIFY_H)
        set(EV_USE_INOTIFY 1)
    endif ()
    if (HAVE_EVENTFD AND HAVE_SYS_EVENTFD_H)
        set(EV_USE_EVENTFD 1)
    endif ()
    if (HAVE_SIGNALFD AND HAVE_SYS_SIGNALFD_H)
        set(EV_USE_SIGNALFD 1)
    endif ()
    if (QB_EV_USE_TIMERFD AND HAVE_SYS_TIMERFD_H)
        set(EV_USE_TIMERFD 1)
    endif ()
    # Check for io_uring
    check_include_file(linux/io_uring.h HAVE_LINUX_IO_URING_H)
    if (HAVE_LINUX_IO_URING_H)
        check_c_source_compiles("
        #include <unistd.h>
        #include <sys/syscall.h>
        #include <linux/io_uring.h>
        int main() {
            struct io_uring_params p = {0};
            int fd = syscall(SYS_io_uring_setup, 1, &p);
            return fd >= 0 ? 0 : 1;
        }" HAVE_IO_URING_SYSCALL)
    endif ()
    if (HAVE_LINUX_IO_URING_H AND HAVE_IO_URING_SYSCALL AND EV_USE_EPOLL)
        set(EV_USE_IOURING 1)
    endif ()
elseif (UNIX)
    if (HAVE_SELECT AND HAVE_SYS_SELECT_H)
        set(EV_USE_SELECT 1)
    endif ()
    if (HAVE_POLL AND HAVE_POLL_H)
        set(EV_USE_POLL 1)
    endif ()
    if (HAVE_PORT_CREATE AND HAVE_PORT_H)
        set(EV_USE_PORT 1)
    endif ()
    if (HAVE_KQUEUE AND HAVE_SYS_EVENT_H)
        set(EV_USE_KQUEUE 1)
    endif ()
else ()
    message(FATAL_ERROR "Unsupported platform")
endif ()

# ----------------------------------------------------------------------------
# Generate configuration header (public)
# ----------------------------------------------------------------------------
# Generated under a dedicated `include/` root mirroring the header namespace, so the ONE directory
# this project puts on its consumers' include path contains nothing but ${QB_EV_HEADER_DIR}/.
#
# qev.h and qev.c look for this file in three ways (qev.h:24-32, qev.c:23-30):
#     -DEV_CONFIG_H=<path>  >  HAVE_CONFIG_H + "config.h"  >  __has_include("qev_config.h")
# The last one is quote-relative to qev.h: it only works when qev_config.h sits NEXT TO qev.h --
# true in an installed tree, never true in a build tree (generated file, source-tree header). And
# because it is guarded by __has_include, a miss is SILENT: qev.h falls back to its own defaults and
# quietly disagrees with the compiled libqev.a (measured: EV_MULTIPLICITY 4 instead of 1). So the
# define below is what actually carries the configuration, and it is deliberately PUBLIC: it must
# reach every translation unit that includes qev.h, not just this library's own sources.
if (QB_EV_EMBEDDED)
    set(QB_EV_CONFIG_INCLUDE "<${QB_EV_HEADER_DIR}/qev_config.h>")
    set(QB_EV_CONFIG_OUT "${CMAKE_CURRENT_BINARY_DIR}/include/${QB_EV_HEADER_DIR}/qev_config.h")
else ()
    set(QB_EV_CONFIG_INCLUDE "<qev_config.h>")
    set(QB_EV_CONFIG_OUT "${CMAKE_CURRENT_BINARY_DIR}/include/qev_config.h")
endif ()

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmakein
        ${QB_EV_CONFIG_OUT}
        @ONLY
)

# Embedded: the parent project's install(DIRECTORY include/) does NOT cover this file (it is
# generated into the build tree), so it still needs its own rule -- at the namespaced destination,
# next to the qev.h that will look for it.
#
# Gated on QB_INSTALL when qb is the embedder, because this rule was the ONE hole in that gate.
# It is the only install() in the entire embedded-qb subtree that sat outside the guard, so a
# `cmake --install` of a QB_INSTALL=OFF build -- which is every dev preset, and every parent that
# reaches qb via add_subdirectory()/FetchContent -- silently deposited exactly one qb header,
# include/qb/vendor/qev/qev_config.h, into the CONSUMER's prefix. Exit 0, no diagnostic. That is
# precisely the leak the QB_INSTALL option exists to prevent (see cmake/qbConfig.cmake), and this
# file is the host-probed generated header, so it is also the worst single file to leak.
# QB_INSTALL is undefined for a standalone qev build; default it ON there so the fork keeps
# behaving on its own.
include(GNUInstallDirs)
if (NOT DEFINED QB_INSTALL)
    set(QB_INSTALL ON)
endif ()
if (QB_INSTALL)
    if (QB_EV_EMBEDDED)
        install(FILES ${QB_EV_CONFIG_OUT}
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${QB_EV_HEADER_DIR})
    else ()
        install(FILES ${QB_EV_CONFIG_OUT} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif ()
endif ()

# ----------------------------------------------------------------------------
# Build library
# ----------------------------------------------------------------------------
set(LIBEV_SRCS qev.c)
if (QB_EV_LIBEVENT_COMPAT)
    list(APPEND LIBEV_SRCS event.c)
endif ()

if (BUILD_STATIC_LIBS OR BUILD_SHARED_LIBS)
    add_library(qev ${EV_LIBRARY_TYPE} ${LIBEV_SRCS})

    #   target_compile_definitions(qev PRIVATE
    #     EV_USE_EPOLL=${EV_USE_EPOLL}
    #     EV_USE_KQUEUE=${EV_USE_KQUEUE}
    #     EV_USE_IOURING=${EV_USE_IOURING}
    #     EV_USE_WSAPOLL=${EV_USE_WSAPOLL}
    #     EV_MULTIPLICITY=1
    #     EV_FEATURES=0x7f
    #   )

    # The generated-config root is PUBLIC in both modes: qev.h needs it wherever it is included.
    target_include_directories(qev
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_definitions(qev PUBLIC EV_CONFIG_H=${QB_EV_CONFIG_INCLUDE})
    # PRIVATE, and that is the point: it is what unlocks the HAVE_* half of the generated config
    # for qev.c only. Those 35 unprefixed autoconf names are qb's build-host answers, and the
    # config header is PUBLIC, so without this gate they reached -- and could silently override --
    # every consumer translation unit that includes <qb/io.h>. See config.h.cmakein.
    target_compile_definitions(qev PRIVATE QEV_BUILDING_LIBRARY=1)

    if (QB_EV_EMBEDDED)
        # PRIVATE, not PUBLIC: this directory holds qev.h, qev++.h, event.h, event_compat.h,
        # qev_vars.h, qev_wrap.h and wepoll.h. Exporting it PUBLIC is what put all seven bare names
        # on the include path of every target that links qev -- transitively, every consumer of
        # qb::io. Embedded consumers reach them through the parent's include root instead
        # (<${QB_EV_HEADER_DIR}/qev++.h>); qev's own .c files use quote-relative includes and need
        # nothing but this private entry.
        target_include_directories(qev PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
    else ()
        target_include_directories(qev PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
    endif ()

    if (QB_EV_STRICT_WARNINGS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        # Keep the vendored event loop visible to modern compilers. The baseline is
        # intentionally warning-clean enough for day-to-day builds; the audit option
        # below enables noisier conversion/cast diagnostics for dedicated cleanups.
        target_compile_options(qev PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wformat=2
                -Wundef
                -Wshadow
                -Wpointer-arith
                -Wcast-align
                -Wstrict-prototypes
                -Wmissing-prototypes
                -Wmissing-declarations
                -Wvla
        )

        if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
            target_compile_options(qev PRIVATE
                    -Walloc-zero
                    -Wduplicated-branches
                    -Wduplicated-cond
                    -Wlogical-op
                    -Wnull-dereference
                    -Wstrict-overflow=2
            )
        elseif (CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
            target_compile_options(qev PRIVATE
                    -Wconditional-uninitialized
                    -Wextra-semi
                    -Wnewline-eof
                    -Wunreachable-code
            )
        endif ()

        if (QB_EV_WARNING_AUDIT)
            target_compile_options(qev PRIVATE
                    -Wcast-qual
                    -Wconversion
                    -Wsign-conversion
            )
        endif ()
    endif ()

    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang" AND NOT QB_EV_WARNING_AUDIT)
        target_compile_options(qev PRIVATE
                -Wno-unused-parameter
                -Wno-unused-function
                -Wno-unused-result
        )
    endif ()

    target_link_libraries(qev PUBLIC ${PLATFORM_LIBS})

    set_target_properties(qev PROPERTIES
            OUTPUT_NAME qev
            POSITION_INDEPENDENT_CODE ${BUILD_PIC_STATIC_LIBS}
            VERSION ${LIBEV_VERSION_CURRENT}.${LIBEV_VERSION_AGE}.${LIBEV_VERSION_REVISION}
            SOVERSION ${LIBEV_VERSION_CURRENT}
            WINDOWS_EXPORT_ALL_SYMBOLS ON
    )

    # Namespaced aliases: consumers link `qb::qev` (brand) or `qev::qev` (standalone).
    add_library(qb::qev ALIAS qev)
    if (NOT TARGET qev::qev)
        add_library(qev::qev ALIAS qev)
    endif ()

    # --------------------------------------------------------------------------
    # Install + package config — find_package(qev) (and the qev::qev alias)
    # --------------------------------------------------------------------------
    # Skipped entirely when embedded: the parent owns the install. It installs the `qev` target in
    # its OWN export set and ships these headers through its own install(DIRECTORY include/), at
    # the namespaced path. Running this block as well would (a) re-install qev.h/qev++.h/event.h/
    # event_compat.h at the include ROOT, undoing the namespacing, and (b) drop a
    # lib/cmake/qev/ package next to the parent's own, exporting a second, differently-named
    # imported target for the same static library.
    if (NOT QB_EV_EMBEDDED)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS qev
            EXPORT qevTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    set(_qev_install_headers qev.h qev++.h)
    if (QB_EV_LIBEVENT_COMPAT)
        # Only shipped with the implementation. Installing the declarations without the bodies
        # would turn a use into an undefined reference -- loud, but pointlessly so; installing
        # them next to a *real* libevent is worse, because the two declare the same names over
        # incompatible struct layouts.
        list(APPEND _qev_install_headers event.h event_compat.h)
    endif ()
    install(FILES ${_qev_install_headers}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT qevTargets
            FILE qevTargets.cmake
            NAMESPACE qb::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qev
    )

    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qevConfig.cmake.in
            "@PACKAGE_INIT@
include(\"\${CMAKE_CURRENT_LIST_DIR}/qevTargets.cmake\")
# Unprefixed alias for projects that link the fork under its own namespace.
if(NOT TARGET qev::qev)
  add_library(qev::qev ALIAS qb::qev)
endif()
")
    configure_package_config_file(
            ${CMAKE_CURRENT_BINARY_DIR}/qevConfig.cmake.in
            ${CMAKE_CURRENT_BINARY_DIR}/qevConfig.cmake
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qev
    )
    write_basic_package_version_file(
            ${CMAKE_CURRENT_BINARY_DIR}/qevConfigVersion.cmake
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
    )
    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/qevConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/qevConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qev
    )
    endif () # NOT QB_EV_EMBEDDED
endif ()
