#include            <network/ip.h>
#include            <network/SocketBase.h>

#ifndef             CUBE_SOCKETTCP_H_
# define            CUBE_SOCKETTCP_H_

namespace           cube {
    namespace       network {

        class CUBE_API SocketTCP
                : public TSocket<Socket::TCP> {
        public:
            SocketTCP();

            ip getRemoteAddress() const;

            unsigned short getLocalPort() const;
            unsigned short getRemotePort() const;

            Socket::Status connect(const ip &remoteAddress, unsigned short remotePort, int timeout = 0);
            void disconnect();

            Socket::Status send(const void *data, std::size_t size);
            Socket::Status send(const void *data, std::size_t size, std::size_t &sent);
            Socket::Status receive(void *data, std::size_t size, std::size_t &received);

        private:
            friend class Listener;
        };

    } // namespace network
} // namespace cube

#endif
