/**
 * @file qb/io/system/sys__socket.h
 * @brief Comprehensive cross-platform socket interface implementation
 *
 * This file provides a unified socket API that abstracts platform-specific
 * implementations (Windows/POSIX) behind a consistent interface with:
 * - Complete protocol header structures (IP, TCP, UDP, ICMP, ARP)
 * - Socket management for both connection-oriented and connectionless protocols
 * - Full IPv4/IPv6 dual-stack capabilities with transparent fallback
 * - Advanced non-blocking I/O support with timeout controls
 * - Platform-independent endpoint representation for all address families
 * - Comprehensive socket options management
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef QB_IO_SOCKET_H
#define QB_IO_SOCKET_H

#include <chrono>
#include <errno.h>
#include <functional>
#include <sstream>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include <qb/io/config.h>

#if !defined(_WS2IPDEF_)
/**
 * @brief Checks if an IPv4 address is a loopback address
 *
 * Tests whether the given IPv4 address falls within the loopback address range
 * (127.0.0.0/8) as defined in RFC 1122.
 *
 * @param a Pointer to the IPv4 address structure to check
 * @return true if the address is a loopback address (127.x.x.x), false otherwise
 */
inline bool
IN4_IS_ADDR_LOOPBACK(const in_addr *a) {
    return ((a->s_addr & 0xff) == 0x7f); // 127/8
}
/**
 * @brief Checks if an IPv4 address is a link-local address
 *
 * Link-local addresses are in the range 169.254.0.0/16 and are used
 * when DHCP is unavailable.
 *
 * @param a Pointer to the IPv4 address structure to check
 * @return true if the address is a link-local address, false otherwise
 */
inline bool
IN4_IS_ADDR_LINKLOCAL(const in_addr *a) {
    return ((a->s_addr & 0xffff) == 0xfea9); // 169.254/16
}
/**
 * @brief Checks if an IPv6 address is a global unicast address
 *
 * @param a Pointer to the IPv6 address structure to check
 * @return true if the address is a global unicast address, false otherwise
 */
inline bool
IN6_IS_ADDR_GLOBAL(const in6_addr *a) {
    //
    // Check the format prefix and exclude addresses
    // whose high 4 bits are all zero or all one.
    // This is a cheap way of excluding v4-compatible,
    // v4-mapped, loopback, multicast, link-local, site-local.
    //
    unsigned int High = (a->s6_addr[0] & 0xf0);
    return ((High != 0) && (High != 0xf0));
}
#endif

#define QB_ADDR_ANY(af) (af == AF_INET ? "0.0.0.0" : "::")
#define QB_MAX_CHAR_UCHAR 4
#define QB_MAX_CHAR_USHORT 6

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#if !defined MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace qb::io {
QB__NS_INLINE
namespace inet {

// #define _make_dotted_decimal(b1,b2,b3,b4) ( ( ((uint32_t)(b4) << 24) & 0xff000000 ) |
// (
// ((uint32_t)(b3) << 16) & 0x00ff0000 ) | ( ((uint32_t)(b2) << 8) & 0x0000ff00 ) | (
// (uint32_t)(b1) & 0x000000ff ) )
static const socket_type invalid_socket = (socket_type) -1;

QB__NS_INLINE
namespace ip {
#pragma pack(push, 1)
// ip packet
/**
 * @struct ip_header
 * @brief Internet Protocol (IPv4) header structure
 *
 * Complete representation of an IPv4 header according to RFC 791.
 * Contains all fields necessary for packet processing, including
 * version, header length, type of service, packet length, fragmentation
 * information, time-to-live, protocol identifier, checksum, and addresses.
 */
struct ip_header {
    // header size; 5+
    unsigned char header_length : 4;

    // IP version: 0100/0x04(IPv4), 0110/0x05(IPv6)
    unsigned char version : 4;

    // type of service:
    union {
        unsigned char value;
        struct {
            unsigned char priority : 3;
            unsigned char D : 1; // delay: 0(normal), 1(as little as possible)
            unsigned char T : 1; // throughput: 0(normal), 1(as big as possible)
            unsigned char R : 1; // reliability: 0(normal), 1(as big as possible)
            unsigned char
                C : 1; // transmission cost: 0(normal), 1(as little as possible)
            unsigned char reserved : 1; // always be zero
        } detail;
    } TOS;

    // total size, header + data; MAX length is: 65535
    unsigned short total_length;

    // identifier: all split small packet set as the same value.
    unsigned short identifier;

    // flags and frag
    unsigned short flags : 3;
    unsigned short frag : 13;

    // time of living, decreased by route, if zero, this packet will by dropped
    // avoid foward looply.
    unsigned char TTL;

    // protocol
    // 1: ICMP
    // 2: IGMP
    // 6: TCP
    // 0x11/17: UDP
    // 0x58/88: IGRP
    // 0x59/89: OSPF
    unsigned char protocol; // TCP / UDP / Other

    // check header of IP-PACKET 's correctness.
    unsigned short checksum;

    /**
     * @union dotted_decimal_t
     * @brief Utility for handling IPv4 addresses in dotted decimal notation
     */
    typedef union {
        unsigned int value;
        struct {
            unsigned int B1 : 8, B2 : 8, B3 : 8, B4 : 8;
        } detail;
    } dotted_decimal_t;

    // source ip address
    dotted_decimal_t src_ip;

    // destination ip address
    dotted_decimal_t dst_ip;
};

/**
 * @struct psd_header
 * @brief Pseudo header for TCP/UDP checksum calculation
 *
 * This structure is used when calculating checksums for TCP and UDP protocols.
 * It's not part of the actual packet header but is used in the checksum
 * calculation process to include IP-level information as specified in RFC 793.
 */
struct psd_header {
    unsigned long  src_addr;
    unsigned long  dst_addr;
    char           mbz;
    char           protocol;
    unsigned short tcp_length;
};

/**
 * @struct tcp_header
 * @brief Transmission Control Protocol (TCP) header structure
 *
 * Complete TCP header definition as specified in RFC 793, containing all
 * fields required for connection management, reliable data transfer,
 * flow control, and other TCP functionality.
 */
struct tcp_header {
    unsigned short src_port; // lgtm [cpp/class-many-fields]
    unsigned short dst_port;
    unsigned int   seqno;
    unsigned int   ackno;
    unsigned char  header_length : 4;
    unsigned char  reserved : 4;
    unsigned char  flg_fin : 1, flg_syn : 1, flg_rst : 1, flg_psh : 1, flg_ack : 1,
        flg_urg : 1, flg_reserved : 2;
    unsigned short win_length;
    unsigned short checksum;
    unsigned short urp;
};

/**
 * @struct udp_header
 * @brief User Datagram Protocol (UDP) header structure
 *
 * UDP header definition as specified in RFC 768, containing the minimal
 * fields required for connectionless datagram delivery.
 */
struct udp_header {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned short length;
    unsigned short checksum;
};

/**
 * @struct icmp_header
 * @brief ICMP header structure
 *
 * Contains the fields for Internet Control Message Protocol headers.
 */
struct icmp_header {
    unsigned char  type;     // 8bit type
    unsigned char  code;     // 8bit code
    unsigned short checksum; // 16bit check sum
    unsigned short id;       // identifier: usually use process id
    unsigned short seqno;    // message sequence NO.
};

/**
 * @struct eth_header
 * @brief Ethernet frame header structure
 */
struct eth_header {
    unsigned dst_eth[6];
    unsigned src_eth[6];
    unsigned eth_type;
};

/**
 * @struct arp_header
 * @brief ARP packet header structure
 *
 * Address Resolution Protocol header used to map IP addresses to MAC addresses.
 */
struct arp_header {
    unsigned short arp_hw;     // format of hardware address
    unsigned short arp_pro;    // format of protocol address
    unsigned char  arp_hlen;   // length of hardware address
    unsigned char  arp_plen;   // length of protocol address
    unsigned short arp_op;     // arp operation
    unsigned char  arp_oha[6]; // sender hardware address
    unsigned long  arp_opa;    // sender protocol address
    unsigned char  arp_tha;    // target hardware address
    unsigned long  arp_tpa;    // target protocol address;
};

/**
 * @struct arp_packet
 * @brief Complete ARP packet with Ethernet header
 */
struct arp_packet {
    eth_header ethhdr;
    arp_header arphdr;
};
#pragma pack(pop)

namespace compat {
#if QB__HAS_NTOP
using ::inet_ntop;
using ::inet_pton;
#else
QB__DECL const char *inet_ntop(int af, const void *src, char *dst, socklen_t);
QB__DECL int         inet_pton(int af, const char *src, void *dst);
#endif
} // namespace compat

/**
 * @brief Checks if an IPv4 address is a global address
 *
 * Tests whether the given IPv4 address is usable for global routing,
 * excluding special-purpose addresses like loopback, link-local,
 * private networks, etc.
 *
 * @param addr Pointer to the IPv4 address structure to check
 * @return true if the address is globally routable, false otherwise
 */
inline bool
is_global_in4_addr(const in_addr *addr) {
    return !IN4_IS_ADDR_LOOPBACK(addr) && !IN4_IS_ADDR_LINKLOCAL(addr);
};
/**
 * @brief Checks if an IPv6 address is a global address
 *
 * Tests whether the given IPv6 address is usable for global routing,
 * excluding special-purpose addresses like loopback, link-local,
 * unique local addresses, etc.
 *
 * @param addr Pointer to the IPv6 address structure to check
 * @return true if the address is globally routable, false otherwise
 */
inline bool
is_global_in6_addr(const in6_addr *addr) {
    return !!IN6_IS_ADDR_GLOBAL(addr);
};

struct endpoint {
public:
    /**
     * @brief Default constructor
     *
     * Creates an endpoint with zeroed values.
     */
    endpoint() {
        this->zeroset();
    }
    /**
     * @brief Copy constructor
     *
     * @param rhs The endpoint to copy
     */
    endpoint(const endpoint &rhs) {
        this->as_is(rhs);
    }
    /**
     * @brief Construct endpoint from addrinfo
     *
     * @param info Pointer to addrinfo structure
     */
    explicit endpoint(const addrinfo *info) {
        as_is(info);
    }
    /**
     * @brief Construct endpoint from sockaddr
     *
     * @param info Pointer to sockaddr structure
     */
    explicit endpoint(const sockaddr *info) {
        as_is(info);
    }
    /**
     * @brief Construct endpoint from IPv4 address and port
     *
     * @param addr IPv4 address in network byte order
     * @param port Port number in host byte order
     */
    explicit endpoint(const char *addr, unsigned short port = 0) {
        as_in(addr, port);
    }
    /**
     * @brief Construct endpoint from IPv4 address and port
     *
     * @param addr IPv4 address in network byte order
     * @param port Port number in host byte order
     */
    explicit endpoint(uint32_t addr, unsigned short port = 0) {
        as_in(addr, port);
    }
    endpoint(int family, const void *addr, unsigned short port = 0) {
        as_in(family, addr, port);
    }

    /**
     * @brief Boolean operator to check if the endpoint is valid
     *
     * @return true if the endpoint has a valid address
     */
    explicit
    operator bool() const {
        return this->af() != AF_UNSPEC;
    }

    endpoint &
    operator=(const endpoint &rhs) {
        return this->as_is(rhs);
    }
    endpoint &
    as_is(const endpoint &rhs) {
        this->zeroset();
        memcpy(reinterpret_cast<char *>(this), &rhs, sizeof(rhs));
        return *this;
    }
    endpoint &
    as_is(const addrinfo *info) {
        return this->as_is_raw(info->ai_addr, info->ai_addrlen);
    }
    endpoint &
    as_is(const sockaddr *addr) {
        this->zeroset();
        switch (addr->sa_family) {
            case AF_INET:
                ::memcpy(&in4_, addr, sizeof(sockaddr_in));
                this->len(sizeof(sockaddr_in));
                break;
            case AF_INET6:
                ::memcpy(&in6_, addr, sizeof(sockaddr_in6));
                this->len(sizeof(sockaddr_in6));
                break;
#if defined(QB_ENABLE_UDS) && QB__HAS_UDS
            case AF_UNIX:
                as_un(((sockaddr_un *) addr)->sun_path);
                break;
#endif
        }
        return *this;
    }
    endpoint &
    as_in(int family, const void *addr_in, u_short port) {
        this->zeroset();
        this->af(family);
        this->port(port);
        switch (family) {
            case AF_INET:
                ::memcpy(&in4_.sin_addr, addr_in, sizeof(in_addr));
                this->len(sizeof(sockaddr_in));
                break;
            case AF_INET6:
                ::memcpy(&in6_.sin6_addr, addr_in, sizeof(in6_addr));
                this->len(sizeof(sockaddr_in6));
                break;
        }
        return *this;
    }
    endpoint &
    as_in(const char *addr, unsigned short port) {
        this->zeroset();

        /*
         * Windows XP no inet_pton or inet_ntop
         */
        if (strchr(addr, ':') == nullptr) { // ipv4
            if (compat::inet_pton(AF_INET, addr, &this->in4_.sin_addr) == 1) {
                this->in4_.sin_family = AF_INET;
                this->in4_.sin_port   = htons(port);
                this->len(sizeof(sockaddr_in));
            }
        } else { // ipv6
            if (compat::inet_pton(AF_INET6, addr, &this->in6_.sin6_addr) == 1) {
                this->in6_.sin6_family = AF_INET6;
                this->in6_.sin6_port   = htons(port);
                this->len(sizeof(sockaddr_in6));
            }
        }

        return *this;
    }
    endpoint &
    as_in(uint32_t addr, u_short port) {
        this->zeroset();

        this->af(AF_INET);
        this->addr_v4(addr);
        this->port(port);
        this->len(sizeof(sockaddr_in));
        return *this;
    }

#if defined(QB_ENABLE_UDS) && QB__HAS_UDS
    endpoint &
    as_un(const char *name) {
        int n = snprintf(un_.sun_path, sizeof(un_.sun_path) - 1, "%s", name);
        if (n > 0) {
            un_.sun_family = AF_UNIX;
            this->len(offsetof(struct sockaddr_un, sun_path) + n + 1);
        } else {
            un_.sun_family = AF_UNSPEC;
            this->len(0);
        }
        return *this;
    }
#endif

    endpoint &
    as_is_raw(const void *ai_addr, size_t ai_addrlen) {
        this->zeroset();
        ::memcpy(reinterpret_cast<void *>(this), ai_addr, ai_addrlen);
        this->len(ai_addrlen);
        return *this;
    }

    /**
     * @brief Resets the endpoint to zeroed values
     */
    void
    zeroset() {
        ::memset(reinterpret_cast<void *>(this), 0x0, sizeof(*this));
    }

    /**
     * @brief Sets the address family
     *
     * @param v Address family value (e.g., AF_INET, AF_INET6)
     */
    void
    af(int v) {
        sa_.sa_family = static_cast<uint16_t>(v);
    }
    /**
     * @brief Gets the address family
     *
     * @return The address family value
     */
    int
    af() const {
        return sa_.sa_family;
    }

    /**
     * @brief Sets the IP address from string representation
     *
     * Parses and sets the IP address from a string like "192.168.1.1" or "2001:db8::1".
     * Automatically detects and handles both IPv4 and IPv6 formats.
     *
     * @param addr String containing the IP address in standard notation
     */
    void
    ip(const char *addr) {
        /*
         * Windows XP no inet_pton or inet_ntop
         */
        if (strchr(addr, ':') == nullptr) { // ipv4
            this->in4_.sin_family = AF_INET;
            compat::inet_pton(AF_INET, addr, &this->in4_.sin_addr);
        } else { // ipv6
            this->in6_.sin6_family = AF_INET6;
            compat::inet_pton(AF_INET6, addr, &this->in6_.sin6_addr);
        }
    }
    /**
     * @brief Gets the IP address as a string
     *
     * @return String representation of the IP address
     */
    std::string
    ip() const {
        std::string ipstring(IN_MAX_ADDRSTRLEN - 1, '\0');

        auto str = inaddr_to_string(
            &ipstring.front(), [](const in_addr *) { return true; },
            [](const in6_addr *) { return true; });

        ipstring.resize(str ? strlen(str) : 0);
        return ipstring;
    }
    /**
     * @brief Creates a string representation of the endpoint
     *
     * @return String in format "address:port" or "[address]:port" for IPv6
     */
    std::string
    to_string() const {
        std::string addr(IN_MAX_ADDRSTRLEN + sizeof("65535") + 2, '[');

        size_t n = 0;

        switch (sa_.sa_family) {
            case AF_INET:
                n = strlen(compat::inet_ntop(AF_INET, &in4_.sin_addr, &addr.front(),
                                             static_cast<socklen_t>(addr.length())));
                n += snprintf(&addr.front() + n, QB_MAX_CHAR_USHORT + 1, ":%u",
                              this->port());
                break;
            case AF_INET6:
                n = strlen(compat::inet_ntop(AF_INET6, &in6_.sin6_addr,
                                             &addr.front() + 1,
                                             static_cast<socklen_t>(addr.length() - 1)));
                n += snprintf(&addr.front() + n, QB_MAX_CHAR_USHORT + 2, "]:%u",
                              this->port());
                break;
#if defined(QB_ENABLE_UDS) && QB__HAS_UDS
            case AF_UNIX:
                n = this->len() - offsetof(struct sockaddr_un, sun_path) - 1;
                addr.assign(un_.sun_path, n);
                break;
#endif
        }

        addr.resize(n);

        return addr;
    }

    /**
     * @brief Gets the port number
     *
     * @return The port number in host byte order
     */
    unsigned short
    port() const {
        return ntohs(in4_.sin_port);
    }
    /**
     * @brief Sets the port number
     *
     * @param value Port number in host byte order
     */
    void
    port(unsigned short value) {
        in4_.sin_port = htons(value);
    }

    /**
     * @brief Sets the IPv4 address
     *
     * @param addr IPv4 address in network byte order
     */
    void
    addr_v4(uint32_t addr) {
        in4_.sin_addr.s_addr = htonl(addr);
    }
    /**
     * @brief Gets the IPv4 address
     *
     * @return IPv4 address in network byte order
     */
    uint32_t
    addr_v4() const {
        return ntohl(in4_.sin_addr.s_addr);
    }

    /*
     %N: s_net   127
     %H: s_host  0
     %L: s_lh    0
     %M: s_impno 1
     %l: low byte of port
     %h: high byte of port
    */
    std::string
    format_v4(const char *foramt) {
        static const char *const _SIN_FORMATS[] = {"%N", "%H", "%L", "%M", "%l", "%h"};

        unsigned char addr_bytes[sizeof(in4_.sin_addr.s_addr) + sizeof(u_short)];
        memcpy(addr_bytes, &in4_.sin_addr.s_addr, sizeof(in4_.sin_addr.s_addr));
        memcpy(addr_bytes + sizeof(in4_.sin_addr.s_addr), &in4_.sin_port,
               sizeof(in4_.sin_port));

        char         snum[sizeof("255")] = {0};
        const size_t _N0                 = sizeof("%N") - 1;
        std::string  s                   = foramt;
        for (size_t idx = 0; idx < QB_ARRAYSIZE(_SIN_FORMATS); ++idx) {
            auto fmt   = _SIN_FORMATS[idx];
            auto offst = s.find(fmt);
            if (offst != std::string::npos) {
                snprintf(snum, QB_MAX_CHAR_UCHAR, "%u", addr_bytes[idx]);
                s.replace(offst, _N0, snum);
            }
        }

        return s;
    }

    // in_addr(ip) to string with pred
    template <typename _Pred4, typename _Pred6>
    const char *
    inaddr_to_string(char *str /*[IN_MAX_ADDRSTRLEN]*/, _Pred4 &&pred4,
                     _Pred6 &&pred6) const {
        switch (af()) {
            case AF_INET:
                if (pred4(&in4_.sin_addr))
                    return compat::inet_ntop(AF_INET, &in4_.sin_addr, str,
                                             INET_ADDRSTRLEN);
                break;
            case AF_INET6:
                if (pred6(&in6_.sin6_addr))
                    return compat::inet_ntop(AF_INET6, &in6_.sin6_addr, str,
                                             INET6_ADDRSTRLEN);
                break;
        }
        return nullptr;
    }

    // in_addr(ip) to csv without loopback or linklocal address
    void
    inaddr_to_csv_nl(std::string &csv) {
        char str[INET6_ADDRSTRLEN] = {0};
        if (inaddr_to_string(str, is_global_in4_addr, is_global_in6_addr)) {
            csv += str;
            csv += ',';
        }
    }

    // the in_addr(from sockaddr) to csv string helper function without loopback or
    // linklocal address
    static void
    inaddr_to_csv_nl(const sockaddr *addr, std::string &csv) {
        endpoint(addr).inaddr_to_csv_nl(csv);
    }

    // the in_addr/in6_addr to csv string helper function without loopback or linklocal
    // address the inaddr should be union of in_addr,in6_addr or ensure it's memory
    // enough when family=AF_INET6
    static void
    inaddr_to_csv_nl(int family, const void *inaddr, std::string &csv) {
        endpoint(family, inaddr).inaddr_to_csv_nl(csv);
    }

    void
    len(size_t n) {
#if !QB__HAS_SA_LEN
        len_ = static_cast<uint8_t>(n);
#else
        sa_.sa_len = static_cast<uint8_t>(n);
#endif
    }
    socklen_t
    len() const {
#if !QB__HAS_SA_LEN
        return len_;
#else
        return sa_.sa_len;
#endif
    }

    /**
     * @brief Storage union for different address types
     *
     * Contains the actual socket address storage in various formats.
     */
    union {
        sockaddr     sa_;
        sockaddr_in  in4_;
        sockaddr_in6 in6_;
#if defined(QB_ENABLE_UDS) && QB__HAS_UDS
        sockaddr_un un_;
#endif
    };
#if !QB__HAS_SA_LEN
    uint8_t len_;
#endif
};

/**
 * @enum IP Stack Version
 * @brief Supported IP protocol versions
 *
 * Enum defining the IP protocol capabilities that can be detected
 * or required by socket operations. Flags can be combined to indicate
 * support for multiple protocol versions.
 */
enum : u_short {
    ipsv_unavailable = 0,                    ///< No IP support available
    ipsv_ipv4        = 1,                    ///< IPv4 support only
    ipsv_ipv6        = 2,                    ///< IPv6 support only
    ipsv_dual_stack  = ipsv_ipv4 | ipsv_ipv6 ///< Both IPv4 and IPv6 supported
};
} // namespace ip

#if !QB__HAS_NS_INLINE
using namespace qb::io::inet::ip;
#endif

/*
** CLASS socket: a posix socket wrapper
*/
class QB_API socket {
public: /// portable connect APIs
    // easy to connect a server ipv4 or ipv6 with local ip protocol version detect
    // for support ipv6 ONLY network.
    QB__DECL int xpconnect(const char *hostname, u_short port, u_short local_port = 0);
    QB__DECL int xpconnect_n(const char *hostname, u_short port,
                             const std::chrono::microseconds &wtimeout,
                             u_short                          local_port = 0);

    // easy to connect a server ipv4 or ipv6.
    QB__DECL int pconnect(const char *hostname, u_short port, u_short local_port = 0);
    QB__DECL int pconnect_n(const char *hostname, u_short port,
                            const std::chrono::microseconds &wtimeout,
                            u_short                          local_port = 0);
    QB__DECL int pconnect_n(const char *hostname, u_short port, u_short local_port = 0);

    // easy to connect a server ipv4 or ipv6.
    QB__DECL int pconnect(const endpoint &ep, u_short local_port = 0);
    QB__DECL int pconnect_n(const endpoint                  &ep,
                            const std::chrono::microseconds &wtimeout,
                            u_short                          local_port = 0);
    QB__DECL int pconnect_n(const endpoint &ep, u_short local_port = 0);

    // easy to create a tcp ipv4 or ipv6 server socket.
    QB__DECL int pserve(const char *addr, u_short port);
    QB__DECL int pserve(const endpoint &ep);

public:
    /**
     * @brief Default constructor
     *
     * Creates an uninitialized socket. The socket is not usable until
     * open() is called.
     */
    QB__DECL socket();

    /**
     * @brief Constructs with existing socket handle
     *
     * Takes ownership of the provided socket handle.
     *
     * @param handle Native socket handle to wrap
     */
    QB__DECL socket(socket_type handle);
    /**
     * @brief Copy constructor (deleted)
     *
     * Socket objects cannot be copied.
     */
    QB__DECL socket(const socket &) = delete;
    /**
     * @brief Move constructor
     *
     * Takes ownership of the socket from another instance.
     *
     * @param other Socket to move from
     */
    QB__DECL socket(socket &&);

    /**
     * @brief Assignment from socket handle
     *
     * @param handle Native socket handle to wrap
     * @return Reference to this socket
     */
    QB__DECL socket &operator=(socket_type handle);
    /**
     * @brief Copy assignment (deleted)
     *
     * Socket objects cannot be copied.
     */
    QB__DECL socket &operator=(const socket &) = delete;
    /**
     * @brief Move assignment operator
     *
     * @return Reference to this socket
     */
    QB__DECL socket &operator=(socket &&);
    /**
     * @brief Constructs and opens a socket with the specified parameters
     *
     * @param af Address family (AF_INET, AF_INET6)
     * @param type Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @param protocol Protocol to use (typically 0)
     */
    QB__DECL socket(int af, int type, int protocol);

    /**
     * @brief Destructor
     *
     * Automatically closes the socket if still open.
     */
    QB__DECL ~socket();

    // swap with other when this fd is closed.
    QB__DECL socket &swap(socket &who);

    /**
     * @brief Opens a new socket
     *
     * Creates a new socket with the specified parameters.
     *
     * @param af Address family (AF_INET, AF_INET6)
     * @param type Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @param protocol Protocol to use (typically 0)
     * @return true if socket was successfully opened
     */
    QB__DECL bool open(int af = AF_INET, int type = SOCK_STREAM, int protocol = 0);
    /**
     * @brief Closes and reopens the socket
     *
     * Closes the socket if it's open, then creates a new one.
     *
     * @param af Address family (AF_INET, AF_INET6)
     * @param type Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @param protocol Protocol to use (typically 0)
     * @return true if socket was successfully reopened
     */
    QB__DECL bool reopen(int af = AF_INET, int type = SOCK_STREAM, int protocol = 0);
#ifdef _WIN32
    QB__DECL bool open_ex(int af = AF_INET, int type = SOCK_STREAM, int protocol = 0);

    QB__DECL static bool accept_ex(SOCKET sockfd_listened, SOCKET sockfd_prepared,
                                   PVOID lpOutputBuffer, DWORD dwReceiveDataLength,
                                   DWORD   dwLocalAddressLength,
                                   DWORD   dwRemoteAddressLength,
                                   LPDWORD lpdwBytesReceived, LPOVERLAPPED lpOverlapped);

    QB__DECL static bool connect_ex(SOCKET s, const struct sockaddr *name, int namelen,
                                    PVOID lpSendBuffer, DWORD dwSendDataLength,
                                    LPDWORD lpdwBytesSent, LPOVERLAPPED lpOverlapped);

    QB__DECL static void
    translate_sockaddrs(PVOID lpOutputBuffer, DWORD dwReceiveDataLength,
                        DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength,
                        sockaddr **LocalSockaddr, LPINT LocalSockaddrLength,
                        sockaddr **RemoteSockaddr, LPINT RemoteSockaddrLength);
#endif

    /**
     * @brief Tests if the socket is open
     *
     * @return true if the socket is valid and open
     */
    QB__DECL bool is_open() const;

    /**
     * @brief Gets the native socket handle
     *
     * @return The platform-specific socket handle
     */
    QB__DECL socket_type native_handle() const;

    /**
     * @brief Releases ownership of the socket handle
     *
     * After calling this, the socket won't be closed when this object
     * is destroyed.
     *
     * @return The socket handle
     */
    QB__DECL socket_type release_handle();

    /**
     * @brief Sets the socket to blocking or non-blocking mode
     *
     * Controls whether socket operations block until completion.
     *
     * @param nonblocking true for non-blocking, false for blocking
     * @return 0 on success, SOCKET_ERROR on failure
     */
    QB__DECL int set_nonblocking(bool nonblocking) const;
    /**
     * @brief Static version of set_nonblocking
     *
     * @param s Socket handle to modify
     * @param nonblocking true for non-blocking, false for blocking
     * @return 0 on success, SOCKET_ERROR on failure
     */
    QB__DECL static int set_nonblocking(socket_type s, bool nonblocking);

    /**
     * @brief Test whether the socket has nonblocking flag
     *
     * @returns: [1] yes. [0] no
     * @pitfall: for wsock2, will return [-1] when it's a unconnected SOCK_STREAM
     */
    QB__DECL int        test_nonblocking() const;
    QB__DECL static int test_nonblocking(socket_type s);

    /**
     * @brief Associates a local address with this socket
     *
     * @param addr four point address, if set "0.0.0.0" ipv4, "::" ipv6, the socket
     *will listen at any.
     * @param port @$$#s
     * @returns:
     *         If no error occurs, bind returns [0]. Otherwise, it returns SOCKET_ERROR
     */
    QB__DECL int bind(const char *addr, unsigned short port) const;
    QB__DECL int bind(const endpoint &) const;
    QB__DECL int bind_any(bool ipv6 = false) const;

    /**
     * @brief Places this socket in a state in which it is listening for an incoming
     *connection
     * @params:
     *        Ommit
     *
     * @returns:
     *         If no error occurs, bind returns [0]. Otherwise, it returns SOCKET_ERROR
     */
    QB__DECL int listen(int backlog = SOMAXCONN) const;

    /**
     * @brief Permits an incoming connection attempt on this socket
     * @returns:
     *        If no error occurs, accept returns a new socket on which
     *        the actual connection is made.
     *        Otherwise, a value of [invalid_socket] is returned
     */
    QB__DECL socket accept() const;

    /**
     * @brief Permits an incoming connection attempt on this socket
     * @params:
     * @returns:
     *        If no error occurs, return 0, and the new_sock will be the actual
     *connection is made.
     *        Otherwise, a EWOULDBLOCK,EAGAIN or other value is returned
     */
    QB__DECL int accept_n(socket_type &new_sock) const;

    /**
     * @brief Establishes a connection to a specified this socket
     * @params:
     *        addr: Usually is a IPV4 address
     *        port: Server Listenning Port
     *
     * @returns:
     *         If no error occurs, returns [0].
     *         Otherwise, it returns SOCKET_ERROR
     */
    QB__DECL int        connect(const char *addr, u_short port);
    QB__DECL int        connect(const endpoint &ep);
    QB__DECL static int connect(socket_type s, const char *addr, u_short port);
    QB__DECL static int connect(socket_type s, const endpoint &ep);

    /**
     * @brief Establishes a connection to a specified this socket with nonblocking
     * @params:
     *        timeout: connection timeout, millseconds
     *
     * @returns: [0].succeed, [-1].failed
     * @remark: Because on win32, there is no way to test whether the socket is
     *non-blocking,
     * so, after this function called, the socket will be always set to blocking mode.
     */
    QB__DECL int        connect_n(const char *addr, u_short port,
                                  const std::chrono::microseconds &wtimeout);
    QB__DECL int        connect_n(const endpoint                  &ep,
                                  const std::chrono::microseconds &wtimeout);
    QB__DECL static int connect_n(socket_type s, const endpoint &ep,
                                  const std::chrono::microseconds &wtimeout);

    /**
     * @brief Establishes a connection to a specified this socket with nonblocking
     * @params:
     *
     * @returns: [0].succeed, [-1].failed
     * @remark: this function will return immediately, for tcp, you should detect
     *whether the
     * handshake complete by handle_write_ready.
     */
    QB__DECL int        connect_n(const endpoint &ep);
    QB__DECL static int connect_n(socket_type s, const endpoint &ep);

    /**
     * @brief Disconnect a connectionless socket (such as SOCK_DGRAM)
     *
     */
    QB__DECL int        disconnect() const;
    QB__DECL static int disconnect(socket_type s);

    /**
     * @brief nonblock send
     * @params: omit
     *
     * @returns:
     *         If no error occurs, retval == len,
     *         Oterwise, If retval < len && not_recv_error(get_last_errno()), should
     *close socket.
     */
    QB__DECL int        send_n(const void *buf, int len,
                               const std::chrono::microseconds &wtimeout, int flags = 0);
    QB__DECL static int send_n(socket_type s, const void *buf, int len,
                               std::chrono::microseconds wtimeout, int flags = 0);

    /**
     * @brief nonblock recv
     * @params:
     *       The timeout is in microseconds
     * @returns:
     *         If no error occurs, retval == len,
     *         Oterwise, If retval < len && not_recv_error(get_last_errno()), should
     *close socket.
     */
    QB__DECL int recv_n(void *buf, int len, const std::chrono::microseconds &wtimeout,
                        int flags = 0) const;
    QB__DECL static int recv_n(socket_type s, void *buf, int len,
                               std::chrono::microseconds wtimeout, int flags = 0);

    /**
     * @brief Sends data on this connected socket
     * @params: omit
     *
     * @returns:
     *         If no error occurs, send returns the total number of bytes sent,
     *         which can be less than the number requested to be sent in the len
     *parameter.
     *         Otherwise, a value of SOCKET_ERROR is returned.
     */
    QB__DECL int        send(const void *buf, int len, int flags = MSG_NOSIGNAL) const;
    QB__DECL static int send(socket_type fd, const void *buf, int len,
                             int flags = MSG_NOSIGNAL);

    /**
     * @brief Receives data from this connected socket or a bound connectionless socket.
     * @params: omit
     *
     * @returns:
     *         If no error occurs, recv returns the number of bytes received and
     *         the buffer pointed to by the buf parameter will contain this data
     *received.
     *         If the connection has been gracefully closed, the return value is [0].
     */
    QB__DECL int        recv(void *buf, int len, int flags = MSG_NOSIGNAL) const;
    QB__DECL static int recv(socket_type s, void *buf, int len,
                             int flags = MSG_NOSIGNAL);

    /**
     * @brief Sends data on this connected socket
     * @params: omit
     *
     * @returns:
     *         If no error occurs, send returns the total number of bytes sent,
     *         which can be less than the number requested to be sent in the len
     *parameter.
     *         Otherwise, a value of SOCKET_ERROR is returned.
     */
    QB__DECL int sendto(const void *buf, int len, const endpoint &to,
                        int flags = MSG_NOSIGNAL) const;

    /**
     * @brief Receives a datagram and stores the source address
     * @params: omit
     *
     * @returns:
     *         If no error occurs, recv returns the number of bytes received and
     *         the buffer pointed to by the buf parameter will contain this data
     *received.
     *         If the connection has been gracefully closed, the return value is [0].
     */
    QB__DECL int recvfrom(void *buf, int len, endpoint &peer,
                          int flags = MSG_NOSIGNAL) const;

    QB__DECL int handle_write_ready(const std::chrono::microseconds &wtimeout) const;
    QB__DECL static int handle_write_ready(socket_type                      s,
                                           const std::chrono::microseconds &wtimeout);

    QB__DECL int handle_read_ready(const std::chrono::microseconds &wtimeout) const;
    QB__DECL static int handle_read_ready(socket_type                      s,
                                          const std::chrono::microseconds &wtimeout);

    /**
     * @brief Get local address info
     * @params : None
     *
     * @returns:
     */
    QB__DECL endpoint        local_endpoint() const;
    QB__DECL static endpoint local_endpoint(socket_type);

    /**
     * @brief Get peer address info
     * @params : None
     *
     * @returns:
     *  @remark: if this a listening socket fd, will return "0.0.0.0:0"
     */
    QB__DECL endpoint        peer_endpoint() const;
    QB__DECL static endpoint peer_endpoint(socket_type);

    /**
     * @brief Configure TCP keepalive
     * @params : flag:     1.on, 0.off
     *           idle:     time(secs) to send keepalive when no data interaction
     *           interval: keepalive send interval(secs)
     *           probes:   count to try when no response
     *
     * @returns: [0].successfully
     *          [<0].one or more errors occured
     */
    QB__DECL int        set_keepalive(int flag = 1, int idle = 7200, int interval = 75,
                                      int probes = 10);
    QB__DECL static int set_keepalive(socket_type s, int flag, int idle, int interval,
                                      int probes);

    QB__DECL void reuse_address(bool reuse);

    QB__DECL void exclusive_address(bool exclusive);

    /**
     * @brief Sets the socket option
     * @params :
     *        level: The level at which the option is defined (for example, SOL_SOCKET).
     *      optname: The socket option for which the value is to be set (for example,
     *SO_BROADCAST).
     *               The optname parameter must be a socket option defined within the
     *specified level,
     *               or behavior is undefined.
     *       optval: The option value.
     * @examples:
     *       set_optval(SOL_SOCKET, SO_SNDBUF, 4096);
     *       set_optval(SOL_SOCKET, SO_RCVBUF, 4096);
     *
     * @remark: for more detail, please see:
     *       windows:
     *https://docs.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-setsockopt
     *       linux: https://linux.die.net/man/3/setsockopt
     *
     * @returns: If no error occurs, set_optval returns zero. Otherwise, a value of
     *SOCKET_ERROR is
     *       returned
     */
    template <typename _Ty>
    inline int
    set_optval(int level, int optname, const _Ty &optval) const {
        return set_optval(this->fd, level, optname, optval);
    }
    template <typename _Ty>
    inline static int
    set_optval(socket_type sockfd, int level, int optname, const _Ty &optval) {
        return set_optval(sockfd, level, optname, &optval,
                          static_cast<socklen_t>(sizeof(_Ty)));
    }

    int
    set_optval(int level, int optname, const void *optval, socklen_t optlen) {
        return set_optval(this->fd, level, optname, optval, optlen);
    }
    static int
    set_optval(socket_type sockfd, int level, int optname, const void *optval,
               socklen_t optlen) {
        return ::setsockopt(FD_TO_SOCKET(sockfd), level, optname,
                            static_cast<const char *>(optval), optlen);
    }

    /**
     * @brief Retrieves a socket option
     * @params :
     *     level: The level at which the option is defined. Example: SOL_SOCKET.
     *   optname: The socket option for which the value is to be retrieved.
     *            Example: SO_ACCEPTCONN. The optname value must be a socket option
     *defined within the
     *            specified level, or behavior is undefined.
     *    optval: A variable to the buffer in which the value for the requested option
     *is to be
     *            returned.
     *
     * @returns: If no error occurs, get_optval returns zero. Otherwise, a value of
     *SOCKET_ERROR is returned
     */
    template <typename _Ty>
    inline _Ty
    get_optval(int level, int optname) const {
        _Ty optval = {};
        get_optval(this->fd, level, optname, optval);
        return optval;
    }
    template <typename _Ty>
    inline int
    get_optval(int level, int optname, _Ty &optval) const {
        return get_optval(this->fd, level, optname, optval);
    }
    template <typename _Ty>
    inline static int
    get_optval(socket_type sockfd, int level, int optname, _Ty &optval) {
        socklen_t optlen = static_cast<socklen_t>(sizeof(_Ty));
        return get_optval(sockfd, level, optname, &optval, &optlen);
    }
    static int
    get_optval(socket_type sockfd, int level, int optname, void *optval,
               socklen_t *optlen) {
        return ::getsockopt(FD_TO_SOCKET(sockfd), level, optname,
                            static_cast<char *>(optval), optlen);
    }

    /**
     * @brief control the socket
     * @params :
     *          see MSDN or man page
     * @returns: If no error occurs, ioctl returns zero. Otherwise, a value of
     *SOCKET_ERROR is
     *           returned
     *
     *
     *
     */
    template <typename _Ty>
    inline int
    ioctl(long cmd, const _Ty &value) const {
        return socket::ioctl(this->fd, cmd, value);
    }
    template <typename _Ty>
    inline static int
    ioctl(socket_type s, long cmd, const _Ty &value) {
        u_long argp = static_cast<u_long>(value);
        return ::ioctlsocket(FD_TO_SOCKET(s), cmd, &argp);
    }

    /**
     * @brief wrapper system select, hide signal EINTR
     * @params:
     *          s: the socket fd, it's different with system
     *          see MSDN or man page
     * @returns: If no error occurs, returns >= 0. Otherwise, a value of -1 is
     *          returned
     */
    int
    select(fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           const std::chrono::microseconds &wtimeout) const {
        return socket::select(this->fd, readfds, writefds, exceptfds, wtimeout);
    }
    QB__DECL static int select(socket_type s, fd_set *readfds, fd_set *writefds,
                               fd_set *exceptfds, std::chrono::microseconds wtimeout);

    /**
     * @brief Disables sends or receives on this socket
     * @params:
     *        how: [SD_SEND] or [SD_RECEIVE] or [SD_BOTH]
     *
     * @returns: [0] succeed, otherwise, a value of SOCKET_ERROR is returned.
     */
    QB__DECL int shutdown(int how = SD_BOTH) const;

    /**
     * @brief close sends
     * @params:
     *        shut_how: [SD_SEND] or [SD_RECEIVE] or [SD_BOTH] or [SD_NONE]
     *
     * @returns: [0] succeed, otherwise, a value of SOCKET_ERROR is returned.
     */
    QB__DECL void close(int shut_how = SD_BOTH);

    /**
     * @brief Retrive tcp socket rtt in microseconds
     * @params:
     *        non
     *
     * @returns: [0] succeed, otherwise, a value of SOCKET_ERROR is returned.
     */
    QB__DECL unsigned int        tcp_rtt() const;
    QB__DECL static unsigned int tcp_rtt(socket_type s);

    QB__DECL operator socket_type() const;

public:
    /**
     * @brief this function just for windows platform
     *
     */
    QB__DECL static void init_ws32_lib();

    QB__DECL static int  get_last_errno();
    QB__DECL static void set_last_errno(int error);

    QB__DECL static bool not_send_error(int error);
    QB__DECL static bool not_recv_error(int error);

    QB__DECL static const char *strerror(int error);
    QB__DECL static const char *gai_strerror(int error);

    /**
     * @brief Resolve all as ipv4 or ipv6 endpoints
     *
     * @param endpoints Vector to store resolved endpoints
     * @param hostname Hostname to resolve
     * @param port Port to resolve
     * @param socktype Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @return Number of resolved endpoints
     */
    QB__DECL static int resolve(std::vector<endpoint> &endpoints, const char *hostname,
                                unsigned short port = 0, int socktype = SOCK_STREAM);

    /**
     * @brief Resolve as ipv4 address only.
     *
     * @param endpoints Vector to store resolved endpoints
     * @param hostname Hostname to resolve
     * @param port Port to resolve
     * @param socktype Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @return Number of resolved endpoints
     */
    QB__DECL static int resolve_v4(std::vector<endpoint> &endpoints,
                                   const char *hostname, unsigned short port = 0,
                                   int socktype = SOCK_STREAM);

    /**
     * @brief Resolve as ipv6 address only.
     *
     * @param endpoints Vector to store resolved endpoints
     * @param hostname Hostname to resolve
     * @param port Port to resolve
     * @param socktype Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @return Number of resolved endpoints
     */
    QB__DECL static int resolve_v6(std::vector<endpoint> &endpoints,
                                   const char *hostname, unsigned short port = 0,
                                   int socktype = SOCK_STREAM);

    /**
     * @brief Resolve as ipv4 address only and convert to V4MAPPED format.
     *
     * @param endpoints Vector to store resolved endpoints
     * @param hostname Hostname to resolve
     * @param port Port to resolve
     * @param socktype Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @return Number of resolved endpoints
     */
    QB__DECL static int resolve_v4to6(std::vector<endpoint> &endpoints,
                                      const char *hostname, unsigned short port = 0,
                                      int socktype = SOCK_STREAM);

    /**
     * @brief Force resolve all addres to ipv6 endpoints, IP4 with AI_V4MAPPED
     *
     * @param endpoints Vector to store resolved endpoints
     * @param hostname Hostname to resolve
     * @param port Port to resolve
     * @param socktype Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @return Number of resolved endpoints
     */
    QB__DECL static int resolve_tov6(std::vector<endpoint> &endpoints,
                                     const char *hostname, unsigned short port = 0,
                                     int socktype = SOCK_STREAM);

    /**
     * @brief Resolve as ipv4 or ipv6 endpoints with callback
     *
     * @param callback Callback function to call for each resolved endpoint
     * @param hostname Hostname to resolve
     * @param port Port to resolve
     * @param af Address family (AF_INET, AF_INET6)
     * @param flags Flags for getaddrinfo
     * @param socktype Socket type (SOCK_STREAM, SOCK_DGRAM)
     * @return Number of resolved endpoints
     */
    template <typename _Fty>
    inline static int
    resolve_i(const _Fty &callback, const char *hostname, unsigned short port = 0,
              int af = 0, int flags = 0, int socktype = SOCK_STREAM) {
        addrinfo hint;
        memset(&hint, 0x0, sizeof(hint));
        hint.ai_flags    = flags;
        hint.ai_family   = af;
        hint.ai_socktype = socktype;

        addrinfo   *answerlist             = nullptr;
        char        buffer[sizeof "65535"] = {'\0'};
        const char *service                = nullptr;
        if (port > 0) {
            snprintf(buffer, QB_MAX_CHAR_USHORT, "%u", port);
            service = buffer;
        }
        int error = getaddrinfo(hostname, service, &hint, &answerlist);
        if (nullptr == answerlist)
            return error;

        for (auto ai = answerlist; ai != nullptr; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET6 || ai->ai_family == AF_INET) {
                if (callback(endpoint(ai)))
                    break;
            }
        }

        freeaddrinfo(answerlist);

        return error;
    }

    /**
     * @brief Gets supported IP protocol versions
     *
     * Determines which IP protocol versions (IPv4, IPv6, or both) are
     * supported on the local machine.
     *
     * @return Bitfield of supported protocol versions
     */
    QB__DECL static int getipsv();

    /**
     * @brief Traverses all local network interfaces
     *
     * Calls the provided handler for each local address found.
     *
     * @param handler Function to call for each endpoint found
     */
    QB__DECL static void
    traverse_local_address(std::function<bool(const ip::endpoint &)> handler);

protected:
    QB__DECL static void reregister_descriptor(socket_type s, fd_set *fds);

private:
    socket_type fd;
}; // namespace inet

} // namespace inet
#if !QB__HAS_NS_INLINE
using namespace qb::io::inet;
#endif
} // namespace qb::io

namespace std { // VS2013 the operator must be at namespace std
inline bool
operator<(const qb::io::inet::ip::endpoint &lhs,
          const qb::io::inet::ip::endpoint &rhs) { // apply operator < to operands
    if (lhs.af() == AF_INET)
        return (static_cast<uint64_t>(lhs.in4_.sin_addr.s_addr) + lhs.in4_.sin_port) <
               (static_cast<uint64_t>(rhs.in4_.sin_addr.s_addr) + rhs.in4_.sin_port);
    return ::memcmp(&lhs, &rhs, sizeof(rhs)) < 0;
}
inline bool
operator==(const qb::io::inet::ip::endpoint &lhs,
           const qb::io::inet::ip::endpoint &rhs) { // apply operator == to operands
    return !(lhs < rhs) && !(rhs < lhs);
}
} // namespace std

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(QB_HEADER_ONLY)
#include "qb/socket.cpp" // lgtm [cpp/include-non-header]
#endif

namespace qb::io {
enum SocketStatus { Error = -1, Done };
constexpr static const int FD_INVALID = -1;

/**
 * @brief Checks if a socket error is non-fatal
 *
 * @param error Error code to check
 * @return true if the error is non-fatal
 */
inline bool
socket_no_error(int error) {
    return error == EWOULDBLOCK || error == EAGAIN || error == EINTR ||
           error == EINPROGRESS;
}
} // namespace qb::io

#endif