#include            <array>
#include            <network/ip.h>
#include            <network/SocketBase.h>

#ifndef				CUBE_SOCKETUDP_HPP_
# define			CUBE_SOCKETUDP_HPP_

namespace			cube {
	namespace 		network {

		class CUBE_API SocketUDP
				: public TSocket<Socket::UDP> {
			constexpr static const std::size_t MaxDatagramSize = 65507;
			
			SocketUDP();
			
			unsigned short getLocalPort() const;

			Socket::Status bind(unsigned short port, const ip& address = ip::Any);
			void unbind();

			Socket::Status send(const void* data, std::size_t size, const ip& remoteAddress, unsigned short remotePort);
			Socket::Status receive(void* data, std::size_t size, std::size_t& received, ip& remoteAddress, unsigned short& remotePort);

		};

	} // namespace network
} // namespace cube

#endif
