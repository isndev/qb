#include            <cstring>
#include            <cube/utility/build_macros.h>

#ifndef             CUBE_NETWORK_HELPER_H_
# define            CUBE_NETWORK_HELPER_H_

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
#else
        typedef            int         SocketHandler;
        typedef            socklen_t   AddrLength;
        constexpr static const SocketHandler SOCKET_INVALID = -1;
#endif

        class CUBE_API helper {
        public:
            static sockaddr_in createAddress(uint32_t address, unsigned short port);
            static bool close(SocketHandler sock);
            static bool block(SocketHandler sock, bool block);
            static bool is_blocking(SocketHandler sock);
            static SocketStatus getErrorStatus();
        };

    } // namespace network
} // namespace cube

#endif // CUBE_NETWORK_HELPER_H_
