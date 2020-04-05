/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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

#include <cstring>
#include <iostream>
#include <qb/utility/build_macros.h>

#ifndef QB_IO_HELPER_H_
#    define QB_IO_HELPER_H_

#    ifdef __WIN__SYSTEM__
#        ifndef WIN32_LEAN_AND_MEAN
#            define WIN32_LEAN_AND_MEAN
#        endif
#        include <WS2tcpip.h>
#        include <winsock2.h>
#    else
#        include <arpa/inet.h>
#        include <errno.h>
#        include <fcntl.h>
#        include <netdb.h>
#        include <netinet/in.h>
#        include <netinet/tcp.h>
#        include <sys/socket.h>
#        include <sys/types.h>
#        include <unistd.h>
#    endif

namespace qb::io {

enum SocketType { TCP, UDP };

enum SocketStatus { Done, NotReady, Partial, Disconnected, Error };

#    ifdef __WIN__SYSTEM__
typedef SOCKET SocketHandler;
typedef int AddrLength;
constexpr static const SocketHandler SOCKET_INVALID = INVALID_SOCKET;
constexpr static const int FD_INVALID = -1;

struct WinSockInitializer {
    bool _init = false;
    WinSockInitializer() noexcept;

    ~WinSockInitializer() noexcept;

    [[nodiscard]] bool isInitialized() const noexcept;

    const static WinSockInitializer status;
};

#    else
typedef int SocketHandler;
typedef socklen_t AddrLength;
constexpr static const SocketHandler SOCKET_INVALID = -1;
constexpr static const int FD_INVALID = -1;
#    endif

class QB_API helper {
public:
    static sockaddr_in createAddress(uint32_t address, unsigned short port);
    static bool close(SocketHandler sock);
    static bool block(SocketHandler sock, bool block);
    static bool is_blocking(SocketHandler sock);
    static SocketStatus getErrorStatus();
};

} // namespace qb::io

#endif // QB_IO_HELPER_H_
