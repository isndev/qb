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

#include            <algorithm>
#include            <qb/io/udp/socket.h>

namespace           qb {
    namespace       io {
        namespace   udp {

            socket::socket() :
                    sys::socket<SocketType::UDP>() {
                init();
            }

            socket::socket(SocketHandler handle) :
                    sys::socket<SocketType::UDP>() {
                _handle = handle;
            }

            unsigned short socket::getLocalPort() const {
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

            SocketStatus socket::bind(unsigned short port, const ip &address) {

                // Create the internal socket if it doesn't exist
                init();

                // Check if the address is valid
                if ((address == ip::None))
                    return SocketStatus::Error;

                // Bind the socket
                sockaddr_in addr = helper::createAddress(address.toInteger(), port);
                if (::bind(_handle, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
                    std::cerr << "Failed to bind socket to port " << port << std::endl;
                    return SocketStatus::Error;
                }

                return SocketStatus::Done;
            }

            void socket::unbind() {
                // Simply close the socket
                close();
            }

            int socket::write(const void *data, std::size_t size, const ip &remoteAddress, unsigned short remotePort) const {

                // Build the target address
                sockaddr_in address = helper::createAddress(remoteAddress.toInteger(), remotePort);

                // Send the data (unlike TCP, all the data is always sent in one call)
                return sendto(_handle, static_cast<const char *>(data), static_cast<int>(size), 0,
                                  reinterpret_cast<sockaddr *>(&address), sizeof(address));
            }

            int socket::read(void *data, std::size_t size, ip &remoteAddress, unsigned short &remotePort) const {
                // First clear the variables to fill
                remoteAddress = ip();
                remotePort = 0;

                // Data that will be filled with the other computer's address
                sockaddr_in address = helper::createAddress(INADDR_ANY, 0);

                // Receive a chunk of bytes
                AddrLength addressSize = sizeof(address);
                const int sizeReceived = recvfrom(_handle, static_cast<char *>(data), static_cast<int>(size), 0,
                                            reinterpret_cast<sockaddr *>(&address), &addressSize);

                // Check for errors
                if (sizeReceived >= 0) {
                    // Fill the sender informations
                    remoteAddress = ip(ntohl(address.sin_addr.s_addr));
                    remotePort = ntohs(address.sin_port);
                }

                return sizeReceived;
            }

        } // namespace udp
    } // namespace io
} // namespace qb
