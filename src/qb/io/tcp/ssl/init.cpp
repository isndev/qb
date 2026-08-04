/**
 * @file qb/io/tcp/ssl/init.cpp
 * @brief SSL/TLS library initialization implementation
 *
 * This file contains the implementation for initializing and configuring the SSL/TLS
 * library used by the QB framework. It handles the setup of the cryptographic subsystem,
 * loading of security certificates, and configuration of cipher suites and security
 * options.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#include <openssl/ssl.h>

// ============================================================================================
// HAZARD -- THIS FILE ONLY RUNS BECAUSE IT IS AMALGAMATED. DO NOT SPLIT IT OUT.
//
// The object below has no name anyone references: its CONSTRUCTOR is the entire payload. An
// archive member that defines no *referenced* external symbol is never extracted. Today that is
// hidden because src/qb/io/CMakeLists.txt compiles io.cpp, which #includes this file, and the
// consumer's link pulls io.cpp.o for other reasons.
//
// Measured, by building qb with only the two SOURCES lists changed (32 TUs instead of 4):
//     AMALGAMATED archive : SIGPIPE at start of main: SIG_IGN  -> this constructor RAN
//     SPLIT       archive : SIGPIPE at start of main: SIG_DFL  -> it DID NOT
// `ld -why_load` on the split link lists 9 extracted qb members; tcp/ssl/init.cpp.o is not one.
// The consequence is silent: the consumer's process dies on SIGPIPE and OpenSSL legacy init
// never happens. No compiler warning, no linker warning, no runtime error.
//
// Before splitting, give this unit a referenced external symbol -- e.g. a
// `qb::io::tcp::ssl::ensure_init()` called from ssl::socket's constructor -- or install with
// -force_load / --whole-archive / /WHOLEARCHIVE. Note also that `signal`/`SIGPIPE` below reach
// this file through the amalgamation, not through any #include here: a split needs <csignal>
// too. See dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0.md §3.2.
// ============================================================================================

namespace {

struct OpenSSLInitializer {
    OpenSSLInitializer() noexcept {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms(); // Still potentially useful
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif // !_WIN32
    }
} initializer = {};

} // namespace