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

#include            <qb/network/helper.h>

namespace           qb {
    namespace       network {
#ifdef __WIN__SYSTEM__

        sockaddr_in helper::createAddress(uint32_t address, unsigned short port)
        {
            sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_addr.s_addr = htonl(address);
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);

            return addr;
        }

        bool helper::close(SocketHandler socket) {
            return !closesocket(socket);
        }

        bool helper::block(SocketHandler socket, bool block) {
            unsigned long new_state = static_cast<unsigned long>(!block);
            return !ioctlsocket(socket, FIONBIO, &new_state);
        }

        SocketStatus helper::getErrorStatus() {
			const auto err = WSAGetLastError();
            switch (err) {
                case WSAEWOULDBLOCK:
                    return SocketStatus::NotReady;
                case WSAEALREADY:
                    return SocketStatus::NotReady;
                case WSAECONNABORTED:
                    return SocketStatus::Disconnected;
                case WSAECONNRESET:
                    return SocketStatus::Disconnected;
                case WSAETIMEDOUT:
                    return SocketStatus::Disconnected;
                case WSAENETRESET:
                    return SocketStatus::Disconnected;
                case WSAENOTCONN:
                    return SocketStatus::Disconnected;
                case WSAEISCONN:
                    return SocketStatus::Done; // when connecting a non-blocking socket
                default:
                    return SocketStatus::Error;
            }
        }

        bool helper::is_blocking(SocketHandler)
        {
            return true;
        }

        struct SocketInitializer {
            SocketInitializer() {
                WSADATA InitData;
                WSAStartup(MAKEWORD(2, 2), &InitData);
            }

            ~SocketInitializer() {
                WSACleanup();
            }
        };

        SocketInitializer GlobalInitializer;
#else
        #include                <errno.h>
        #include                <fcntl.h>

        sockaddr_in helper::createAddress(uint32_t address, unsigned short port)
        {
            sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_addr.s_addr = htonl(address);
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);

            return addr;
        }

        bool helper::close(SocketHandler sock)
        {
            return !::close(sock);
        }

        bool helper::block(SocketHandler sock, bool newst)
        {
            int    status = fcntl(sock, F_GETFL);

            return (newst ?
                     fcntl(sock, F_SETFL, status & ~O_NONBLOCK) != -1 :
                     fcntl(sock, F_SETFL, status | O_NONBLOCK) != -1);
        }

        SocketStatus helper::getErrorStatus()
        {
            if ((errno == EAGAIN) || (errno == EINPROGRESS))
                return SocketStatus::NotReady;

            switch (errno)
            {
                case EWOULDBLOCK:  return SocketStatus::NotReady;
                case ECONNABORTED: return SocketStatus::Disconnected;
                case ECONNRESET:   return SocketStatus::Disconnected;
                case ETIMEDOUT:    return SocketStatus::Disconnected;
                case ENETRESET:    return SocketStatus::Disconnected;
                case ENOTCONN:     return SocketStatus::Disconnected;
                case EPIPE:        return SocketStatus::Disconnected;
                default:           return SocketStatus::Error;
            }
        }

        bool helper::is_blocking(SocketHandler sock)
        {
            int    status = fcntl(sock, F_GETFL);

            return !(status & O_NONBLOCK);
        }
#endif

    } // namespace network
} // namespace qb

