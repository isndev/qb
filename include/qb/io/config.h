/**
 * @file qb/io/config.h
 * @brief Configuration and platform-specific definitions for the QB IO library
 * 
 * This file provides platform-specific definitions, macros, and configuration
 * options for the QB IO library. It handles cross-platform compatibility issues,
 * defines platform-specific settings, and includes the necessary system headers
 * for socket programming on different operating systems.
 * 
 * The file includes macros for configuring features like Unix Domain Sockets,
 * header-only implementation, and compatibility settings for different platforms.
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
 * @ingroup IO
 */

#include <qb/utility/build_macros.h>

#ifndef QB_IO_CONFIG_HPP
#    define QB_IO_CONFIG_HPP
/*
** Uncomment or add compiler flag -DQB_HEADER_ONLY to enable qb core implementation
*header
** only
*/
// #define QB_HEADER_ONLY 1

/*
** Uncomment or add compiler flag -DQB_ENABLE_UDS to enable unix domain socket via
*SOCK_STREAM
*/
#    ifndef QB_ENABLE_UDS
#        define QB_ENABLE_UDS 1
#    endif

/*
** Uncomment or add compiler flag -DQB_NT_COMPAT_GAI for earlier versions of Windows XP
** see:
*https://docs.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfo
*/
// #define QB_NT_COMPAT_GAI 1

#    if defined(QB_HEADER_ONLY)
#        define QB__DECL inline
#    else
#        define QB__DECL
#    endif

/*
** The interop decl, it's useful for store managed c# function as c++ function pointer
*properly.
*/
#    if !defined(_WIN32) || QB__64BITS
#        define QB_INTEROP_DECL
#    else
#        define QB_INTEROP_DECL __stdcall
#    endif

#    define QB_ARRAYSIZE(A) (sizeof(A) / sizeof((A)[0]))
#    define QB_SSIZEOF(T) static_cast<int>(sizeof(T))

// clang-format off
/*
** QB_OBSOLETE_DEPRECATE
*/
#if defined(__GNUC__) && ((__GNUC__ >= 4) || ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 1)))
#  define QB_OBSOLETE_DEPRECATE(_Replacement) __attribute__((deprecated))
#elif _MSC_VER >= 1400 // vs 2005 or higher
#  define QB_OBSOLETE_DEPRECATE(_Replacement) \
    __declspec(deprecated("This function will be removed in the future. Consider using " #_Replacement " instead."))
#else
#  define QB_OBSOLETE_DEPRECATE(_Replacement)
#endif
// clang-format on

#    if defined(UE_BUILD_DEBUG) || defined(UE_BUILD_DEVELOPMENT) || \
        defined(UE_BUILD_TEST) || defined(UE_BUILD_SHIPPING) || defined(UE_SERVER)
#        define QB_INSIDE_UNREAL 1
#    endif // Unreal Engine 4 bullshit

/*
**  The qb version macros
*/
#    define QB_VERSION_NUM 0x033705

// The default ttl of multicast
#    define QB_DEFAULT_MULTICAST_TTL (int)128

// The max internet buffer size
#    define QB_INET_BUFFER_SIZE 65536

// The max pdu buffer length, avoid large memory allocation when application decode a
// huge length.
#    define QB_MAX_PDU_BUFFER_SIZE static_cast<int>(1 * 1024 * 1024)

// The max Initial Bytes To Strip for unpack.
#    define QB_UNPACK_MAX_STRIP 32

#    ifdef _WIN32
#        if !defined(WIN32_LEAN_AND_MEAN)
#            define WIN32_LEAN_AND_MEAN
#        endif
#        ifndef NOMINMAX
#            define NOMINMAX
#        endif
#        include <WinSock2.h>
#        include <Windows.h>
#        include <io.h>
#        if defined(_WIN32) && !defined(_WINSTORE)
#            include <Mstcpip.h>
#            include <Mswsock.h>
#        endif
#        include <Ws2tcpip.h>
#        if defined(QB_NT_COMPAT_GAI)
#            include <Wspiapi.h>
#        endif
#        if QB__HAS_UDS
#            include <afunix.h>
#        endif
typedef int socket_type;
typedef int socklen_t;
#        define FD_TO_SOCKET(fd) _get_osfhandle(fd)
#        define OPEN_FD_FROM_SOCKET(sock) _open_osfhandle(sock, 0)
#        define poll WSAPoll
#        pragma comment(lib, "ws2_32.lib")

#        undef gai_strerror
#    else
#        include <netdb.h>
#        include <signal.h>
#        include <sys/ioctl.h>
#        include <sys/poll.h>
#        include <sys/types.h>
#        include <unistd.h>
#        if defined(__linux__)
#            include <sys/epoll.h>
#        endif
#        include <arpa/inet.h>
#        include <net/if.h>
#        include <netinet/in.h>
#        include <netinet/tcp.h>
#        include <sys/select.h>
#        include <sys/socket.h>
#        include <sys/un.h>
#        if !defined(SD_RECEIVE)
#            define SD_RECEIVE SHUT_RD
#        endif
#        if !defined(SD_SEND)
#            define SD_SEND SHUT_WR
#        endif
#        if !defined(SD_BOTH)
#            define SD_BOTH SHUT_RDWR
#        endif
#        if !defined(closesocket)
#            define closesocket close
#        endif
#        if !defined(ioctlsocket)
#            define ioctlsocket ioctl
#        endif
#        if defined(__linux__)
#            define SO_NOSIGPIPE MSG_NOSIGNAL
#        endif
typedef int socket_type;
#        define FD_TO_SOCKET(fd) fd
#        define OPEN_FD_FROM_SOCKET(sock) sock
#        undef socket
#    endif
#    define SD_NONE -1

#    include <fcntl.h> // common platform header

// redefine socket error code for posix api
#    ifdef _WIN32
#        undef EWOULDBLOCK
#        undef EINPROGRESS
#        undef EALREADY
#        undef ENOTSOCK
#        undef EDESTADDRREQ
#        undef EMSGSIZE
#        undef EPROTOTYPE
#        undef ENOPROTOOPT
#        undef EPROTONOSUPPORT
#        undef ESOCKTNOSUPPORT
#        undef EOPNOTSUPP
#        undef EPFNOSUPPORT
#        undef EAFNOSUPPORT
#        undef EADDRINUSE
#        undef EADDRNOTAVAIL
#        undef ENETDOWN
#        undef ENETUNREACH
#        undef ENETRESET
#        undef ECONNABORTED
#        undef ECONNRESET
#        undef ENOBUFS
#        undef EISCONN
#        undef ENOTCONN
#        undef ESHUTDOWN
#        undef ETOOMANYREFS
#        undef ETIMEDOUT
#        undef ECONNREFUSED
#        undef ELOOP
#        undef ENAMETOOLONG
#        undef EHOSTDOWN
#        undef EHOSTUNREACH
#        undef ENOTEMPTY
#        undef EPROCLIM
#        undef EUSERS
#        undef EDQUOT
#        undef ESTALE
#        undef EREMOTE
#        undef EBADF
#        undef EFAULT
#        undef EAGAIN

#        define EWOULDBLOCK WSAEWOULDBLOCK
#        define EINPROGRESS WSAEINPROGRESS
#        define EALREADY WSAEALREADY
#        define ENOTSOCK WSAENOTSOCK
#        define EDESTADDRREQ WSAEDESTADDRREQ
#        define EMSGSIZE WSAEMSGSIZE
#        define EPROTOTYPE WSAEPROTOTYPE
#        define ENOPROTOOPT WSAENOPROTOOPT
#        define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#        define ESOCKTNOSUPPORT WSAESOCKTNOSUPPORT
#        define EOPNOTSUPP WSAEOPNOTSUPP
#        define EPFNOSUPPORT WSAEPFNOSUPPORT
#        define EAFNOSUPPORT WSAEAFNOSUPPORT
#        define EADDRINUSE WSAEADDRINUSE
#        define EADDRNOTAVAIL WSAEADDRNOTAVAIL
#        define ENETDOWN WSAENETDOWN
#        define ENETUNREACH WSAENETUNREACH
#        define ENETRESET WSAENETRESET
#        define ECONNABORTED WSAECONNABORTED
#        define ECONNRESET WSAECONNRESET
#        define ENOBUFS WSAENOBUFS
#        define EISCONN WSAEISCONN
#        define ENOTCONN WSAENOTCONN
#        define ESHUTDOWN WSAESHUTDOWN
#        define ETOOMANYREFS WSAETOOMANYREFS
#        define ETIMEDOUT WSAETIMEDOUT
#        define ECONNREFUSED WSAECONNREFUSED
#        define ELOOP WSAELOOP
#        define ENAMETOOLONG WSAENAMETOOLONG
#        define EHOSTDOWN WSAEHOSTDOWN
#        define EHOSTUNREACH WSAEHOSTUNREACH
#        define ENOTEMPTY WSAENOTEMPTY
#        define EPROCLIM WSAEPROCLIM
#        define EUSERS WSAEUSERS
#        define EDQUOT WSAEDQUOT
#        define ESTALE WSAESTALE
#        define EREMOTE WSAEREMOTE
#        define EBADF WSAEBADF
#        define EFAULT WSAEFAULT
#        define EAGAIN WSATRY_AGAIN
#    endif

#    if !defined(MAXNS)
#        define MAXNS 3
#    endif

#    define IN_MAX_ADDRSTRLEN INET6_ADDRSTRLEN

#endif