#include            "ip.h"
#include            "sys.h"

#ifndef             QB_NETWORK_UDP_H_
# define            QB_NETWORK_UDP_H_

namespace           qb {
    namespace       network {
        namespace   udp {

            class QB_API Socket
                : public sys::Socket<SocketType::UDP> {
                constexpr static const std::size_t MaxDatagramSize = 65507;

                Socket();

                unsigned short getLocalPort() const;

                SocketStatus bind(unsigned short port, const ip &address = ip::Any);

                void unbind();

                SocketStatus
                send(const void *data, std::size_t size, const ip &remoteAddress, unsigned short remotePort) const;

                SocketStatus
                receive(void *data, std::size_t size, std::size_t &received, ip &remoteAddress, unsigned short &remotePort) const;

            };

        } // namespace udp
    } // namespace network
} // namespace qb

#endif // QB_NETWORK_UDP_H_
