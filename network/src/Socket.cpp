#include            <network/Socket.h>

namespace           cube {
    namespace       network {
#ifdef __WIN__SYSTEM__

        sockaddr_in Socket::createAddress(uint32_t address, unsigned short port)
        {
            sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_addr.s_addr = htonl(address);
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);

            return addr;
        }

        bool Socket::close(Socket::Handler socket) {
            return !closesocket(socket);
        }

        bool Socket::block(Socket::Handler socket, bool block) {
            unsigned long new_state = static_cast<unsigned long>(block);
            return !ioctlsocket(socket, FIONBIO, &new_state);
        }

        Socket::Status Socket::getErrorStatus() {
            switch (WSAGetLastError()) {
                case WSAEWOULDBLOCK:
                    return Socket::NotReady;
                case WSAEALREADY:
                    return Socket::NotReady;
                case WSAECONNABORTED:
                    return Socket::Disconnected;
                case WSAECONNRESET:
                    return Socket::Disconnected;
                case WSAETIMEDOUT:
                    return Socket::Disconnected;
                case WSAENETRESET:
                    return Socket::Disconnected;
                case WSAENOTCONN:
                    return Socket::Disconnected;
                case WSAEISCONN:
                    return Socket::Done; // when connecting a non-blocking socket
                default:
                    return Socket::Error;
            }
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
#include				<errno.h>
#include				<fcntl.h>

        sockaddr_in Socket::createAddress(uint32_t address, unsigned short port)
        {
            sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_addr.s_addr = htonl(address);
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);
            addr.sin_len = sizeof(addr);

            return addr;
        }

        bool Socket::close(Socket::Handler sock)
        {
            return !::close(sock);
        }

        bool Socket::block(Socket::Handler sock, bool newst)
        {
            int	status = fcntl(sock, F_GETFL);

            return ((newst ?
                     fcntl(sock, F_SETFL, status & ~O_NONBLOCK) :
                     fcntl(sock, F_SETFL, status | O_NONBLOCK)) != -1);
        }

        Socket::Status Socket::getErrorStatus()
        {
            if ((errno == EAGAIN) || (errno == EINPROGRESS))
                return Socket::NotReady;

            switch (errno)
            {
                case EWOULDBLOCK:  return Socket::NotReady;
                case ECONNABORTED: return Socket::Disconnected;
                case ECONNRESET:   return Socket::Disconnected;
                case ETIMEDOUT:    return Socket::Disconnected;
                case ENETRESET:    return Socket::Disconnected;
                case ENOTCONN:     return Socket::Disconnected;
                case EPIPE:        return Socket::Disconnected;
                default:           return Socket::Error;
            }
        }

#endif
    }
}

