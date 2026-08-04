/**
 * @file qb/io/io.cpp
 * @brief Implementation of core I/O functionality for the QB framework
 *
 * This file includes the implementation of various I/O components and serves
 * as the main entry point for the I/O subsystem. It includes all necessary
 * implementations and provides the UUID generation functionality.
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

#ifdef QB_WITH_LOGGING
#include <qb/vendor/nanolog/nanolog.cpp>
#endif
// First in the amalgamation on purpose: the fingerprint symbols must be defined by this
// archive member, and this member is the one every consumer already pulls in.
#include "abi.cpp"
#include "logger.cpp"
#include "pipe.cpp"
#include "uri.cpp"
#include "system/sys__socket.cpp"
#include "system/file.cpp"
#include "tcp/listener.cpp"
#include "tcp/socket.cpp"

#include <qb/uuid.h>
namespace qb {
uuid
generate_random_uuid() {
    thread_local uuids::uuid_random_generator gen{[]() {
        std::random_device                        rd;
        std::array<int, std::mt19937::state_size> seed_data;
        std::generate(std::begin(seed_data), std::end(seed_data), std::ref(rd));
        thread_local std::seed_seq seq(std::begin(seed_data), std::end(seed_data));
        thread_local std::mt19937  generator(seq);
        return &generator;
    }()};
    return gen();
}
} // namespace qb
#include "json.cpp"

// The OpenSSL-free members of qb::crypto (hex codec, xor_bytes,
// constant_time_compare). Compiled unconditionally: qbm-pgsql's bytea codec and
// the HPACK tests use the hex helpers, and they must link in a QB_WITH_SSL=OFF
// build. Everything else in qb::crypto stays behind the guard below.
#include "crypto_core.cpp"

#ifdef QB_HAS_SSL
#include "crypto.cpp"
#include "crypto_modern.cpp"
#include "crypto_advanced.cpp"
#include "crypto_asymmetric.cpp"
#include "crypto_jwt.cpp"
#include "tcp/ssl/init.cpp"
#include "tcp/ssl/listener.cpp"
#include "tcp/ssl/socket.cpp"
#include "tcp/ssl/context.cpp"
#endif
#ifdef QB_HAS_COMPRESSION
#include "compression.cpp"
#endif
#include "async/listener.cpp"
#include "stream.cpp"
#include "udp/socket.cpp"

// CoroutineScheduler TLS used to be defined HERE, "in exactly one TU". That is what made it one
// per *image* instead of one per process: an out-of-line thread_local emits a `non-external` TLS
// descriptor, so a host executable and a plugin that each statically link libqb-io.a each got
// their own "current scheduler" on the same thread, silently. Both are now `inline` +
// QB_ABI_ANCHOR in qb/io/async/coroutine/scheduler.h, which emits a weak-external descriptor that
// dyld coalesces. Do not move them back.
#include <qb/io/async/coroutine/scheduler.h>