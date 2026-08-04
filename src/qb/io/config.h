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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#if defined(UE_BUILD_DEBUG) || defined(UE_BUILD_DEVELOPMENT) || defined(UE_BUILD_TEST) || defined(UE_BUILD_SHIPPING) || defined(UE_SERVER)
#define QB_INSIDE_UNREAL 1
#endif // Unreal Engine 4 integration detection

/*
**  The qb version macros
*/
/**
 * @def QB_VERSION_NUM
 * @brief Defines the QB library version number in hexadecimal format
 * @details Format is 0xMMNNRR where MM=major, NN=minor, RR=revision.
 * Computed from the QB_VERSION_* macros published by qb's CMake usage requirements
 * (see qb/cmake/qbConfig.cmake, sourced from QB_FRAMEWORK_VERSION) rather than written
 * out by hand -- the hand-written form was still reporting 0x020600 two releases later.
 * @ingroup IO
 */
#define QB_VERSION_NUM ((QB_VERSION_MAJOR << 16) | (QB_VERSION_MINOR << 8) | QB_VERSION_PATCH)

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
 * @def QB_MAX_MESSAGE_SIZE
 * @brief Maximum allowed message size for protocol parsing (DoS protection)
 * @details Limits the maximum size of a single message that can be parsed by protocols.
 * Messages exceeding this size will cause the protocol to be marked as invalid (`not_ok()`)
 * and the connection to be closed. Default is 100MB.
 * @note This is a safety measure against DoS attacks via oversized messages.
 *       Individual protocols can enforce stricter limits if needed.
 * @ingroup IO
 */
#ifndef QB_MAX_MESSAGE_SIZE
#define QB_MAX_MESSAGE_SIZE static_cast<std::size_t>(100 * 1024 * 1024) // 100MB default
#endif

/**
 * @def QB_UNPACK_MAX_STRIP
 * @brief Maximum number of initial bytes that can be stripped during unpacking
 * @details Limits the number of bytes that can be removed from the beginning
 * of a message during protocol unpacking operations
 * @ingroup IO
 */
#define QB_UNPACK_MAX_STRIP 32

/**
 * @def QB_WINDOWS_WOULDBLOCK_ERROR
 * @brief Windows-specific error code for "would block" (WSAEWOULDBLOCK)
 * @details This error code (10035) indicates that a non-blocking socket operation
 *          cannot be completed immediately. In qb-io, this is treated as a non-fatal
 *          condition that should be ignored, as the event loop will retry the operation.
 * @note This is equivalent to EAGAIN/EWOULDBLOCK on Unix systems.
 * @ingroup IO
 */
#ifdef _WIN32
#define QB_WINDOWS_WOULDBLOCK_ERROR 10035
#endif

/**
 * @def QB_DEFAULT_READ_BUFFER_SIZE
 * @brief Default buffer size for stream read operations
 * @details Defines the default chunk size for reading data into stream buffers.
 *          Set to 64KB which provides good performance while remaining well below
 *          32-bit integer limits to prevent truncation issues on 64-bit systems.
 * @note This value is intentionally kept below INT_MAX/2 to ensure safe casts
 *       to platform-specific socket API parameters (e.g., int on POSIX/Windows).
 * @ingroup IO
 */
#define QB_DEFAULT_READ_BUFFER_SIZE static_cast<std::size_t>(65536)

/**
 * @def QB_MAX_IO_SIZE
 * @brief Maximum safe I/O operation size
 * @details Limits the maximum size of a single read/write operation to prevent
 *          integer overflow when casting from size_t (64-bit) to platform APIs
 *          that expect int or unsigned int (32-bit).
 * @note Set to 1GB which is safely below UINT_MAX (4GB) while allowing large transfers.
 * @ingroup IO
 */
#define QB_MAX_IO_SIZE (static_cast<std::size_t>(1) << 30) // 1GB

/**
 * @def QB_DEFAULT_MAX_SESSIONS
 * @brief Default maximum number of sessions per io_handler instance.
 * @details Used by `io_handler` to limit the number of concurrent sessions to prevent
 *          resource exhaustion. Default is 0 (unlimited) to preserve backward compatibility.
 *          Set to a positive value to enable the limit (e.g., 10000 for production servers).
 *          When the limit is reached, `registerSession()` closes the incoming I/O
 *          and returns `nullptr` (it does not throw).
 *          You can also set it at runtime via `set_max_sessions()`.
 * @ingroup IO
 */
#ifndef QB_DEFAULT_MAX_SESSIONS
#define QB_DEFAULT_MAX_SESSIONS 0
#endif

/**
 * @def QB_MAX_READ_BUFFER_SIZE
 * @brief Maximum allowed size for input buffers (DoS protection)
 * @details Limits the maximum size that an input buffer can grow to before reading is rejected.
 * If the buffer size would exceed this limit during a read operation, the read will fail
 * and the connection will be closed. Default is 200MB.
 * @note This is a critical safety measure against DoS attacks where an attacker sends
 *       data that never forms a complete message, causing the buffer to grow indefinitely.
 *       Without this limit, an attacker could exhaust server memory by keeping connections
 *       open and sending incomplete data.
 * @ingroup IO
 */
#ifndef QB_MAX_READ_BUFFER_SIZE
#define QB_MAX_READ_BUFFER_SIZE static_cast<std::size_t>(200 * 1024 * 1024) // 200MB default
#endif

/**
 * @def QB_MAX_WRITE_BUFFER_SIZE
 * @brief Maximum allowed size for output buffers (DoS protection)
 * @details Limits the maximum size that an output buffer can grow to before writing is rejected.
 * If the buffer size would exceed this limit during a publish operation, the publish will fail
 * and the connection will be closed. Default is 200MB.
 * @note This is a critical safety measure against DoS attacks where an attacker could cause
 *       the output buffer to grow indefinitely if data cannot be written fast enough.
 * @ingroup IO
 */
#ifndef QB_MAX_WRITE_BUFFER_SIZE
#define QB_MAX_WRITE_BUFFER_SIZE static_cast<std::size_t>(200 * 1024 * 1024) // 200MB default
#endif

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
using socket_type = SOCKET; // Modern C++: using alias
typedef int socklen_t;      // Note: socklen_t is a POSIX type, kept for compatibility
#define QB_FD_TO_SOCKET(fd) _get_osfhandle(fd)
#define QB_OPEN_FD_FROM_SOCKET(sock) _open_osfhandle(sock, 0)
#define QB_SD_RECEIVE SD_RECEIVE
#define QB_SD_SEND SD_SEND
#define QB_SD_BOTH SD_BOTH
#define QB_CLOSESOCKET closesocket
#define QB_IOCTLSOCKET ioctlsocket
// `poll` -> `WSAPoll` is a *function* name taken from every consumer of this header. It is kept
// unguarded only because it is Windows-only and no Windows toolchain is reachable from this
// project's development hosts to verify a rename (see dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0.md
// section 7); it is recorded as a known leak, together with the ~40 errno macros redefined below.
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
/**
 * @def QB_SD_RECEIVE
 * @brief Socket shutdown flag for disabling receive operations
 * @details Cross-platform macro that maps to platform-specific constants
 * (SHUT_RD on Unix, SD_RECEIVE on Windows). `SD_RECEIVE` remains available as a guarded alias.
 * @ingroup IO
 */
#define QB_SD_RECEIVE SHUT_RD
/**
 * @def QB_SD_SEND
 * @brief Socket shutdown flag for disabling send operations
 * @details Cross-platform macro that maps to platform-specific constants
 * (SHUT_WR on Unix, SD_SEND on Windows). `SD_SEND` remains available as a guarded alias.
 * @ingroup IO
 */
#define QB_SD_SEND SHUT_WR
/**
 * @def QB_SD_BOTH
 * @brief Socket shutdown flag for disabling both send and receive operations
 * @details Cross-platform macro that maps to platform-specific constants
 * (SHUT_RDWR on Unix, SD_BOTH on Windows). `SD_BOTH` remains available as a guarded alias.
 * @ingroup IO
 */
#define QB_SD_BOTH SHUT_RDWR
/**
 * @def QB_CLOSESOCKET
 * @brief Cross-platform macro for closing a socket
 * @details Maps to close() on Unix systems, closesocket() on Windows. `closesocket` remains
 *          available as a guarded alias.
 * @ingroup IO
 */
#define QB_CLOSESOCKET close
/**
 * @def QB_IOCTLSOCKET
 * @brief Cross-platform macro for socket I/O control
 * @details Maps to ioctl() on Unix systems, ioctlsocket() on Windows. `ioctlsocket` remains
 *          available as a guarded alias.
 * @ingroup IO
 */
#define QB_IOCTLSOCKET ioctl
// SO_NOSIGPIPE is deliberately NOT defined on Linux.
//
// This header used to say `#define SO_NOSIGPIPE MSG_NOSIGNAL` there: a socket OPTION
// name bound to a message FLAG value, in a header that ships in the install tree.
// Linux has no such option — measured, `setsockopt(SOL_SOCKET, MSG_NOSIGNAL, …)` is
// `setsockopt(SOL_SOCKET, 0x4000, …)` and returns -1 / ENOPROTOOPT.
//
// The failed call was never the damage. The portable idiom is a fork on the name:
//
//     #ifdef SO_NOSIGPIPE                               // BSD/macOS: per DESCRIPTOR
//         setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, …);
//     #else                                             // Linux: per CALL
//         send(fd, buf, len, MSG_NOSIGNAL);
//     #endif
//
// so defining the name pushed a Linux consumer down the BSD branch and away from the
// one mechanism that works there — reproducing in the consumer exactly the SIGPIPE hole
// qb had just closed in its own send/recv defaults. `SO_*` is reserved to
// <sys/socket.h> besides, so qb has no authority to define it and a future Linux that
// adds the option would collide with this.
//
// Nothing in qb reads it on Linux: both `defined(SO_NOSIGPIPE)` sites in the tree
// already spell `&& !defined(__linux__)`, so its absence here changes no behaviour.
// A consumer that names it on Linux now gets a compile error, which is the truth.
// Use MSG_NOSIGNAL per call — the default of every qb::io::socket send/recv entry
// point — and let qb apply the BSD option at descriptor acquisition on the platforms
// that have it (`suppress_sigpipe()` in src/qb/io/system/sys__socket.cpp).
/**
 * @typedef socket_type
 * @brief Cross-platform socket handle type
 * @details int on Unix systems, SOCKET (unsigned integer) on Windows
 * @ingroup IO
 */
using socket_type = int; // Modern C++: using alias
/**
 * @def QB_FD_TO_SOCKET(fd)
 * @brief Converts a file descriptor to a socket handle
 * @details On Unix, returns fd unchanged. On Windows, converts using _get_osfhandle()
 * @param fd File descriptor to convert
 * @return Equivalent socket handle
 * @ingroup IO
 */
#define QB_FD_TO_SOCKET(fd) fd
/**
 * @def QB_OPEN_FD_FROM_SOCKET(sock)
 * @brief Converts a socket handle to a file descriptor
 * @details On Unix, returns sock unchanged. On Windows, converts using _open_osfhandle()
 * @param sock Socket handle to convert
 * @return Equivalent file descriptor
 * @ingroup IO
 */
#define QB_OPEN_FD_FROM_SOCKET(sock) sock
#undef socket
#endif
/**
 * @def QB_SD_NONE
 * @brief Special value indicating no socket shutdown operation
 * @details Used to indicate that no shutdown operation should be performed
 * @ingroup IO
 */
#define QB_SD_NONE -1

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

/*
 * Legacy unprefixed spellings of qb's socket-portability macros.
 *
 * qb's own 21 call sites use QB_CLOSESOCKET / QB_IOCTLSOCKET / QB_SD_RECEIVE / QB_SD_SEND /
 * QB_SD_BOTH / QB_SD_NONE / QB_FD_TO_SOCKET / QB_OPEN_FD_FROM_SOCKET, and those eight are the
 * supported spellings. The unprefixed ones are **off by default since 3.0.0** -- a documented
 * break -- and come back with `-DQB_LEGACY_SOCKET_MACROS`.
 *
 * `closesocket`, `ioctlsocket`, `SD_BOTH` and friends are Winsock's API names, and this header is
 * reached by every consumer of <qb/io.h>. Taking those names in a public header meant a consumer's
 * own `closesocket` shim -- a common thing to write in portable network code -- was decided by
 * include order, with no diagnostic under the `-isystem` line qb's CMake package exports. Three of
 * the eight were already `#if !defined(...)`-guarded; the other five were not, and none of them
 * had a prefixed spelling to migrate to.
 *
 * An `#ifndef` guard is NOT enough for this set and that is why the default flipped rather than
 * merely gaining a guard. `closesocket` and `ioctlsocket` are *function* names: a consumer who
 * writes `static int closesocket(int)` -- not a macro -- sails through `#ifndef closesocket`, and
 * qb's object-like macro then rewrites every one of their calls to `close`. Measured, with the
 * guard in place: the consumer's function was never entered and `closesocket(3)` returned `close`'s
 * 0. Only not defining the name fixes that. Nothing in qb, qbm or examples used the unprefixed
 * spellings, so this costs the tree nothing.
 *
 * KNOWN, NOT FIXED HERE, both Windows-only and both recorded rather than changed blind (no
 * Windows toolchain is reachable from this project's development hosts -- see
 * dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0.md section 7):
 *   * `#define poll WSAPoll` above takes a POSIX *function* name.
 *   * the `#ifdef _WIN32` block below redefines ~40 unprefixed `E*` errno constants
 *     (EWOULDBLOCK, EINPROGRESS, ENOTSOCK, ...) to their WSA equivalents.
 */
#ifdef QB_LEGACY_SOCKET_MACROS
#ifndef SD_RECEIVE
#define SD_RECEIVE QB_SD_RECEIVE
#endif
#ifndef SD_SEND
#define SD_SEND QB_SD_SEND
#endif
#ifndef SD_BOTH
#define SD_BOTH QB_SD_BOTH
#endif
#ifndef SD_NONE
#define SD_NONE QB_SD_NONE
#endif
#ifndef closesocket
#define closesocket QB_CLOSESOCKET
#endif
#ifndef ioctlsocket
#define ioctlsocket QB_IOCTLSOCKET
#endif
#ifndef FD_TO_SOCKET
#define FD_TO_SOCKET(fd) QB_FD_TO_SOCKET(fd)
#endif
#ifndef OPEN_FD_FROM_SOCKET
#define OPEN_FD_FROM_SOCKET(sock) QB_OPEN_FD_FROM_SOCKET(sock)
#endif
#endif /* QB_LEGACY_SOCKET_MACROS */

#endif