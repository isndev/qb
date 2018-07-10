#include            "ip.h"
#include            "SocketBase.h"

#ifndef             CUBE_NETWORK_SOCKETTCP_H_
# define            CUBE_NETWORK_SOCKETTCP_H_

namespace           cube {
    namespace       network {

        class CUBE_API SocketTCP
                : public TSocket<Socket::TCP> {
        public:
            SocketTCP();
            SocketTCP(Socket::Handler fd);
            SocketTCP(SocketTCP const &) = default;

            ip getRemoteAddress() const;

            unsigned short getLocalPort() const;
            unsigned short getRemotePort() const;

            Socket::Status connect(const ip &remoteAddress, unsigned short remotePort, int timeout = 0);
            void disconnect();

            Socket::Status send(const void *data, std::size_t size) const;
            Socket::Status send(const void *data, std::size_t size, std::size_t &sent) const;
            Socket::Status receive(void *data, std::size_t size, std::size_t &received) const;

        private:
            friend class Listener;
        };

    } //namespace network
} //namespace cube

#endif
