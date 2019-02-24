#include            "ip.h"
#include            "tsocket.h"

#ifndef             CUBE_NETWORK_UDP_H_
# define            CUBE_NETWORK_UDP_H_

namespace           cube {
    namespace       network {
        namespace   udp {

            class CUBE_API socket
                : public internal::TSocket<SocketType::UDP> {
                constexpr static const std::size_t MaxDatagramSize = 65507;

                socket();

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
} // namespace cube

#endif // CUBE_NETWORK_UDP_H_
