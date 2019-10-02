/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include            <qb/utility/branch_hints.h>
#include            <qb/io/tcp/listener.h>

namespace qb {
    namespace io {
        namespace tcp {

            listener::listener()
                    : socket() {}

            listener::~listener() {
                close();
            }

            unsigned short listener::getLocalPort() const {
                if (good()) {
                    // Retrieve informations about the local end of the socket
                    sockaddr_in address;
                    AddrLength size = sizeof(address);
                    if (getsockname(_handle, reinterpret_cast<sockaddr *>(&address), &size) != -1) {
                        return ntohs(address.sin_port);
                    }
                }

                // We failed to retrieve the port
                return 0;
            }

            SocketStatus listener::listen(unsigned short port, const ip &address) {
                // Close the socket if it is already bound
                close();

                // init the internal socket if it doesn't exist
                init();

#ifdef __LINUX__SYSTEM__
                int Yes = 1;
                setsockopt(_handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&Yes), sizeof(Yes));
#endif
                // Check if the address is valid
                if ((address == ip::None)) {
                    _handle = SOCKET_INVALID;
                    return SocketStatus::Error;
                }

                // Bind the socket to the specified port
                sockaddr_in addr = helper::createAddress(address.toInteger(), port);
                if (bind(_handle, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
                    _handle = SOCKET_INVALID;
                    // Not likely to happen, but...
                    std::cerr << "Failed to bind listener socket to port " << port << std::endl;
                    return SocketStatus::Error;
                }

                // Listen to the bound port
                if (::listen(_handle, SOMAXCONN) == -1) {
                    close();
                    _handle = SOCKET_INVALID;
                    // Oops, socket is deaf
                    std::cerr << "Failed to listen to port " << port << std::endl;
                    return SocketStatus::Error;
                }

                return SocketStatus::Done;
            }

            SocketStatus listener::accept(socket &sock) {
                // Make sure that we're listening
                if (!good()) {
                    std::cerr << "Failed to accept a new connection, the socket is not listening" << std::endl;
                    return SocketStatus::Error;
                }

                // Accept a new connection
                sockaddr_in address;
                AddrLength length = sizeof(address);
                SocketHandler remote = ::accept(_handle, reinterpret_cast<sockaddr *>(&address), &length);

                // Check for errors
                if (remote == SOCKET_INVALID)
                    return helper::getErrorStatus();

                // Initialize the new connected socket
                sock.close();
                sock.init(remote);

                return SocketStatus::Done;
            }

        } // namespace tcp
    } // namespace io
} // namespace qb
