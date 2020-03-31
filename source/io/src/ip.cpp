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

#include            <qb/io/ip.h>
#include            <qb/io/helper.h>

namespace           qb {
    namespace       io {

        const ip ip::None(INADDR_NONE);
        const ip ip::Any(0, 0, 0, 0);
        const ip ip::LocalHost(127, 0, 0, 1);

        ip::ip() :
                _address(INADDR_NONE) {
        }

        ip::ip(const std::string &address) :
                _address(0) {
            resolve(address);
        }

        ip::ip(const char *address) :
                _address(0) {
            resolve(address);
        }

        ip::ip(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3) :
                _address(htonl((byte0 << 24) | (byte1 << 16) | (byte2 << 8) | byte3)) {
        }

        ip::ip(uint32_t address) :
                _address(htonl(address)) {
        }

        std::string ip::toString() const {
            char buffer[32];
            in_addr address;
            address.s_addr = _address;
            inet_ntop(AF_INET, &address, buffer, 32);
            return {buffer};
        }

        uint32_t ip::toInteger() const {
            return ntohl(_address);
        }

        void ip::resolve(const std::string &address) {
            _address = ip::None.toInteger();

            if (address == "255.255.255.255") {
                // The broadcast address needs to be handled explicitly,
                // because it is also the value returned by inet_addr on error
                _address = INADDR_BROADCAST;
            } else if (address == "0.0.0.0") {
                _address = INADDR_ANY;
            } else {
                // Try to convert the address as a byte representation ("xxx.xxx.xxx.xxx")
                in_addr addr;

                if (inet_pton(AF_INET, address.c_str(), &addr)) {
                    _address = static_cast<uint32_t>(addr.s_addr);
                } else {
                    // Not a valid address, try to convert it as a host name
                    struct addrinfo hints;
                    memset(&hints, 0, sizeof(hints));
                    hints.ai_family = AF_INET; /* v4 or v6 is fine. */
                    hints.ai_socktype = SOCK_STREAM;
                    hints.ai_protocol = IPPROTO_TCP; /* We want a TCP socket */
                    hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */

                    /* Look up the hostname. */
                    struct addrinfo* answer = nullptr;
                    int err = getaddrinfo(address.c_str(), nullptr, &hints, &answer);
                    if (!err) {
                        struct addrinfo* rp = answer;
                        while ( rp != nullptr) {
                            struct sockaddr_in* a = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);

                            if (a->sin_addr.s_addr) {
                                _address = a->sin_addr.s_addr;
                            }
                            rp = rp->ai_next;
                        }
                    }
                    freeaddrinfo(answer);
                }
            }
        }

        bool operator==(const ip &left, const ip &right) {
            return !(left < right) && !(right < left);
        }

        bool operator!=(const ip &left, const ip &right) {
            return !(left == right);
        }

        bool operator<(const ip &left, const ip &right) {
            return left._address < right._address;
        }

        bool operator>(const ip &left, const ip &right) {
            return right < left;
        }

        bool operator<=(const ip &left, const ip &right) {
            return !(right < left);
        }

        bool operator>=(const ip &left, const ip &right) {
            return !(left < right);
        }

        std::istream &operator>>(std::istream &stream, ip &address) {
            std::string str;
            stream >> str;
            address = ip(str);

            return stream;
        }

        std::ostream &operator<<(std::ostream &stream, const ip &address) {
            return stream << address.toString();
        }

    } // namespace io
} // namespace qb