/**
 * @file qb/io/system/sys__inet_compat.inl
 * @brief IP address conversion compatibility utilities for IPv4 and IPv6.
 *
 * This file provides implementations of `inet_ntop` and `inet_pton` functions
 * for converting IP addresses between their binary (network) representation and
 * their text (presentation) representation. These implementations are
 * platform-independent and primarily serve as fallbacks when native system functions
 * are not available or to ensure consistent behavior (e.g., on older Android versions
 * or specific Windows configurations not providing these via `ws2tcpip.h` directly).
 *
 * The implementations support both IPv4 (AF_INET) and IPv6 (AF_INET6) and
 * aim to comply with RFC specifications for address formatting.
 *
 * @note This file is typically included internally by other qb-io headers (like `sys__socket.h`)
 *       and is not usually intended for direct inclusion by application code.
 *       The functions herein are often exposed through `qb::io::ip::compat::inet_ntop` and `inet_pton`.
 * @ingroup Networking
 */

#ifndef QB_IO_INET_COMPAT_INL
#define QB_IO_INET_COMPAT_INL

// !!!Don't include this file directly, it's used for internal.

// from glibc
#ifdef SPRINTF_CHAR
#define SPRINTF(x) strlen(sprintf /**/ x)
#else
#ifndef SPRINTF
#define SPRINTF(x) (/*(size_t)*/ sprintf x)
#endif
#endif

/**
 * @brief Constants defined based on RFC 883, RFC 1034, RFC 1035
 * These constants are used for domain name processing and DNS messages.
 */
#define NS_PACKETSZ 512   /*%< default UDP packet size */
#define NS_MAXDNAME 1025  /*%< maximum domain name */
#define NS_MAXMSG 65535   /*%< maximum message size */
#define NS_MAXCDNAME 255  /*%< maximum compressed domain name */
#define NS_MAXLABEL 63    /*%< maximum length of domain label */
#define NS_HFIXEDSZ 12    /*%< #/bytes of fixed data in header */
#define NS_QFIXEDSZ 4     /*%< #/bytes of fixed data in query */
#define NS_RRFIXEDSZ 10   /*%< #/bytes of fixed data in r record */
#define NS_INT32SZ 4      /*%< #/bytes of data in a u_int32_t */
#define NS_INT16SZ 2      /*%< #/bytes of data in a u_int16_t */
#define NS_INT8SZ 1       /*%< #/bytes of data in a u_int8_t */
#define NS_INADDRSZ 4     /*%< IPv4 T_A */
#define NS_IN6ADDRSZ 16   /*%< IPv6 T_AAAA */
#define NS_CMPRSFLGS 0xc0 /*%< Flag bits indicating name compression. */
#define NS_DEFAULTPORT 53 /*%< For both TCP and UDP. */

/////////////////// inet_ntop //////////////////
/*
 * WARNING: Don't even consider trying to compile this on a system where
 * sizeof(int) < 4.  sizeof(int) > 4 is fine; all the world's not a VAX.
 */

/**
 * @brief Converts a binary IPv4 address to its presentation (text) format.
 * @param src Pointer to the binary IPv4 address (4 bytes in network byte order).
 * @param dst Destination buffer to store the null-terminated text representation (e.g., "192.168.1.1").
 * @param size Size of the `dst` buffer in bytes. Must be at least `INET_ADDRSTRLEN`.
 * @return Pointer to `dst` on success, or `NULL` on error (e.g., if `size` is too small, `errno` is set to `ENOSPC`).
 * @private
 */
static const char *inet_ntop4(const u_char *src, char *dst, socklen_t size);

/**
 * @brief Converts a binary IPv6 address to its presentation (text) format.
 * @param src Pointer to the binary IPv6 address (16 bytes in network byte order).
 * @param dst Destination buffer to store the null-terminated text representation (e.g., "2001:db8::1").
 * @param size Size of the `dst` buffer in bytes. Must be at least `INET6_ADDRSTRLEN`.
 * @return Pointer to `dst` on success, or `NULL` on error (e.g., if `size` is too small, `errno` is set to `ENOSPC`).
 * @private
 */
static const char *inet_ntop6(const u_char *src, char *dst, socklen_t size);

/**
 * @brief Converts a binary network address (IPv4 or IPv6) to its presentation (text) format.
 * @ingroup Networking
 * @param af The address family of the source address (`AF_INET` for IPv4, `AF_INET6` for IPv6).
 * @param src Pointer to the source binary address (e.g., `struct in_addr*` or `struct in6_addr*`).
 * @param dst Destination buffer to store the null-terminated text representation of the IP address.
 * @param size Size of the `dst` buffer in bytes. Should be `INET_ADDRSTRLEN` for IPv4 or `INET6_ADDRSTRLEN` for IPv6.
 * @return Pointer to `dst` on success. Returns `NULL` on error, and `errno` is set to indicate the error
 *         (e.g., `EAFNOSUPPORT` if `af` is invalid, `ENOSPC` if `size` is too small).
 * @details This function is a portable implementation or wrapper for `inet_ntop`.
 */
const char *
inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    switch (af) {
        case AF_INET:
            return (inet_ntop4((const u_char *) src, dst, size));
        case AF_INET6:
            return (inet_ntop6((const u_char *) src, dst, size));
        default:
            errno = EAFNOSUPPORT;
            return (NULL);
    }
    /* NOTREACHED */
}

/**
 * @brief Implementation of IPv4 binary to text conversion
 *
 * This function formats a binary IPv4 address into the standard dotted-decimal notation
 * (e.g., "192.168.1.1").
 *
 * @param src Pointer to the source IPv4 binary address (4 bytes)
 * @param dst Destination buffer to store the text representation
 * @param size Size of the destination buffer
 * @return Pointer to the string containing the formatted address, or NULL on error
 */
static const char *
inet_ntop4(const u_char *src, char *dst, socklen_t size) {
    char fmt[] = "%u.%u.%u.%u";
    char tmp[sizeof "255.255.255.255"];

    if (SPRINTF((tmp, fmt, src[0], src[1], src[2], src[3])) >= static_cast<int>(size)) {
        errno = (ENOSPC);
        return (NULL);
    }
    return strcpy(dst, tmp);
}

/**
 * @brief Implementation of IPv6 binary to text conversion
 *
 * This function formats a binary IPv6 address into its standard text representation
 * (e.g., "2001:db8::1"). It supports compression of zero sequences.
 *
 * @param src Pointer to the source IPv6 binary address (16 bytes)
 * @param dst Destination buffer to store the text representation
 * @param size Size of the destination buffer
 * @return Pointer to the string containing the formatted address, or NULL on error
 */
static const char *
inet_ntop6(const u_char *src, char *dst, socklen_t size) {
    /*
     * Note that int32_t and int16_t need only be "at least" large enough
     * to contain a value of the specified size.  On some systems, like
     * Crays, there is no such thing as an integer variable with 16 bits.
     * Keep this in mind if you think this function should have been coded
     * to use pointer overlays.  All the world's not a VAX.
     */
    char tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"], *tp;
    struct {
        int base, len;
    } best, cur;
    u_int words[NS_IN6ADDRSZ / NS_INT16SZ];
    int   i;

    /*
     * Preprocessing:
     * Copy the input (bytewise) array into a wordwise array.
     * Find the longest run of 0x00's in src[] for :: shorthand notation.
     */
    memset(words, '\0', sizeof words);
    for (i = 0; i < NS_IN6ADDRSZ; i += 2)
        words[i / 2] = (src[i] << 8) | src[i + 1];
    best.base = -1;
    cur.base  = -1;
    best.len  = 0;
    cur.len   = 0;
    for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
        if (words[i] == 0) {
            if (cur.base == -1) {
                cur.base = i;
                cur.len  = 1;
            } else
                cur.len++;
        } else {
            if (cur.base != -1) {
                if (best.base == -1 || cur.len > best.len)
                    best = cur;
                cur.base = -1;
            }
        }
    }
    if (cur.base != -1) {
        if (best.base == -1 || cur.len > best.len)
            best = cur;
    }
    if (best.base != -1 && best.len < 2)
        best.base = -1;

    /*
     * Format the result.
     */
    tp = tmp;
    for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
        /* Are we inside the best run of 0x00's? */
        if (best.base != -1 && i >= best.base && i < (best.base + best.len)) {
            if (i == best.base)
                *tp++ = ':';
            continue;
        }
        /* Are we following an initial run of 0x00s or any real hex? */
        if (i != 0)
            *tp++ = ':';
        /* Is this address an encapsulated IPv4? */
        if (i == 6 && best.base == 0 &&
            (best.len == 6 || (best.len == 5 && words[5] == 0xffff))) {
            if (!inet_ntop4(src + 12, tp,
                            static_cast<socklen_t>(sizeof tmp - (tp - tmp))))
                return (NULL);
            tp += strlen(tp);
            break;
        }
        tp += SPRINTF((tp, "%x", words[i]));
    }
    /* Was it a trailing run of 0x00's? */
    if (best.base != -1 && (best.base + best.len) == (NS_IN6ADDRSZ / NS_INT16SZ))
        *tp++ = ':';
    *tp++ = '\0';

    /*
     * Check for overflow, copy, and we're done.
     */
    if ((socklen_t) (tp - tmp) > size) {
        errno = (ENOSPC);
        return (NULL);
    }
    return strcpy(dst, tmp);
}

/////////////////// inet_pton ///////////////////

/*
 * WARNING: Don't even consider trying to compile this on a system where
 * sizeof(int) < 4.  sizeof(int) > 4 is fine; all the world's not a VAX.
 */

/**
 * @brief Converts an IPv4 address from presentation (text) format to binary (network) format.
 * @param src Null-terminated string containing the IPv4 address in text form (e.g., "192.168.1.1").
 * @param dst Destination buffer (at least `NS_INADDRSZ` bytes, typically `struct in_addr*`) for the binary address.
 * @return 1 if the conversion succeeded, 0 if the input string `src` is not a valid IPv4 address format.
 * @private
 */
static int inet_pton4(const char *src, u_char *dst);

/**
 * @brief Converts an IPv6 address from presentation (text) format to binary (network) format.
 * @param src Null-terminated string containing the IPv6 address in text form (e.g., "2001:db8::1").
 * @param dst Destination buffer (at least `NS_IN6ADDRSZ` bytes, typically `struct in6_addr*`) for the binary address.
 * @return 1 if the conversion succeeded, 0 if the input string `src` is not a valid IPv6 address format.
 * @private
 */
static int inet_pton6(const char *src, u_char *dst);

/**
 * @brief Converts an IP address (IPv4 or IPv6) from presentation (text) format to binary (network) format.
 * @ingroup Networking
 * @param af The address family of the address being converted (`AF_INET` for IPv4, `AF_INET6` for IPv6).
 * @param src Null-terminated string containing the IP address in its standard text representation.
 * @param dst Destination buffer where the binary form of the address will be stored (e.g., `struct in_addr*` or `struct in6_addr*`).
 *            The buffer must be large enough for the address family (4 bytes for IPv4, 16 for IPv6).
 * @return 1 if the conversion succeeded.
 *         0 if the input string `src` does not contain a character string representing a valid network address in the specified address family.
 *         -1 if the `af` argument is not a valid address family, and `errno` is set to `EAFNOSUPPORT`.
 * @details This function is a portable implementation or wrapper for `inet_pton`.
 */
int
inet_pton(int af, const char *src, void *dst) {
    switch (af) {
        case AF_INET:
            return (inet_pton4(src, (u_char *) dst));
        case AF_INET6:
            return (inet_pton6(src, (u_char *) dst));
        default:
            errno = (EAFNOSUPPORT);
            return (-1);
    }
    /* NOTREACHED */
}

/**
 * @brief Implementation of IPv4 text to binary conversion
 *
 * This function parses an IPv4 address in dotted-decimal notation
 * (e.g., "192.168.1.1") and converts it to its binary equivalent.
 *
 * @param src String containing the IPv4 address in text form
 * @param dst Destination buffer for the binary address (4 bytes)
 * @return 1 if conversion succeeded, 0 if the address is invalid
 */
static int
inet_pton4(const char *src, u_char *dst) {
    int    saw_digit, octets, ch;
    u_char tmp[NS_INADDRSZ], *tp;

    saw_digit   = 0;
    octets      = 0;
    *(tp = tmp) = 0;
    while ((ch = *src++) != '\0') {
        if (ch >= '0' && ch <= '9') {
            u_int newv = *tp * 10 + (ch - '0');

            if (saw_digit && *tp == 0)
                return (0);
            if (newv > 255)
                return (0);
            *tp = static_cast<u_char>(newv);
            if (!saw_digit) {
                if (++octets > 4)
                    return (0);
                saw_digit = 1;
            }
        } else if (ch == '.' && saw_digit) {
            if (octets == 4)
                return (0);
            *++tp     = 0;
            saw_digit = 0;
        } else
            return (0);
    }
    if (octets < 4)
        return (0);
    memcpy(dst, tmp, NS_INADDRSZ);
    return (1);
}

/**
 * @brief Implementation of IPv6 text to binary conversion
 *
 * This function parses an IPv6 address in its standard text form
 * (e.g., "2001:db8::1") and converts it to its binary equivalent.
 * It supports compressed notation with "::".
 *
 * @param src String containing the IPv6 address in text form
 * @param dst Destination buffer for the binary address (16 bytes)
 * @return 1 if conversion succeeded, 0 if the address is invalid
 */
static int
inet_pton6(const char *src, u_char *dst) {
    static const char xdigits[] = "0123456789abcdef";
    u_char            tmp[NS_IN6ADDRSZ], *tp, *endp, *colonp;
    const char       *curtok;
    int               ch, saw_xdigit;
    u_int             val;

    tp     = (u_char *) memset(tmp, '\0', NS_IN6ADDRSZ);
    endp   = tp + NS_IN6ADDRSZ;
    colonp = NULL;
    /* Leading :: requires special handling. */
    if (*src == ':')
        if (*++src != ':')
            return (0);
    curtok     = src;
    saw_xdigit = 0;
    val        = 0;
    while ((ch = tolower(*src++)) != '\0') {
        const char *pch;

        pch = strchr(xdigits, ch);
        if (pch != NULL) {
            val <<= 4;
            val |= (pch - xdigits);
            if (val > 0xffff)
                return (0);
            saw_xdigit = 1;
            continue;
        }
        if (ch == ':') {
            curtok = src;
            if (!saw_xdigit) {
                if (colonp)
                    return (0);
                colonp = tp;
                continue;
            } else if (*src == '\0') {
                return (0);
            }
            if (tp + NS_INT16SZ > endp)
                return (0);
            *tp++      = (u_char) (val >> 8) & 0xff;
            *tp++      = (u_char) val & 0xff;
            saw_xdigit = 0;
            val        = 0;
            continue;
        }
        if (ch == '.' && ((tp + NS_INADDRSZ) <= endp) && inet_pton4(curtok, tp) > 0) {
            tp += NS_INADDRSZ;
            saw_xdigit = 0;
            break; /* '\0' was seen by inet_pton4(). */
        }
        return (0);
    }
    if (saw_xdigit) {
        if (tp + NS_INT16SZ > endp)
            return (0);
        *tp++ = (u_char) (val >> 8) & 0xff;
        *tp++ = (u_char) val & 0xff;
    }
    if (colonp != NULL) {
        /*
         * Since some memmove()'s erroneously fail to handle
         * overlapping regions, we'll do the shift by hand.
         */
        const auto n = tp - colonp;
        int        i;

        if (tp == endp)
            return (0);
        for (i = 1; i <= n; i++) {
            endp[-i]      = colonp[n - i];
            colonp[n - i] = 0;
        }
        tp = endp;
    }
    if (tp != endp)
        return (0);
    memcpy(dst, tmp, NS_IN6ADDRSZ);
    return (1);
}
#endif