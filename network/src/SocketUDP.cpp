#include            <algorithm>
#include            "../SocketUDP.h"

namespace           cube {
    namespace       network {

        SocketUDP::SocketUDP() :
                TSocket<Socket::UDP>() {
        }

        unsigned short SocketUDP::getLocalPort() const {
            if (good()) {
                // Retrieve informations about the local end of the socket
                sockaddr_in address;
                Socket::AddrLength size = sizeof(address);
                if (getsockname(_handle, reinterpret_cast<sockaddr *>(&address), &size) != -1) {
                    return ntohs(address.sin_port);
                }
            }

            // We failed to retrieve the port
            return 0;
        }

        Socket::Status SocketUDP::bind(unsigned short port, const ip &address) {

            // Create the internal socket if it doesn't exist
            init();

            // Check if the address is valid
            if ((address == ip::None))
                return Socket::Error;

            // Bind the socket
            sockaddr_in addr = Socket::createAddress(address.toInteger(), port);
            if (::bind(_handle, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
                std::cerr << "Failed to bind socket to port " << port << std::endl;
                return Socket::Error;
            }

            return Socket::Done;
        }

        void SocketUDP::unbind() {
            // Simply close the socket
            close();
        }

        Socket::Status
        SocketUDP::send(const void *data, std::size_t size, const ip &remoteAddress, unsigned short remotePort) const {

            // Build the target address
            sockaddr_in address = Socket::createAddress(remoteAddress.toInteger(), remotePort);

            // Send the data (unlike TCP, all the data is always sent in one call)
            int sent = sendto(_handle, static_cast<const char *>(data), static_cast<int>(size), 0,
                              reinterpret_cast<sockaddr *>(&address), sizeof(address));

            // Check for errors
            if (sent < 0)
                return Socket::getErrorStatus();

            return Socket::Done;
        }

        Socket::Status SocketUDP::receive(void *data, std::size_t size, std::size_t &received, ip &remoteAddress,
                                          unsigned short &remotePort) const {
            // First clear the variables to fill
            received = 0;
            remoteAddress = ip();
            remotePort = 0;

            // Data that will be filled with the other computer's address
            sockaddr_in address = Socket::createAddress(INADDR_ANY, 0);

            // Receive a chunk of bytes
            Socket::AddrLength addressSize = sizeof(address);
            int sizeReceived = recvfrom(_handle, static_cast<char *>(data), static_cast<int>(size), 0,
                                        reinterpret_cast<sockaddr *>(&address), &addressSize);

            // Check for errors
            if (sizeReceived < 0)
                return Socket::getErrorStatus();

            // Fill the sender informations
            received = static_cast<std::size_t>(sizeReceived);
            remoteAddress = ip(ntohl(address.sin_addr.s_addr));
            remotePort = ntohs(address.sin_port);

            return Socket::Done;
        }

    } // namespace network
} // namespace cube
