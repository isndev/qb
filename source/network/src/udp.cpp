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
#include            <qb/network/udp.h>

namespace           qb {
    namespace       network {
        namespace   udp {

            Socket::Socket() :
                    sys::Socket<SocketType::UDP>() {
                init();
            }

            unsigned short Socket::getLocalPort() const {
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

            SocketStatus Socket::bind(unsigned short port, const ip &address) {

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

            void Socket::unbind() {
                // Simply close the socket
                close();
            }

            SocketStatus
            Socket::send(const void *data, std::size_t size, const ip &remoteAddress, unsigned short remotePort) const {

                // Build the target address
                sockaddr_in address = helper::createAddress(remoteAddress.toInteger(), remotePort);

                // Send the data (unlike TCP, all the data is always sent in one call)
                int sent = sendto(_handle, static_cast<const char *>(data), static_cast<int>(size), 0,
                                  reinterpret_cast<sockaddr *>(&address), sizeof(address));

                // Check for errors
                if (sent < 0)
                    return helper::getErrorStatus();

                return SocketStatus::Done;
            }

            SocketStatus Socket::receive(void *data, std::size_t size, std::size_t &received, ip &remoteAddress,
                                         unsigned short &remotePort) const {
                // First clear the variables to fill
                received = 0;
                remoteAddress = ip();
                remotePort = 0;

                // Data that will be filled with the other computer's address
                sockaddr_in address = helper::createAddress(INADDR_ANY, 0);

                // Receive a chunk of bytes
                AddrLength addressSize = sizeof(address);
                int sizeReceived = recvfrom(_handle, static_cast<char *>(data), static_cast<int>(size), 0,
                                            reinterpret_cast<sockaddr *>(&address), &addressSize);

                // Check for errors
                if (sizeReceived < 0)
                    return helper::getErrorStatus();

                // Fill the sender informations
                received = static_cast<std::size_t>(sizeReceived);
                remoteAddress = ip(ntohl(address.sin_addr.s_addr));
                remotePort = ntohs(address.sin_port);

                return SocketStatus::Done;
            }

        } // namespace udp
    } // namespace network
} // namespace qb
