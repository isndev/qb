/**
 * @file qb/io/tcp/ssl/openssl_required.h
 * @brief The one place qb's TLS headers reach OpenSSL, and the check that a build has it.
 *
 * `qb/io/tcp/ssl/context.h` and `qb/io/tcp/ssl/socket.h` include this instead of `<openssl/ssl.h>`,
 * and through them `qb/io/tcp/ssl/listener.h`, `transport::stcp` and `transport::saccept`: all of
 * them are the TLS layer, compiled into qb only when `QB_HAS_SSL` is defined. Included without it,
 * they used to reach `<openssl/ssl.h>` regardless -- a "file not found" on a host without the
 * OpenSSL headers, and on a host that has them a TU that compiles against declarations qb never
 * built (Huly QB-1015). Fail here instead, with the reason, the way `qb/io/crypto_jwt.h` does.
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
 * @ingroup SSL
 */

#ifndef QB_IO_TCP_SSL_OPENSSL_REQUIRED_H_
#define QB_IO_TCP_SSL_OPENSSL_REQUIRED_H_
#ifndef QB_HAS_SSL
#error "qb's TLS headers (qb/io/tcp/ssl/*, transport::stcp, transport::saccept) need OpenSSL: build qb with QB_WITH_SSL=ON"
#else
#include <openssl/ssl.h>
#endif
#endif // QB_IO_TCP_SSL_OPENSSL_REQUIRED_H_
