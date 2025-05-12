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
#define QB_IO_CONFIG_HPP
/*
** Uncomment or add compiler flag -DQB_HEADER_ONLY to enable qb core implementation
*header
** only
*/
// #define QB_HEADER_ONLY 1

/**
 * @def QB_ENABLE_UDS
 * @brief Enables Unix Domain Socket support via SOCK_STREAM
 * @details When defined as 1, enables support for Unix Domain Sockets which
 * provide efficient inter-process communication on Unix-like systems
 * @ingroup IO
 */
#ifndef QB_ENABLE_UDS
#define QB_ENABLE_UDS 1
#endif

/*
** Uncomment or add compiler flag -DQB_NT_COMPAT_GAI for earlier versions of Windows XP
** see:
*https://docs.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfo
*/
// #define QB_NT_COMPAT_GAI 1

/**
 * @def QB__DECL
 * @brief Function declaration specifier that changes based on whether header-only mode is enabled
 * @details When QB_HEADER_ONLY is defined, functions are marked as inline
 * @ingroup IO
 */
#if defined(QB_HEADER_ONLY)
#define QB__DECL inline
#else
#define QB__DECL
#endif

/**
 * @def QB_INTEROP_DECL
 * @brief Interoperability declaration for function pointers
 * @details Used for properly storing managed C# functions as C++ function pointers
 * Uses __stdcall on 32-bit Windows platforms
 * @ingroup IO
 */
#if !defined(_WIN32) || QB__64BITS
#define QB_INTEROP_DECL
#else
#define QB_INTEROP_DECL __stdcall
#endif

/**
 * @def QB_ARRAYSIZE(A)
 * @brief Macro to calculate the number of elements in a statically-allocated array
 * @param A Array to determine the size of
 * @return Number of elements in the array
 * @ingroup IO
 */
#define QB_ARRAYSIZE(A) (sizeof(A) / sizeof((A)[0]))

/**
 * @def QB_SSIZEOF(T)
 * @brief Macro to get the size of a type as a signed integer
 * @param T Type to get the size of
 * @return Size of the type as a signed integer
 * @ingroup IO
 */
#define QB_SSIZEOF(T) static_cast<int>(sizeof(T))

// clang-format off
/**
 * @def QB_OBSOLETE_DEPRECATE(_Replacement)
 * @brief Marks functions as deprecated with a replacement suggestion
 * @details Cross-platform macro that applies the appropriate compiler-specific
 * deprecation annotation. On GCC/Clang, it uses __attribute__((deprecated)),
 * on MSVC it uses __declspec(deprecated) with a message suggesting the replacement.
 * @param _Replacement The recommended replacement function name
 * @ingroup IO
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

#if defined(UE_BUILD_DEBUG) || defined(UE_BUILD_DEVELOPMENT) || \
    defined(UE_BUILD_TEST) || defined(UE_BUILD_SHIPPING) || defined(UE_SERVER)
#define QB_INSIDE_UNREAL 1
#endif // Unreal Engine 4 bullshit

/*
**  The qb version macros
*/
/**
 * @def QB_VERSION_NUM
 * @brief Defines the QB library version number in hexadecimal format
 * @details Format is 0xMMNNRR where MM=major, NN=minor, RR=revision
 * @ingroup IO
 */
#define QB_VERSION_NUM 0x033705

/**
 * @def QB_DEFAULT_MULTICAST_TTL
 * @brief Default Time-To-Live value for multicast packets
 * @details Sets the default number of hops a multicast packet can traverse
 * @ingroup IO
 */
#define QB_DEFAULT_MULTICAST_TTL (int) 128

/**
 * @def QB_INET_BUFFER_SIZE
 * @brief Maximum size for internet protocol buffers
 * @details Defines the maximum buffer size for TCP/IP communication (65536 bytes)
 * @ingroup IO
 */
#define QB_INET_BUFFER_SIZE 65536

/**
 * @def QB_MAX_PDU_BUFFER_SIZE
 * @brief Maximum Protocol Data Unit buffer size
 * @details Limits the size of PDU buffers to avoid large memory allocations
 * when decoding (1MB)
 * @ingroup IO
 */
#define QB_MAX_PDU_BUFFER_SIZE static_cast<int>(1 * 1024 * 1024)

/**
 * @def QB_UNPACK_MAX_STRIP
 * @brief Maximum number of initial bytes that can be stripped during unpacking
 * @details Limits the number of bytes that can be removed from the beginning
 * of a message during protocol unpacking operations
 * @ingroup IO
 */
#define QB_UNPACK_MAX_STRIP 32

#ifdef _WIN32
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>
#include <io.h>
#if defined(_WIN32) && !defined(_WINSTORE)
#include <Mstcpip.h>
#include <Mswsock.h>
#endif
#include <Ws2tcpip.h>
#if defined(QB_NT_COMPAT_GAI)
#include <Wspiapi.h>
#endif
#if QB__HAS_UDS
#include <afunix.h>
#endif
typedef SOCKET socket_type;
typedef int socklen_t;
#define FD_TO_SOCKET(fd) _get_osfhandle(fd)
#define OPEN_FD_FROM_SOCKET(sock) _open_osfhandle(sock, 0)
#define poll WSAPoll
#pragma comment(lib, "ws2_32.lib")

#undef gai_strerror
#else
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#endif
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#if !defined(SD_RECEIVE)
/**
 * @def SD_RECEIVE
 * @brief Socket shutdown flag for disabling receive operations
 * @details Cross-platform macro that maps to platform-specific constants
 * (SHUT_RD on Unix, SD_RECEIVE on Windows)
 * @ingroup IO
 */
#define SD_RECEIVE SHUT_RD
#endif
#if !defined(SD_SEND)
/**
 * @def SD_SEND
 * @brief Socket shutdown flag for disabling send operations
 * @details Cross-platform macro that maps to platform-specific constants
 * (SHUT_WR on Unix, SD_SEND on Windows)
 * @ingroup IO
 */
#define SD_SEND SHUT_WR
#endif
#if !defined(SD_BOTH)
/**
 * @def SD_BOTH
 * @brief Socket shutdown flag for disabling both send and receive operations
 * @details Cross-platform macro that maps to platform-specific constants
 * (SHUT_RDWR on Unix, SD_BOTH on Windows)
 * @ingroup IO
 */
#define SD_BOTH SHUT_RDWR
#endif
#if !defined(closesocket)
/**
 * @def closesocket
 * @brief Cross-platform macro for closing a socket
 * @details Maps to close() on Unix systems, keeps closesocket() on Windows
 * @ingroup IO
 */
#define closesocket close
#endif
#if !defined(ioctlsocket)
/**
 * @def ioctlsocket
 * @brief Cross-platform macro for socket I/O control
 * @details Maps to ioctl() on Unix systems, keeps ioctlsocket() on Windows
 * @ingroup IO
 */
#define ioctlsocket ioctl
#endif
#if defined(__linux__)
#define SO_NOSIGPIPE MSG_NOSIGNAL
#endif
/**
 * @typedef socket_type
 * @brief Cross-platform socket handle type
 * @details int on Unix systems, SOCKET (unsigned integer) on Windows
 * @ingroup IO
 */
typedef int socket_type;
/**
 * @def FD_TO_SOCKET(fd)
 * @brief Converts a file descriptor to a socket handle
 * @details On Unix, returns fd unchanged. On Windows, converts using _get_osfhandle()
 * @param fd File descriptor to convert
 * @return Equivalent socket handle
 * @ingroup IO
 */
#define FD_TO_SOCKET(fd) fd
/**
 * @def OPEN_FD_FROM_SOCKET(sock)
 * @brief Converts a socket handle to a file descriptor
 * @details On Unix, returns sock unchanged. On Windows, converts using _open_osfhandle()
 * @param sock Socket handle to convert
 * @return Equivalent file descriptor
 * @ingroup IO
 */
#define OPEN_FD_FROM_SOCKET(sock) sock
#undef socket
#endif
/**
 * @def SD_NONE
 * @brief Special value indicating no socket shutdown operation
 * @details Used to indicate that no shutdown operation should be performed
 * @ingroup IO
 */
#define SD_NONE -1

#include <fcntl.h> // common platform header

// redefine socket error code for posix api
#ifdef _WIN32
#undef EWOULDBLOCK
#undef EINPROGRESS
#undef EALREADY
#undef ENOTSOCK
#undef EDESTADDRREQ
#undef EMSGSIZE
#undef EPROTOTYPE
#undef ENOPROTOOPT
#undef EPROTONOSUPPORT
#undef ESOCKTNOSUPPORT
#undef EOPNOTSUPP
#undef EPFNOSUPPORT
#undef EAFNOSUPPORT
#undef EADDRINUSE
#undef EADDRNOTAVAIL
#undef ENETDOWN
#undef ENETUNREACH
#undef ENETRESET
#undef ECONNABORTED
#undef ECONNRESET
#undef ENOBUFS
#undef EISCONN
#undef ENOTCONN
#undef ESHUTDOWN
#undef ETOOMANYREFS
#undef ETIMEDOUT
#undef ECONNREFUSED
#undef ELOOP
#undef ENAMETOOLONG
#undef EHOSTDOWN
#undef EHOSTUNREACH
#undef ENOTEMPTY
#undef EPROCLIM
#undef EUSERS
#undef EDQUOT
#undef ESTALE
#undef EREMOTE
#undef EBADF
#undef EFAULT
#undef EAGAIN

#define EWOULDBLOCK WSAEWOULDBLOCK
#define EINPROGRESS WSAEINPROGRESS
#define EALREADY WSAEALREADY
#define ENOTSOCK WSAENOTSOCK
#define EDESTADDRREQ WSAEDESTADDRREQ
#define EMSGSIZE WSAEMSGSIZE
#define EPROTOTYPE WSAEPROTOTYPE
#define ENOPROTOOPT WSAENOPROTOOPT
#define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#define ESOCKTNOSUPPORT WSAESOCKTNOSUPPORT
#define EOPNOTSUPP WSAEOPNOTSUPP
#define EPFNOSUPPORT WSAEPFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#define EADDRINUSE WSAEADDRINUSE
#define EADDRNOTAVAIL WSAEADDRNOTAVAIL
#define ENETDOWN WSAENETDOWN
#define ENETUNREACH WSAENETUNREACH
#define ENETRESET WSAENETRESET
#define ECONNABORTED WSAECONNABORTED
#define ECONNRESET WSAECONNRESET
#define ENOBUFS WSAENOBUFS
#define EISCONN WSAEISCONN
#define ENOTCONN WSAENOTCONN
#define ESHUTDOWN WSAESHUTDOWN
#define ETOOMANYREFS WSAETOOMANYREFS
#define ETIMEDOUT WSAETIMEDOUT
#define ECONNREFUSED WSAECONNREFUSED
#define ELOOP WSAELOOP
#define ENAMETOOLONG WSAENAMETOOLONG
#define EHOSTDOWN WSAEHOSTDOWN
#define EHOSTUNREACH WSAEHOSTUNREACH
#define ENOTEMPTY WSAENOTEMPTY
#define EPROCLIM WSAEPROCLIM
#define EUSERS WSAEUSERS
#define EDQUOT WSAEDQUOT
#define ESTALE WSAESTALE
#define EREMOTE WSAEREMOTE
#define EBADF WSAEBADF
#define EFAULT WSAEFAULT
#define EAGAIN WSATRY_AGAIN
#endif

#if !defined(MAXNS)
/**
 * @def MAXNS
 * @brief Maximum number of nameservers
 * @details Defines the maximum number of DNS nameservers that can be configured
 * @ingroup IO
 */
#define MAXNS 3
#endif

/**
 * @def IN_MAX_ADDRSTRLEN
 * @brief Maximum length of string representation for an IP address
 * @details Set to INET6_ADDRSTRLEN to accommodate IPv6 addresses, which are longer than IPv4
 * @ingroup IO
 */
#define IN_MAX_ADDRSTRLEN INET6_ADDRSTRLEN

#endif