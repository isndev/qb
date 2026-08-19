cmake_minimum_required(VERSION 3.22)
project(qev
        VERSION 5.0.0
        DESCRIPTION "A modernized, cross-platform fork of libev (source-compatible ev_* API)"
        HOMEPAGE_URL "https://github.com/isndev/qev"
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
# An embedding project sets QB_EV_EMBED_INCLUDE_DIR to name the directory ITS include root exposes
# these headers under -- qb sets it to "qb/ev", so a qb consumer writes <qb/ev/ev++.h>. In that
# mode this project publishes NO include directory of its own for its headers and installs NO loose
# ev.h / ev++.h / event.h / event_compat.h: dropping those four generic names into a consumer's
# include root is exactly the shadowing this variable exists to stop (a consumer with its own
# event.h had qb's silently win).
#
# Unset/empty is the STANDALONE build -- the isndev/qev repo itself, or add_subdirectory() by a
# project that did not opt in. There the headers install under include/qev/, never loose at the
# include root: `include/ev.h` and `include/event.h` at a prefix root are how this fork used to
# collide with a real libev and a real libevent installed beside it.
#
# This file is byte-shared with the standalone repo (dev/agent/check-qev-identity.py asserts it),
# which is why the variable is spelled after this project and not after its embedder.
if (DEFINED QB_EV_EMBED_INCLUDE_DIR AND NOT QB_EV_EMBED_INCLUDE_DIR STREQUAL "")
    set(QB_EV_HEADER_DIR "${QB_EV_EMBED_INCLUDE_DIR}")
    set(QB_EV_EMBEDDED ON)
else ()
    # "qev", NOT "qb/ev". Sharing qb's include namespace was tried and is WRONG: the two builds
    # are not the same library. qb compiles the reduced watcher profile and the standalone compiles
    # all fourteen, so `ev_config.h` differs (EV_IDLE/ASYNC/EMBED_ENABLE 0 vs 1) and with it the
    # layout of `struct ev_loop` -- 1224 bytes against 1520. Installing both into one prefix would
    # have let the second overwrite the first's header while `libqb-io.a`, compiled against the
    # other layout, kept linking. Same-named files with different content is the collision class
    # this whole fork exists to close; it does not get an exception for being ours.
    set(QB_EV_HEADER_DIR "qev")
    set(QB_EV_EMBEDDED OFF)
endif ()

# ----------------------------------------------------------------------------
# Versioning (keep in sync with ev.h EV_VERSION_MAJOR / EV_VERSION_MINOR)
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
# Default OFF: matches legacy ev_config.h (#define EV_USE_TIMERFD 0). Enabling makes
# ev_run use MAX_BLOCKTIME2 when timerfd is active and timercnt==0, which stalls
# epoll_wait (regression #1 on event I/O tests). Opt in with -D QB_EV_USE_TIMERFD=ON.
option(QB_EV_USE_TIMERFD "Enable qev timerfd (time-jump detection; affects idle waittime)" OFF)
option(QB_EV_STRICT_WARNINGS "Build bundled qev with strict compiler warnings" ON)
# Default OFF, and that default is a fix, not a taste. event.c is libev's *libevent
# compatibility* layer, and it exports 24 upstream libevent C symbols under their real,
# unprefixed names -- event_base_new, event_add, event_del, event_dispatch, event_set, ... The
# ev->qev rename covered libev's own API (58 qev_*, 0 ev_*) and stopped there, so those 24 were
# still in the installed libqev.a, which sits on EVERY consumer's link line (qb::io names
# qb::ev in its INTERFACE_LINK_LIBRARIES). A consumer that also links the real libevent then
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
    message(STATUS "[qev] QB_EV_WARNING_AUDIT=ON forces QB_EV_STRICT_WARNINGS=ON")
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
# Watcher families
# ----------------------------------------------------------------------------
# ON standalone, OFF embedded, and the asymmetry is the point: an embedder that uses six watcher
# families should not pay for fourteen, and a library published to strangers must not silently
# ship without half of them. Flip it explicitly if you want the other answer.
option(QB_EV_WATCHERS_FULL
       "Build every libev watcher family (idle, prepare, check, fork, child, async, embed)"
       ${QB_EV_STANDALONE})
if (QB_EV_WATCHERS_FULL)
    set(QB_EV_IDLE_ENABLE 1)
    set(QB_EV_PREPARE_ENABLE 1)
    set(QB_EV_CHECK_ENABLE 1)
    set(QB_EV_FORK_ENABLE 1)
    set(QB_EV_ASYNC_ENABLE 1)
    set(QB_EV_EMBED_ENABLE 1)
    # The child watcher is the one family that is not a choice. It is built on waitpid(),
    # SIGCHLD, WNOHANG and WUNTRACED -- POSIX, none of which Windows has -- so asking for it
    # there is not a reduced feature set, it is a compile error (ev.c:3620, :4339). Upstream
    # libev gates it the same way. "Full" therefore means every family the PLATFORM has.
    if (WIN32)
        set(QB_EV_CHILD_ENABLE 0)
    else ()
        set(QB_EV_CHILD_ENABLE 1)
    endif ()
else ()
    set(QB_EV_IDLE_ENABLE 0)
    set(QB_EV_PREPARE_ENABLE 0)
    set(QB_EV_CHECK_ENABLE 0)
    set(QB_EV_FORK_ENABLE 0)
    set(QB_EV_CHILD_ENABLE 0)
    set(QB_EV_ASYNC_ENABLE 0)
    set(QB_EV_EMBED_ENABLE 0)
endif ()
message(STATUS "[qev] watcher families: ${QB_EV_WATCHERS_FULL} (full=all fourteen, off=qb's six)")

# ----------------------------------------------------------------------------
# Generate configuration header (public)
# ----------------------------------------------------------------------------
# Generated under a dedicated `include/` root mirroring the header namespace, so the ONE directory
# this project puts on its consumers' include path contains nothing but ${QB_EV_HEADER_DIR}/.
#
# ev.h and ev.c look for this file in three ways (ev.h:24-32, ev.c:23-30):
#     -DEV_CONFIG_H=<path>  >  HAVE_CONFIG_H + "config.h"  >  __has_include("ev_config.h")
# The last one is quote-relative to ev.h: it only works when ev_config.h sits NEXT TO ev.h --
# true in an installed tree, never true in a build tree (generated file, source-tree header). And
# because it is guarded by __has_include, a miss is SILENT: ev.h falls back to its own defaults and
# quietly disagrees with the compiled libqev.a (measured: EV_MULTIPLICITY 4 instead of 1). So the
# define below is what actually carries the configuration, and it is deliberately PUBLIC: it must
# reach every translation unit that includes ev.h, not just this library's own sources.
# One layout, both modes. The namespace differs (qb/ev when embedded, qev standalone); the SHAPE
# does not, and that is deliberate -- the standalone used to generate this at the include ROOT,
# which is how `include/ev_config.h` ended up in a prefix beside a real libev's.
set(QB_EV_CONFIG_INCLUDE "<${QB_EV_HEADER_DIR}/ev_config.h>")
set(QB_EV_CONFIG_OUT "${CMAKE_CURRENT_BINARY_DIR}/include/${QB_EV_HEADER_DIR}/ev_config.h")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmakein
        ${QB_EV_CONFIG_OUT}
        @ONLY
)

# Embedded: the parent project's install(DIRECTORY include/) does NOT cover this file (it is
# generated into the build tree), so it still needs its own rule -- at the namespaced destination,
# next to the ev.h that will look for it.
#
# Gated on QB_INSTALL when qb is the embedder, because this rule was the ONE hole in that gate.
# It is the only install() in the entire embedded-qb subtree that sat outside the guard, so a
# `cmake --install` of a QB_INSTALL=OFF build -- which is every dev preset, and every parent that
# reaches qb via add_subdirectory()/FetchContent -- silently deposited exactly one qb header,
# include/qb/ev/ev_config.h, into the CONSUMER's prefix. Exit 0, no diagnostic. That is
# precisely the leak the QB_INSTALL option exists to prevent (see cmake/qbConfig.cmake), and this
# file is the host-probed generated header, so it is also the worst single file to leak.
# QB_INSTALL is undefined for a standalone qev build; default it ON there so the fork keeps
# behaving on its own.
include(GNUInstallDirs)
if (NOT DEFINED QB_INSTALL)
    set(QB_INSTALL ON)
endif ()
if (QB_INSTALL)
    install(FILES ${QB_EV_CONFIG_OUT}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${QB_EV_HEADER_DIR}
            COMPONENT qev_Development)
endif ()

# ----------------------------------------------------------------------------
# Build library
# ----------------------------------------------------------------------------
set(LIBEV_SRCS ev.c)
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

    # The generated-config root is PUBLIC in both modes: ev.h needs it wherever it is included.
    target_include_directories(qev
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_definitions(qev PUBLIC QB_EV_CONFIG_H=${QB_EV_CONFIG_INCLUDE})
    # PRIVATE, and that is the point: it is what unlocks the HAVE_* half of the generated config
    # for ev.c only. Those 35 unprefixed autoconf names are qb's build-host answers, and the
    # config header is PUBLIC, so without this gate they reached -- and could silently override --
    # every consumer translation unit that includes <qb/io.h>. See config.h.cmakein.
    target_compile_definitions(qev PRIVATE QB_EV_BUILDING_LIBRARY=1)

    # The language level is a CONTRACT, not a hope: the sources use C99 designated initialisers and
    # `//` comments, and a consumer compiling with -std=c89 would fail deep inside ev.h instead of
    # at the point of the requirement. INTERFACE, so it propagates to whoever links.
    target_compile_features(qev PUBLIC c_std_99)

    # A Debug and a Release build must be able to share one prefix. Without a postfix the second
    # install silently overwrites the first with a differently-instrumented archive -- the same
    # same-name-different-content class this fork exists to close.
    set_target_properties(qev PROPERTIES DEBUG_POSTFIX "d")

    # PRIVATE in BOTH modes, and the standalone half of that is a fix. This directory holds
    # ev.h, ev++.h, event.h, event_compat.h, ev_vars.h, ev_wrap.h and wepoll.h. Exporting it
    # PUBLIC is what put all seven bare names on the include path of every target that links qev --
    # transitively, every consumer of qb::io, and every consumer of a standalone libqev. Consumers
    # reach the two public headers through the namespaced root staged just below
    # (<${QB_EV_HEADER_DIR}/ev++.h>); qev's own .c files use quote-relative includes and need
    # nothing but this private entry.
    target_include_directories(qev PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

    # Stage the public headers into the generated include root under the same namespace the
    # install uses, so <${QB_EV_HEADER_DIR}/ev.h> resolves identically in a build tree and in an
    # installed prefix. Embedded, the parent already exposes them at that path through its own
    # include root and this is a harmless duplicate of two headers; standalone, it is the only
    # thing that makes the namespaced spelling work before `cmake --install`.
    foreach (_qev_public_header ev.h ev++.h)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${_qev_public_header}"
                       "${CMAKE_CURRENT_BINARY_DIR}/include/${QB_EV_HEADER_DIR}/${_qev_public_header}"
                       COPYONLY)
    endforeach ()

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

    # Namespaced aliases. `qb::ev` follows qb's own rule -- the namespace absorbs the prefix,
    # exactly as qb-io becomes qb::io -- so a consumer writes qb::ev beside qb::io and qb::core.
    # `qev::qev` is the standalone idiom (fmt::fmt), for a project that reached this by
    # find_package(qev) and has never heard of qb.
    # EXPORT_NAME is what makes the exported name `qb::ev` while the physical target stays `qev`
    # (so the archive is libqev.a). Without it, install(EXPORT ... NAMESPACE qb::) concatenates the
    # namespace with the TARGET name and produces `qb::qev` -- which then makes the package config's
    # own alias fail, because `qb::ev` never exists in the consumer's scope. Same mechanism qb uses
    # for qb-io -> qb::io.
    set_target_properties(qev PROPERTIES EXPORT_NAME ev)
    # The ARCHIVE is named for the product that ships it, so the two never land on the same path:
    # embedded it is libqb-ev.a, qb's third archive beside libqb-io.a and libqb-core.a, reached as
    # qb::ev; standalone it is libqev.a, reached as find_package(qev) -> qev::qev. The CMake target
    # keeps one name in both modes because the shared CMakeLists has to build the same thing.
    if (QB_EV_EMBEDDED)
        set_target_properties(qev PROPERTIES OUTPUT_NAME qb-ev)
    else ()
        set_target_properties(qev PROPERTIES OUTPUT_NAME qev)
    endif ()
    add_library(qb::ev ALIAS qev)
    if (NOT TARGET qev::qev)
        add_library(qev::qev ALIAS qev)
    endif ()

    # --------------------------------------------------------------------------
    # Install + package config — find_package(qev) (and the qev::qev alias)
    # --------------------------------------------------------------------------
    # Skipped entirely when embedded: the parent owns the install. It installs the `qev` target in
    # its OWN export set and ships these headers through its own install(DIRECTORY include/), at
    # the namespaced path. Running this block as well would (a) re-install ev.h/ev++.h/event.h/
    # event_compat.h at the include ROOT, undoing the namespacing, and (b) drop a
    # lib/cmake/qev/ package next to the parent's own, exporting a second, differently-named
    # imported target for the same static library.
    if (NOT QB_EV_EMBEDDED)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # Components, because a distribution splits one build into two packages: libqev (the shared
    # object a program needs at RUNTIME) and libqev-dev (headers, static archive, CMake package,
    # pkg-config -- needed only to COMPILE against it). NAMELINK_COMPONENT puts the unversioned
    # `libqev.so` symlink on the development side, where it belongs: it exists for the linker, not
    # for the loader. Without this, packagers hand-curate file lists and get it wrong.
    install(TARGETS qev
            EXPORT qevTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                    COMPONENT qev_Development
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                    COMPONENT qev_Runtime
                    NAMELINK_COMPONENT qev_Development
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                    COMPONENT qev_Runtime
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    set(_qev_install_headers ev.h ev++.h)
    if (QB_EV_LIBEVENT_COMPAT)
        # Only shipped with the implementation. Installing the declarations without the bodies
        # would turn a use into an undefined reference -- loud, but pointlessly so; installing
        # them next to a *real* libevent is worse, because the two declare the same names over
        # incompatible struct layouts.
        list(APPEND _qev_install_headers event.h event_compat.h)
    endif ()
    # ${CMAKE_INSTALL_INCLUDEDIR}/qev, never the include ROOT. A prefix that carries a loose
    # `ev.h` / `event.h` is how this fork used to shadow a real libev and a real libevent
    # installed beside it -- the same shadowing the embedded mode has always prevented, applied
    # to the standalone build that actually ships to strangers.
    install(FILES ${_qev_install_headers}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${QB_EV_HEADER_DIR}
            COMPONENT qev_Development)

    install(EXPORT qevTargets
            FILE qevTargets.cmake
            NAMESPACE qb::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qev
            COMPONENT qev_Development
    )

    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qevConfig.cmake.in
            "@PACKAGE_INIT@
include(\"\${CMAKE_CURRENT_LIST_DIR}/qevTargets.cmake\")
# Unprefixed alias for projects that link the fork under its own namespace.
if(NOT TARGET qev::qev)
  add_library(qev::qev ALIAS qb::ev)
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
            COMPONENT qev_Development
    )

    # pkg-config for non-CMake consumers: `pkg-config --cflags --libs qev`. Standalone only, and
    # the template must EXIST -- qb's copy of this tree carries no qev.pc.in because an embedded
    # build ships qb's package config instead. The file is byte-shared, so "it is not there" has
    # to be a supported answer rather than a configure error.
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qev.pc.in")
        if (WIN32)
            set(QB_EV_PC_PRIVATE_LIBS "-lws2_32")
        else ()
            set(QB_EV_PC_PRIVATE_LIBS "-lm")
        endif ()
        configure_file(
                ${CMAKE_CURRENT_SOURCE_DIR}/qev.pc.in
                ${CMAKE_CURRENT_BINARY_DIR}/qev.pc
                @ONLY
        )
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qev.pc
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
                COMPONENT qev_Development)
    endif ()

    # The upstream copyright notices, installed with the artefacts they cover.
    #
    # BSD-2 clause 2 requires a redistribution IN BINARY FORM to reproduce the upstream
    # copyright notice, the conditions and the disclaimer "in the documentation and/or
    # other materials provided with the distribution". Nothing installed them until now, and
    # the published v5.0.0 archives proved what that costs: the Runtime tarball carried a
    # shared object derived from libev and wepoll with not one line of attribution in it, and
    # the Development tarball was no better. Only the SOURCE archive was ever compliant,
    # because CPack copies the whole tree into that one.
    #
    # Installed into BOTH components deliberately. CPack packages each component into its own
    # archive and either can be downloaded on its own, so a notice present in only one of them
    # is a notice absent from half the distributions.
    foreach (_qev_notice_component qev_Runtime qev_Development)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                      "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/qev"
                COMPONENT ${_qev_notice_component})
    endforeach ()

    # The manual page, when the tool that renders it is available.
    #
    # `qev.3` was an autotools-only artefact: `Makefile.am` has generated and installed it all
    # along, and the CMake build -- the path the README documents FIRST -- shipped no manual
    # page at all, while README and CHANGELOG both name `qev.3` among the things an install
    # puts on disk. pod2man comes with perl and is absent on a plain Windows runner, so this is
    # conditional on finding it rather than a new hard dependency; the .pod is likewise absent
    # from qb's byte-shared copy of this tree, so its existence is a supported answer too.
    find_program(QB_EV_POD2MAN NAMES pod2man)
    if (QB_EV_POD2MAN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs/ev.pod")
        set(_qev_man "${CMAKE_CURRENT_BINARY_DIR}/qev.3")
        add_custom_command(
            OUTPUT  "${_qev_man}"
            COMMAND "${QB_EV_POD2MAN}" -n QEV -r "qev-${PROJECT_VERSION}"
                    -c "qev - a modernized libev fork" -s 3
                    "${CMAKE_CURRENT_SOURCE_DIR}/docs/ev.pod" > "${_qev_man}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/ev.pod"
            COMMENT "Generating qev.3"
            VERBATIM)
        add_custom_target(qev-man ALL DEPENDS "${_qev_man}")
        # qev.3, not ev.3: the page is an INSTALLED artefact, and `ev.3` would overwrite a real
        # libev's in a shared prefix -- the same rule the archive and the header directory follow.
        install(FILES "${_qev_man}"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man3"
                COMPONENT qev_Development)
    endif ()

    endif () # NOT QB_EV_EMBEDDED
endif ()

# ----------------------------------------------------------------------------
# Tests & benchmark (standalone only)
# ----------------------------------------------------------------------------
# This file is byte-shared with qb's copy of the tree (dev/agent/check-qev-identity.py asserts
# it), and qb's copy carries no tests/ or bench/ directory -- it reaches this loop through its own
# suite. So the sources are required to EXIST rather than assumed: guarding on QB_EV_STANDALONE
# alone would be enough today and would break the day somebody add_subdirectory()s this tree as
# the top-level project of something else.
# BUILD_TESTING is the switch every CMake user already knows and every CI already sets; honouring
# it means `-DBUILD_TESTING=OFF` does what it says here too, instead of needing a project-specific
# flag nobody will guess. Defaulted rather than include(CTest), which drags in CDash machinery a
# library this size has no use for.
if (NOT DEFINED BUILD_TESTING)
    set(BUILD_TESTING ON)
endif ()
if (QB_EV_STANDALONE AND BUILD_TESTING)
    set(_qev_tests_default ON)
else ()
    set(_qev_tests_default OFF)
endif ()
option(QB_EV_BUILD_TESTS      "Build the qev watcher coverage tests"  ${_qev_tests_default})
option(QB_EV_BUILD_BENCHMARKS "Build the qev cross-backend benchmark" ${QB_EV_STANDALONE})

if (QB_EV_BUILD_TESTS AND TARGET qev AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-watchers.c")
    enable_testing()
    add_executable(qev-test-watchers tests/test-watchers.c)
    target_link_libraries(qev-test-watchers PRIVATE qev)
    # PRIVATE source dir: the test is an INTERNAL consumer and includes "ev.h" quote-relative,
    # the same way ev.c does. A public consumer writes <qev/ev.h> instead.
    # No private source-dir include: the test consumes qev through its PUBLIC interface, so it
    # compiles against the same <qev/ev.h> a downstream user does. A test that reached into the
    # source tree would keep passing after an install layout broke.
    add_test(NAME watchers COMMAND qev-test-watchers)
    set_tests_properties(watchers PROPERTIES TIMEOUT 60)
endif ()

if (QB_EV_BUILD_BENCHMARKS AND TARGET qev AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/bench-backends.c")
    add_executable(qev-bench-backends bench/bench-backends.c)
    target_link_libraries(qev-bench-backends PRIVATE qev)

endif ()

# ----------------------------------------------------------------------------
# CPack — source and binary archives, standalone only
# ----------------------------------------------------------------------------
# `cpack` from a build tree produces the tarball a packager starts from, and `cpack --config
# CPackSourceConfig.cmake` the source release. Both were absent, so cutting a release meant
# `git archive` plus remembering what to exclude. Component-aware, so a distribution can build
# libqev and libqev-dev from one invocation.
if (QB_EV_STANDALONE)
    set(CPACK_PACKAGE_NAME              "qev")
    set(CPACK_PACKAGE_VENDOR            "isndev")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "A maintained, cross-platform continuation of libev with a modernised backend set")
    set(CPACK_PACKAGE_HOMEPAGE_URL      "https://github.com/isndev/qev")
    set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
    set(CPACK_RESOURCE_FILE_LICENSE     "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    # README.md is standalone-only -- qb's byte-shared copy of this tree does not carry one, and
    # CPack's check is a hard FATAL_ERROR, not a warning. This is the same rule the tests and
    # benchmark blocks below state for their own sources: a byte-shared file must not assume a
    # sibling that only one of the two trees has.
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
        set(CPACK_RESOURCE_FILE_README  "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    endif ()
    set(CPACK_GENERATOR                 "TGZ;ZIP")
    set(CPACK_SOURCE_GENERATOR          "TGZ")
    set(CPACK_DEB_COMPONENT_INSTALL     ON)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_ALL            qev_Runtime qev_Development)
    set(CPACK_COMPONENT_QEV_RUNTIME_DISPLAY_NAME     "qev runtime")
    set(CPACK_COMPONENT_QEV_DEVELOPMENT_DISPLAY_NAME "qev development files")
    set(CPACK_COMPONENT_QEV_DEVELOPMENT_DEPENDS      qev_Runtime)
    # What must never end up in a source tarball. Anchored, so `build/` at the root goes and a
    # legitimate `src/rebuild.c` stays.
    set(CPACK_SOURCE_IGNORE_FILES
            "/\\.git/" "/\\.github/" "/build.*/" "/b-[a-z]*/" "\\.DS_Store$"
            "/autom4te\\.cache/" "\\.libs/" "\\.deps/" "^${CMAKE_CURRENT_SOURCE_DIR}/Makefile$")
    include(CPack)
endif ()
