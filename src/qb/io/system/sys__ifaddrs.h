/**
 * @file qb/io/system/sys__ifaddrs.h
 * @brief Network interface address information utilities.
 *
 * This file exposes the system's `getifaddrs()` and `freeifaddrs()` in the
 * `qb::io` namespace. Every supported POSIX platform ships `ifaddrs.h`; an
 * Android target below API level 24, which lacks it, is refused at compile
 * time (Windows lists its addresses through `getaddrinfo()` on the host
 * name instead and never includes this header).
 * @ingroup Networking
 */

#ifndef QB_IO_IFADDRS_H
#define QB_IO_IFADDRS_H

#include <qb/io/config.h>

#if !(defined(ANDROID) || defined(__ANDROID__)) || __ANDROID_API__ >= 24
#include <ifaddrs.h>
namespace qb::io {
/**
 * @brief Frees the linked list of structures returned by `getifaddrs()`.
 * @ingroup Networking
 * @param ifa A pointer to the head of the list of `ifaddrs` structures.
 * @note This is a wrapper around the system's `freeifaddrs` when available.
 */
using ::freeifaddrs;
/**
 * @brief Creates a linked list of structures describing the network interfaces of the local system.
 * @ingroup Networking
 * @param ifap A pointer to a pointer where the head of the linked list of `ifaddrs` structures will be stored.
 *             The caller is responsible for freeing this list using `freeifaddrs()`.
 * @return 0 on success, or -1 on error (with `errno` set).
 * @note This is a wrapper around the system's `getifaddrs` when available.
 */
using ::getifaddrs;
} // namespace qb::io
#else
// The netlink-based getifaddrs() replacement this header carried for Android API < 24 (derived
// from Xamarin.Android) never compiled -- two commented-out log calls left their argument lines
// live -- and Android is not a qb platform: it was removed rather than repaired (Huly QB-321).
#error "qb requires getifaddrs(): Android API level 24 or later"
#endif

#endif