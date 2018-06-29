#include            <network/ip.h>
#include            <network/SocketBase.h>
#include            <network/SocketTCP.h>

#ifndef             CUBE_LISTENER_H
#define             CUBE_LISTENER_H

namespace           cube {
    namespace       network {

        class CUBE_API Listener
                : public TSocket<Socket::TCP> {
            Listener();

            unsigned short getLocalPort() const;

            Socket::Status listen(unsigned short port, const ip& address = ip::Any);
            Socket::Status accept(SocketTCP& socket);
        };

    } //namespace network
} //namespace cube

#endif //CUBE_LISTENER_H
