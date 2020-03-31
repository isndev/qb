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

#include            <cstring>
#include            <iostream>
#include            <qb/utility/build_macros.h>

#ifndef             QB_IO_HELPER_H_
# define            QB_IO_HELPER_H_

#ifdef __WIN__SYSTEM__
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <WS2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#endif

namespace           qb {
    namespace       io {

        enum SocketType {
            TCP,
            UDP
        };

        enum SocketStatus
        {
            Done,
            NotReady,
            Partial,
            Disconnected,
            Error
        };

#ifdef      __WIN__SYSTEM__
        typedef            SOCKET      SocketHandler;
        typedef            int         AddrLength;
        constexpr static const SocketHandler SOCKET_INVALID = INVALID_SOCKET;
        constexpr static const int FD_INVALID = -1;
#else
        typedef            int         SocketHandler;
        typedef            socklen_t   AddrLength;
        constexpr static const SocketHandler SOCKET_INVALID = -1;
        constexpr static const int FD_INVALID = -1;
#endif

        class QB_API helper {
        public:
			//struct socket {
				static sockaddr_in createAddress(uint32_t address, unsigned short port);
				static bool close(SocketHandler sock);
				static bool block(SocketHandler sock, bool block);
				static bool is_blocking(SocketHandler sock);
				static SocketStatus getErrorStatus();
			//};

			//struct file {
			//	static int open(const char* fname, int flags);
			//	static int close();
			//	static int write(int fd, const char *data, std::size_t size);
			//	static int read(int fd, char* data, std::size_t size);
			//};
        };

    } // namespace io
} // namespace qb

#endif // QB_IO_HELPER_H_
