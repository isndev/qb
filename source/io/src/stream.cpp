/**
 * @file qb/io/src/stream.cpp
 * @brief Implementation of templated stream classes for file, TCP and UDP sockets.
 *
 * @details This file contains explicit template instantiations for stream classes
 * with different underlying I/O mechanisms (files, TCP sockets, UDP sockets, and SSL
 * sockets when enabled). These instantiations ensure that the templated classes are
 * available at link time.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include <qb/io/stream.h>
#include <qb/io/system/file.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/udp/socket.h>

namespace qb {
namespace io {

template class istream<io::sys::file>;
template class ostream<io::sys::file>;
template class stream<io::sys::file>;

template class istream<tcp::socket>;
template class ostream<tcp::socket>;
template class stream<tcp::socket>;

template class istream<udp::socket>;
template class ostream<udp::socket>;
template class stream<udp::socket>;

} // namespace io
} // namespace qb

#ifdef QB_IO_WITH_SSL

#include <qb/io/tcp/ssl/socket.h>

namespace qb {
namespace io {

template class istream<tcp::ssl::socket>;
template class ostream<tcp::ssl::socket>;
template class stream<tcp::ssl::socket>;

} // namespace io
} // namespace qb

#endif