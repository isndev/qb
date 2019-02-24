#include            "ip.h"
#include            "tsocket.h"

#ifndef             CUBE_NETWORK_TCP_H_
# define            CUBE_NETWORK_TCP_H_

namespace           cube {
    namespace       network {
        namespace   tcp {

            class CUBE_API socket
                : public internal::TSocket<SocketType::TCP> {
            public:
                socket();
                socket(socket const &) = default;
                socket(SocketHandler fd);

                ip getRemoteAddress() const;
                unsigned short getLocalPort() const;
                unsigned short getRemotePort() const;

                SocketStatus connect(const ip &remoteAddress, unsigned short remotePort, int timeout = 0);
                void disconnect();

                SocketStatus send(const void *data, std::size_t size) const;
                SocketStatus send(const void *data, std::size_t size, std::size_t &sent) const;
                SocketStatus sendall(const void *data, std::size_t size, std::size_t &sent) const;
                SocketStatus receive(void *data, std::size_t size, std::size_t &received) const;

            private:
                friend class listener;
            };

            class CUBE_API listener
                    : public socket {
            public:
                listener();

                unsigned short getLocalPort() const;

                SocketStatus listen(unsigned short port, const ip &address = ip::Any);
                SocketStatus accept(socket &socket);
            };

        } // namespace tcp
    } // namespace network
} // namespace cube

#endif // CUBE_NETWORK_TCP_H_
