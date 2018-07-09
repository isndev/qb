#include            <cstring>
#include            "../utils/build_macros.h"

#ifndef             CUBE_NETWORK_SOCKET_H_
# define            CUBE_NETWORK_SOCKET_H_

#ifdef __WIN__SYSTEM__
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
#endif

namespace           cube {
    namespace       network {

        class CUBE_API Socket {
        public:
            enum Type {
                TCP,
                UDP
            };

            enum Status
            {
                Done,
                NotReady,
                Partial,
                Disconnected,
                Error
            };

#ifdef      __WIN__SYSTEM__
            typedef            SOCKET      Handler;
            typedef            int         AddrLength;
            constexpr static const Handler INVALID = INVALID_SOCKET;
#else
            typedef            int         Handler;
            typedef            socklen_t   AddrLength;
            constexpr static const Handler INVALID = -1;
#endif

            static sockaddr_in createAddress(uint32_t address, unsigned short port);
            static bool close(Socket::Handler sock);
            static bool block(Socket::Handler sock, bool block);
            static bool is_blocking(Socket::Handler sock);
            static Socket::Status getErrorStatus();
        };

    } //namespace network
} //namespace cube

#endif
