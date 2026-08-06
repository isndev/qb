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

# -----------------------------------------------------------------------------
# qb IO Library - CMake Configuration
#
# This file configures the build system for the qb IO component, which provides
# network I/O, socket handling, and core communication functionality.
# -----------------------------------------------------------------------------

# Define project with same version as parent project
project(qb-io
        LANGUAGES CXX
        VERSION ${QB_FRAMEWORK_VERSION}
        DESCRIPTION "I/O and networking library for the qb framework"
)

# -----------------------------------------------------------------------------
# Library Configuration
# -----------------------------------------------------------------------------
# -----------------------------------------------------------------------------
# HAZARD -- THIS AMALGAMATION IS LOAD-BEARING FOR CORRECTNESS, NOT JUST BUILD TIME.
#
# io.cpp is an amalgamation: it #includes the real .cpp files rather than compiling them
# separately. Splitting this list (and qb-core's, which is the same shape) is a THREE-line
# CMake edit that silently breaks the program. Measured, by rebuilding with only these two
# SOURCES lists changed -- 32 TUs instead of 4, `diff -r` confirming no source-text change:
#
#  1. src/qb/io/tcp/ssl/init.cpp defines an anonymous-namespace object whose CONSTRUCTOR is its
#     entire payload (SSL_library_init / SSL_load_error_strings / OpenSSL_add_all_algorithms /
#     signal(SIGPIPE, SIG_IGN)). Split out, that object file defines no *referenced* external
#     symbol, so the linker never extracts it from the archive:
#         AMALGAMATED : SIGPIPE at start of main: SIG_IGN  -> the initialiser RAN
#         SPLIT       : SIGPIPE at start of main: SIG_DFL  -> it DID NOT
#     Consequence for a consumer: the process dies on SIGPIPE and OpenSSL legacy init never
#     happens. No compiler warning, no linker warning, no runtime error.
#     SCOPE: io.cpp includes that file inside `#ifdef QB_HAS_SSL`, so in an SSL-OFF build (the
#     `feature-gates` preset) the initialiser is not compiled at all and the process-wide SIGPIPE
#     disposition is never set, split or not. qb's own sockets are still safe there via
#     MSG_NOSIGNAL on every write plus SO_NOSIGPIPE per descriptor (system/sys__socket.cpp:79-120);
#     the SIG_IGN above is an SSL-build backstop, and it is simply the observable this test reads.
#     Before splitting: give the unit a referenced external symbol (e.g. an
#     `qb::io::tcp::ssl::ensure_init()` called from ssl::socket's constructor), or install with
#     -force_load / --whole-archive / /WHOLEARCHIVE.
#
#  2. src/qb/io/system/file.cpp and src/qb/io/crypto.cpp are NOT self-contained (18 errors
#     between them: FD_INVALID, <unistd.h>, DISABLE_WARNING_PUSH/POP). The amalgamation
#     supplies those from the lines above the #include. `::write`/`::read`/`::close` are the
#     nasty ones -- without <unistd.h> the qualified call binds to the *member* instead of
#     failing on an unknown name.
#
#  3. FIXED IN 3.0, RECORDED BECAUSE IT IS THE ONE THAT WENT AWAY. Through 2.6.0, qb-core's
#     VirtualCore.cpp used fill_event<SignalEvent> and push<UnregisterCallbackEvent> while
#     including only VirtualCore.h; the definitions lived in VirtualCore.tpp and were reached
#     only because Actor.cpp sits after VirtualCore.cpp in core.cpp. Split, the library built
#     and archived cleanly and the CONSUMER's link failed. Retiring the four .tpp put those
#     bodies at the tail of VirtualCore.h itself, so the dependency on amalgamation ORDER is
#     gone. Re-measured on this tree: VirtualCore.cpp compiles standalone (rc=0) and its object
#     DEFINES both templates rather than referencing them --
#         __ZN2qb11VirtualCore10fill_eventINS_11SignalEventEEE...          defined
#         __ZN2qb11VirtualCore4pushINS_23UnregisterCallbackEventEJEEE...   defined
#     Items 1 and 2 are unchanged and were re-measured the same day: file.cpp still fails
#     standalone with 13 errors and crypto.cpp with 5 -- the "18 between them" above.
#
# Full measurement in dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0.md §3.2 and §6(a)(b).
# -----------------------------------------------------------------------------
# Define the library using the new qb framework functions
set(IO_SOURCES
        cpu.cpp
        io.cpp
        quic.cpp
)

# Collect dependencies. PUBLIC (see _qb_apply_dependencies): qb/json.h exposes nlohmann::json in
# qb's own API, so every consumer of qb::io -- including qbm/pgsql's type_converter -- needs the
# same nlohmann include path qb-io was compiled against.
set(IO_DEPENDENCIES qev stduuid qb-nlohmann)

# Collect definitions
set(IO_DEFINITIONS)

# -----------------------------------------------------------------------------
# Logging configuration
# -----------------------------------------------------------------------------
if (QB_WITH_LOGGING)
    list(APPEND IO_DEFINITIONS QB_WITH_LOGGING=1)
    qb_debug_message("Building with logger support")
else ()
    qb_debug_message("Building without logger support")
    if (QB_STDOUT_LOGGING)
        list(APPEND IO_DEFINITIONS QB_STDOUT_LOGGING=1)
        qb_debug_message("Enabling stdout logging")
    endif ()
endif ()

# -----------------------------------------------------------------------------
# Platform-specific dependencies
# -----------------------------------------------------------------------------
if (QB_PLATFORM_WINDOWS)
    list(APPEND IO_DEPENDENCIES ws2_32)
    qb_debug_message("Adding Windows-specific dependency: ws2_32")
endif ()

# -----------------------------------------------------------------------------
# Optional features based on detected dependencies
# -----------------------------------------------------------------------------
if (QB_HAS_COMPRESSION)
    list(APPEND IO_DEPENDENCIES ZLIB::ZLIB)
    list(APPEND IO_DEFINITIONS QB_HAS_COMPRESSION=1)
    qb_debug_message("Building with ZLIB compression support")
endif ()

if (QB_HAS_SSL)
    list(APPEND IO_DEPENDENCIES OpenSSL::SSL OpenSSL::Crypto)
    list(APPEND IO_DEFINITIONS QB_HAS_SSL=1)
    qb_debug_message("Building with OpenSSL SSL/TLS support")

    if (QB_HAS_ARGON2)
        list(APPEND IO_DEPENDENCIES Argon2::Argon2)
        list(APPEND IO_DEFINITIONS QB_HAS_ARGON2=1)
        qb_debug_message("Building with Argon2 advanced crypto support")
    endif ()
endif ()

if (QB_HAS_QUIC)
    list(APPEND IO_DEPENDENCIES
            Ngtcp2::ngtcp2
            Ngtcp2::crypto_ossl
    )
    list(APPEND IO_DEFINITIONS QB_HAS_QUIC=1)
    qb_debug_message("Building with libngtcp2 QUIC transport support")
endif ()

# -----------------------------------------------------------------------------
# Create the IO library
# -----------------------------------------------------------------------------
qb_add_library(
        NAME ${PROJECT_NAME}
        SOURCES ${IO_SOURCES}
        DEPENDS ${IO_DEPENDENCIES}
        DEFINES ${IO_DEFINITIONS}
)

# Set the export name to io (instead of qb-io)
set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME io)

# -----------------------------------------------------------------------------
# Tests configuration
# -----------------------------------------------------------------------------
# QB_BUILD_BENCHMARKS is in the condition because the benchmarks live UNDER tests/io/ (in
# tests/io/benchmark/, added from tests/io/CMakeLists.txt). With the gate on QB_BUILD_TESTS alone,
# `-DQB_BUILD_TESTS=OFF -DQB_BUILD_BENCHMARKS=ON` never reached qb_add_benchmark at all: it
# configured cleanly, printed "- Benchmarks: ON" and "Google Benchmark: TRUE", fetched/linked the
# dependency, and produced ZERO benchmark executables. No preset could see it because the one
# preset that enables benchmarks (`benchmarks`) also has tests on. tests/io/CMakeLists.txt gates
# unit/ and system/ on QB_BUILD_TESTS, so entering here with tests off adds only the benchmarks.
if (QB_BUILD_TESTS OR QB_BUILD_BENCHMARKS)
    if (QB_BUILD_TESTS)
        qb_status_message("Building qb-io tests")
    endif ()
    # Tests live BESIDE the include root, never inside it: a test header swept into
    # install(DIRECTORY src/ ...) is the failure mode this layout exists to prevent.
    # Added from here rather than from the top level so the suite inherits PROJECT_NAME
    # (= qb-io), which every qb_add_test call in it names as its DEPENDS.
    add_subdirectory("${QB_ROOT_DIR}/tests/io" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif ()
