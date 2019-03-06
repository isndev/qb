#include            "ip.h"
#include            "sys.h"

#ifndef             QB_NETWORK_TCP_H_
# define            QB_NETWORK_TCP_H_

namespace           qb {
    namespace       network {
        namespace   tcp {

            class QB_API Socket
                : public sys::Socket<SocketType::TCP> {
            public:
                Socket();
                Socket(Socket const &rhs) = default;
                Socket(SocketHandler fd);

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
                friend class Listener;
            };

            class QB_API Listener
                    : public Socket {
            public:
                Listener();
                Listener(Listener const &) = delete;

                unsigned short getLocalPort() const;

                SocketStatus listen(unsigned short port, const ip &address = ip::Any);
                SocketStatus accept(Socket &socket);
            };

        } // namespace tcp
    } // namespace network
} // namespace qb

#endif // QB_NETWORK_TCP_H_
