#include            <cstdlib>
#include            <cube/utility/nocopy.h>
#include            "helper.h"

#ifndef             QB_NETWORK_TSOCKET_H_
# define            QB_NETWORK_TSOCKET_H_

namespace           qb {
    namespace       network {
        namespace   sys {

            template<SocketType _Type>
            class QB_API Socket{
            protected:
                SocketHandler _handle;

                void init() {
                    if (!good()) {
                        SocketHandler handle;
                        if constexpr (_Type == SocketType::TCP)
                            handle = ::socket(PF_INET, SOCK_STREAM, 0);
                        else
                            handle = ::socket(PF_INET, SOCK_DGRAM, 0);
                        init(handle);
                    }
                }

                void init(SocketHandler handle) {
                    if (!good() && (handle != SOCKET_INVALID)) {
                        if constexpr (_Type == SocketType::TCP) {
                            // Disable the Nagle algorithm (i.e. removes buffering of TCP packets)
                            int yes = 1;
                            if (setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&yes),
                                           sizeof(yes)) == -1) {
                                std::cerr << "Failed to set socket option \"TCP_NODELAY\" ; "
                                          << "all your TCP packets will be buffered" << std::endl;
                            }
                        } else {
                            // Enable broadcast by default for UDP sockets
                            int yes = 1;
                            if (setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char *>(&yes),
                                           sizeof(yes)) == -1)
                                std::cerr << "Failed to enable broadcast on UDP socket" << std::endl;
                        }
                        _handle = handle;
                    } else {
                        throw std::runtime_error("Failed to init socket");
                    }
                }

            public:
                constexpr static const SocketType type = _Type;

                Socket()
                        : _handle(SOCKET_INVALID)
                {}

                ~Socket() {
                }

                SocketHandler raw() const {
                    return _handle;
                }

                int setBlocking(bool new_state) const {
                    return helper::block(_handle, new_state);
                }

                bool isBlocking() const {
                    return helper::is_blocking(_handle);
                }

                bool setReceiveBufferSize(int size) const {
                    return setsockopt(_handle, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)) != -1;
                }

                bool setSendBufferSize(int size) const {
                    return setsockopt(_handle, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)) != -1;
                }

                bool good() const {
                    return _handle != SOCKET_INVALID;
                }

                void close() const {
                    if (good() && !helper::close(_handle))
                        std::cerr << "Failed to close socket" << std::endl;
                }
            };

        } // namespace sys
    } // namespace network
} // namespace qb

#endif // QB_NETWORK_TSOCKET_H_
