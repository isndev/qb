#include            <network/Listener.h>

namespace           cube {
    namespace       network {

        Listener::Listener()
                : TSocket<Socket::TCP>()
        {}

        unsigned short Listener::getLocalPort() const
        {
            if (good())
            {
                // Retrieve informations about the local end of the socket
                sockaddr_in address;
                Socket::AddrLength size = sizeof(address);
                if (getsockname(_handle, reinterpret_cast<sockaddr*>(&address), &size) != -1)
                {
                    return ntohs(address.sin_port);
                }
            }

            // We failed to retrieve the port
            return 0;
        }

        Socket::Status Listener::listen(unsigned short port, const ip& address)
        {
            // Close the socket if it is already bound
            close();

            // init the internal socket if it doesn't exist
            init();

            // Check if the address is valid
            if ((address == ip::None))
                return Socket::Error;

            // Bind the socket to the specified port
            sockaddr_in addr = Socket::createAddress(address.toInteger(), port);
            if (bind(_handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
            {
                // Not likely to happen, but...
                std::cerr << "Failed to bind listener socket to port " << port << std::endl;
                return Socket::Error;
            }

            // Listen to the bound port
            if (::listen(_handle, SOMAXCONN) == -1)
            {
                // Oops, socket is deaf
                std::cerr << "Failed to listen to port " << port << std::endl;
                return Socket::Error;
            }

            return Socket::Done;
        }

        Socket::Status Listener::accept(SocketTCP& socket)
        {
            // Make sure that we're listening
            if (!good())
            {
                std::cerr << "Failed to accept a new connection, the socket is not listening" << std::endl;
                return Socket::Error;
            }

            // Accept a new connection
            sockaddr_in address;
            Socket::AddrLength length = sizeof(address);
            Socket::Handler remote = ::accept(_handle, reinterpret_cast<sockaddr*>(&address), &length);

            // Check for errors
            if (remote == Socket::INVALID)
                return Socket::getErrorStatus();

            // Initialize the new connected socket
            socket.close();
            socket.init(remote);

            return Socket::Done;
        }

    }
}