#include            "ip.h"
#include            "SocketBase.h"
#include            "SocketTCP.h"

#ifndef             CUBE_NETWORK_LISTENER_H
#define             CUBE_NETWORK_LISTENER_H

namespace           cube {
    namespace       network {

        class CUBE_API Listener
                : public TSocket<Socket::TCP> {
        public:
            Listener();

            unsigned short getLocalPort() const;

            Socket::Status listen(unsigned short port, const ip& address = ip::Any);
            Socket::Status accept(SocketTCP& socket);
        };

    } //namespace network
} //namespace cube

#endif //CUBE_LISTENER_H
