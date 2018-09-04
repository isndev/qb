#include            <iostream>
#include            <cstring>
#include            <cstdlib>
#include            "../utils/nocopy.h"
#include            "Socket.h"

#ifndef             CUBE_NETWORK_SOCKET_BASE_H_
# define            CUBE_NETWORK_SOCKET_BASE_H_

namespace           cube {
    namespace       network {

        template <Socket::Type _Type>
        class CUBE_API TSocket {
        protected:
            Socket::Handler _handle;

            void init() {
                if (!good()) {
                    Socket::Handler handle;
                    if constexpr (_Type == Socket::TCP)
                        handle = socket(PF_INET, SOCK_STREAM, 0);
                    else
                        handle = socket(PF_INET, SOCK_DGRAM, 0);
                    init(handle);
                }
            }
            constexpr static const Socket::Type type = _Type;

            void init(Socket::Handler handle) {
                if (!good() && (handle != Socket::INVALID)) {
                    if constexpr (_Type == Socket::TCP) {
                        // Disable the Nagle algorithm (i.e. removes buffering of TCP packets)
                        int yes = 1;
                        if (setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&yes), sizeof(yes)) == -1)
                        {
                            std::cerr << "Failed to set socket option \"TCP_NODELAY\" ; "
                                      << "all your TCP packets will be buffered" << std::endl;
                        }
                    } else {
                        // Enable broadcast by default for UDP sockets
                        int yes = 1;
                        if (setsockopt(_handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&yes), sizeof(yes)) == -1)
                            std::cerr << "Failed to enable broadcast on UDP socket" << std::endl;
                    }

                    _handle = handle;
                } else {
                    throw std::runtime_error("Failed to init socket");
                }
            }

        public:
            TSocket()
                    : _handle(Socket::INVALID)
            {}

            ~TSocket() {
            }

            Socket::Handler raw() const {
                return _handle;
            }

            int setBlocking(bool new_state) const {
                return Socket::block(_handle, new_state);
            }

            bool isBlocking() const {
                return Socket::is_blocking(_handle);
            }

            bool good() const {
                return _handle != Socket::INVALID;
            }

            void close() const {
                if (good() && !Socket::close(_handle))
                    std::cerr << "Failed to close socket" << std::endl;
            }
        };

    } //namespace network
} //namespace cube

#endif
