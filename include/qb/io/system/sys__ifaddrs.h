/**
 * @file qb/io/system/sys__ifaddrs.h
 * @brief Network interface address information utilities
 * 
 * This file provides functionality to retrieve network interface addresses
 * through the getifaddrs() and freeifaddrs() functions. It contains 
 * platform-specific implementations for various systems including Android.
 * 
 * On systems with native ifaddrs.h support, this header simply provides
 * namespace wrappers. For Android versions prior to API level 24, it provides
 * a complete implementation.
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

#ifndef QB_IO_IFADDRS_H
#define QB_IO_IFADDRS_H

#include <qb/io/config.h>

#if !(defined(ANDROID) || defined(__ANDROID__)) || __ANDROID_API__ >= 24
#    include <ifaddrs.h>
namespace qb::io {
using ::freeifaddrs;
using ::getifaddrs;
} // namespace qb::io
#else
#    include <assert.h>
#    include <dlfcn.h>
#    include <errno.h>
#    include <limits.h>
#    include <stdio.h>
#    include <stdlib.h>
#    include <string.h>

#    include <sys/socket.h>
#    include <sys/types.h>

#    include <linux/if_arp.h>
#    include <linux/netlink.h>
#    include <linux/rtnetlink.h>
#    include <mutex>
#    include <netinet/in.h>
#    include <unistd.h>

/* Some of these aren't defined in android's rtnetlink.h (as of ndk 16). We define values
 * for all of them if they aren't found so that the debug code works properly. We could
 * skip them but future versions of the NDK might include definitions for them. Values
 * are taken from Linux headers shipped with glibc
 */
#    ifndef IFLA_UNSPEC
#        define IFLA_UNSPEC 0
#    endif

#    ifndef IFLA_ADDRESS
#        define IFLA_ADDRESS 1
#    endif

#    ifndef IFLA_BROADCAST
#        define IFLA_BROADCAST 2
#    endif

#    ifndef IFLA_IFNAME
#        define IFLA_IFNAME 3
#    endif

#    ifndef IFLA_MTU
#        define IFLA_MTU 4
#    endif

#    ifndef IFLA_LINK
#        define IFLA_LINK 5
#    endif

#    ifndef IFLA_QDISC
#        define IFLA_QDISC 6
#    endif

#    ifndef IFLA_STATS
#        define IFLA_STATS 7
#    endif

#    ifndef IFLA_COST
#        define IFLA_COST 8
#    endif

#    ifndef IFLA_PRIORITY
#        define IFLA_PRIORITY 9
#    endif

#    ifndef IFLA_MASTER
#        define IFLA_MASTER 10
#    endif

#    ifndef IFLA_WIRELESS
#        define IFLA_WIRELESS 11
#    endif

#    ifndef IFLA_PROTINFO
#        define IFLA_PROTINFO 12
#    endif

#    ifndef IFLA_TXQLEN
#        define IFLA_TXQLEN 13
#    endif

#    ifndef IFLA_MAP
#        define IFLA_MAP 14
#    endif

#    ifndef IFLA_WEIGHT
#        define IFLA_WEIGHT 15
#    endif

#    ifndef IFLA_OPERSTATE
#        define IFLA_OPERSTATE 16
#    endif

#    ifndef IFLA_LINKMODE
#        define IFLA_LINKMODE 17
#    endif

#    ifndef IFLA_LINKINFO
#        define IFLA_LINKINFO 18
#    endif

#    ifndef IFLA_NET_NS_PID
#        define IFLA_NET_NS_PID 19
#    endif

#    ifndef IFLA_IFALIAS
#        define IFLA_IFALIAS 20
#    endif

#    ifndef IFLA_NUM_VF
#        define IFLA_NUM_VF 21
#    endif

#    ifndef IFLA_VFINFO_LIST
#        define IFLA_VFINFO_LIST 22
#    endif

#    ifndef IFLA_STATS64
#        define IFLA_STATS64 23
#    endif

#    ifndef IFLA_VF_PORTS
#        define IFLA_VF_PORTS 24
#    endif

#    ifndef IFLA_PORT_SELF
#        define IFLA_PORT_SELF 25
#    endif

#    ifndef IFLA_AF_SPEC
#        define IFLA_AF_SPEC 26
#    endif

#    ifndef IFLA_GROUP
#        define IFLA_GROUP 27
#    endif

#    ifndef IFLA_NET_NS_FD
#        define IFLA_NET_NS_FD 28
#    endif

#    ifndef IFLA_EXT_MASK
#        define IFLA_EXT_MASK 29
#    endif

#    ifndef IFLA_PROMISCUITY
#        define IFLA_PROMISCUITY 30
#    endif

#    ifndef IFLA_NUM_TX_QUEUES
#        define IFLA_NUM_TX_QUEUES 31
#    endif

#    ifndef IFLA_NUM_RX_QUEUES
#        define IFLA_NUM_RX_QUEUES 32
#    endif

#    ifndef IFLA_CARRIER
#        define IFLA_CARRIER 33
#    endif

#    ifndef IFLA_PHYS_PORT_ID
#        define IFLA_PHYS_PORT_ID 34
#    endif

#    ifndef IFLA_CARRIER_CHANGES
#        define IFLA_CARRIER_CHANGES 35
#    endif

#    ifndef IFLA_PHYS_SWITCH_ID
#        define IFLA_PHYS_SWITCH_ID 36
#    endif

#    ifndef IFLA_LINK_NETNSID
#        define IFLA_LINK_NETNSID 37
#    endif

#    ifndef IFLA_PHYS_PORT_NAME
#        define IFLA_PHYS_PORT_NAME 38
#    endif

#    ifndef IFLA_PROTO_DOWN
#        define IFLA_PROTO_DOWN 39
#    endif

#    ifndef IFLA_GSO_MAX_SEGS
#        define IFLA_GSO_MAX_SEGS 40
#    endif

#    ifndef IFLA_GSO_MAX_SIZE
#        define IFLA_GSO_MAX_SIZE 41
#    endif

#    ifndef IFLA_PAD
#        define IFLA_PAD 42
#    endif

#    ifndef IFLA_XDP
#        define IFLA_XDP 43
#    endif

/* Maximum interface address label size, should be more than enough */
#    define MAX_IFA_LABEL_SIZE 1024

/**
 * @struct ifaddrs
 * @brief Structure for managing network interface information
 * 
 * This structure provides information about a network interface, including
 * its name, flags, addresses, and related data. It forms a linked list with
 * the ifa_next pointer pointing to the next interface entry.
 */
struct ifaddrs {
    struct ifaddrs *ifa_next; /* Pointer to the next structure.      */

    char *ifa_name;         /* Name of this network interface.     */
    unsigned int ifa_flags; /* Flags as from SIOCGIFFLAGS ioctl.   */

    struct sockaddr *ifa_addr;    /* Network address of this interface.  */
    struct sockaddr *ifa_netmask; /* Netmask of this interface.          */
    union {
        /* At most one of the following two is valid.  If the IFF_BROADCAST
           bit is set in `ifa_flags', then `ifa_broadaddr' is valid.  If the
           IFF_POINTOPOINT bit is set, then `ifa_dstaddr' is valid.
           It is never the case that both these bits are set at once.  */
        struct sockaddr *ifu_broadaddr; /* Broadcast address of this interface. */
        struct sockaddr *ifu_dstaddr;   /* Point-to-point destination address.  */
    } ifa_ifu;
    /* These very same macros are defined by <net/if.h> for `struct ifaddr'.
       So if they are defined already, the existing definitions will be fine.  */
#    ifndef ifa_broadaddr
#        define ifa_broadaddr ifa_ifu.ifu_broadaddr
#    endif
#    ifndef ifa_dstaddr
#        define ifa_dstaddr ifa_ifu.ifu_dstaddr
#    endif
    void *ifa_data; /* Address-specific data (may be unused).  */
};

namespace qb::io {
namespace internal {
/**
 * @brief Netlink request structure for communicating with the kernel
 */
typedef struct {
    struct nlmsghdr header;
    struct rtgenmsg message;
} netlink_request;

/**
 * @brief Session information for netlink communication
 */
typedef struct {
    int sock_fd;
    int seq;
    struct sockaddr_nl them;      /* kernel end */
    struct sockaddr_nl us;        /* our end */
    struct msghdr message_header; /* for use with sendmsg */
    struct iovec payload_vector;  /* Used to send netlink_request */
} netlink_session;

/**
 * @brief Extended sockaddr_ll structure to handle larger hardware addresses
 * 
 * Standard sockaddr_ll has limited space for hardware addresses. This extended 
 * version supports longer addresses for interfaces like Infiniband or IPv6 tunnels.
 */
struct sockaddr_ll_extended {
    unsigned short int sll_family;
    unsigned short int sll_protocol;
    int sll_ifindex;
    unsigned short int sll_hatype;
    unsigned char sll_pkttype;
    unsigned char sll_halen;
    unsigned char sll_addr[24];   /* Extended from standard 8 bytes */
};

/* Function declarations */
static struct ifaddrs *get_link_info(const struct nlmsghdr *message);
static struct ifaddrs *get_link_address(const struct nlmsghdr *message,
                                        struct ifaddrs **ifaddrs_head);

/**
 * @brief Attempts to load getifaddrs and freeifaddrs from the system's libc
 * 
 * This function tries to dynamically load the native getifaddrs and freeifaddrs
 * implementations from libc. If successful, these will be used instead of the
 * custom implementation.
 * 
 * @param getifaddrs_impl Pointer to store the getifaddrs function pointer
 * @param freeifaddrs_impl Pointer to store the freeifaddrs function pointer
 */
static void
get_ifaddrs_impl(int (**getifaddrs_impl)(struct ifaddrs **ifap),
                 void (**freeifaddrs_impl)(struct ifaddrs *ifa)) {
    void *libc = nullptr;

    assert(getifaddrs_impl);
    assert(freeifaddrs_impl);

    libc = dlopen("libc.so", RTLD_NOW);
    if (libc) {
        *getifaddrs_impl =
            reinterpret_cast<int (*)(struct ifaddrs **)>(dlsym(libc, "getifaddrs"));
        if (*getifaddrs_impl)
            *freeifaddrs_impl =
                reinterpret_cast<void (*)(struct ifaddrs *)>(dlsym(libc, "freeifaddrs"));
    }

    if (!*getifaddrs_impl) {
        // QB_LOG("This libc does not have getifaddrs/freeifaddrs, using Xamarin's");
    } else {
        // QB_LOG("This libc has getifaddrs/freeifaddrs");
    }
}

/**
 * @brief Frees a single ifaddrs structure and all its associated resources
 * 
 * @param ifap Pointer to pointer of the ifaddrs structure to free
 */
static void
free_single_ifaddrs(struct ifaddrs **ifap) {
    struct ifaddrs *ifa = ifap ? *ifap : NULL;
    if (!ifa)
        return;

    if (ifa->ifa_name)
        free(ifa->ifa_name);

    if (ifa->ifa_addr)
        free(ifa->ifa_addr);

    if (ifa->ifa_netmask)
        free(ifa->ifa_netmask);

    if (ifa->ifa_broadaddr)
        free(ifa->ifa_broadaddr);

    if (ifa->ifa_data)
        free(ifa->ifa_data);

    free(ifa);
    *ifap = NULL;
}

/**
 * @brief Opens a netlink socket and initializes a session for communication
 * 
 * @param session Pointer to a netlink_session structure to initialize
 * @return 0 on success, -1 on failure
 */
static int
open_netlink_session(netlink_session *session) {
    assert(session != 0);

    memset(session, 0, sizeof(*session));
    session->sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (session->sock_fd == -1) {
        // QB_LOG("Failed to create a netlink socket. %s", strerror(errno));
        return -1;
    }

    /* Fill out addresses */
    session->us.nl_family = AF_NETLINK;

    /* We have previously used `getpid()` here but it turns out that WebView/Chromium
       does the same and there can only be one session with the same PID. Setting it to 0
       will cause the kernel to assign some PID that's unique and valid instead. See:
       https://bugzilla.xamarin.com/show_bug.cgi?id=41860
    */
    session->us.nl_pid = 0;
    session->us.nl_groups = 0;

    session->them.nl_family = AF_NETLINK;

    if (bind(session->sock_fd, (struct sockaddr *)&session->us, sizeof(session->us)) <
        0) {
        // QB_LOG("Failed to bind to the netlink socket. %s", strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * @brief Sends a netlink request to retrieve network information
 * 
 * @param session Pointer to an initialized netlink_session
 * @param type Request type (RTM_GETLINK or RTM_GETADDR)
 * @return 0 on success, -1 on failure
 */
static int
send_netlink_dump_request(netlink_session *session, int type) {
    netlink_request request;

    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
    /* Flags (from netlink.h):
       NLM_F_REQUEST - it's a request message
       NLM_F_DUMP - gives us the root of the link tree and returns all links matching our
       requested AF, which in our case means all of them (AF_PACKET)
    */
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT | NLM_F_MATCH;
    request.header.nlmsg_seq = static_cast<__u32>(++session->seq);
    request.header.nlmsg_pid = session->us.nl_pid;
    request.header.nlmsg_type = static_cast<__u16>(type);

    /* AF_PACKET means we want to see everything */
    request.message.rtgen_family = AF_PACKET;

    memset(&session->payload_vector, 0, sizeof(session->payload_vector));
    session->payload_vector.iov_len = request.header.nlmsg_len;
    session->payload_vector.iov_base = &request;

    memset(&session->message_header, 0, sizeof(session->message_header));
    session->message_header.msg_namelen = sizeof(session->them);
    session->message_header.msg_name = &session->them;
    session->message_header.msg_iovlen = 1;
    session->message_header.msg_iov = &session->payload_vector;

    if (sendmsg(session->sock_fd, (const struct msghdr *)&session->message_header, 0) <
        0) {
        // QB_LOG("Failed to send netlink message. %s", strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * @brief Appends an interface address structure to the linked list
 * 
 * @param addr Address structure to append
 * @param ifaddrs_head Pointer to head of the linked list
 * @param last_ifaddr Pointer to the last item in the linked list
 * @return 0 on success, -1 on failure
 */
static int
append_ifaddr(struct ifaddrs *addr, struct ifaddrs **ifaddrs_head,
              struct ifaddrs **last_ifaddr) {
    assert(addr);
    assert(ifaddrs_head);
    assert(last_ifaddr);

    if (!*ifaddrs_head) {
        *ifaddrs_head = *last_ifaddr = addr;
        if (!*ifaddrs_head)
            return -1;
    } else if (!*last_ifaddr) {
        struct ifaddrs *last = *ifaddrs_head;

        while (last->ifa_next)
            last = last->ifa_next;
        *last_ifaddr = last;
    }

    addr->ifa_next = NULL;
    if (addr == *last_ifaddr)
        return 0;

    assert(addr != *last_ifaddr);
    (*last_ifaddr)->ifa_next = addr;
    *last_ifaddr = addr;
    assert((*last_ifaddr)->ifa_next == NULL);

    return 0;
}

static int
parse_netlink_reply(netlink_session *session, struct ifaddrs **ifaddrs_head,
                    struct ifaddrs **last_ifaddr) {
    struct msghdr netlink_reply;
    struct iovec reply_vector;
    struct nlmsghdr *current_message;
    struct ifaddrs *addr;
    int ret = -1;
    unsigned char *response = NULL;

    assert(session);
    assert(ifaddrs_head);
    assert(last_ifaddr);

    int buf_size = static_cast<int>(getpagesize());
    // QB_LOGV("receive buffer size == %d", buf_size);

    response = (unsigned char *)malloc(sizeof(*response) * buf_size);
    ssize_t length = 0;
    if (!response) {
        goto cleanup;
    }

    while (1) {
        memset(response, 0, buf_size);
        memset(&reply_vector, 0, sizeof(reply_vector));
        reply_vector.iov_len = buf_size;
        reply_vector.iov_base = response;

        memset(&netlink_reply, 0, sizeof(netlink_reply));
        netlink_reply.msg_namelen = sizeof(&session->them);
        netlink_reply.msg_name = &session->them;
        netlink_reply.msg_iovlen = 1;
        netlink_reply.msg_iov = &reply_vector;

        length = recvmsg(session->sock_fd, &netlink_reply, 0);
        // QB_LOGV("  length == %d", static_cast<int>(length));

        if (length < 0) {
            // QB_LOGV("Failed to receive reply from netlink. %s", strerror(errno));
            goto cleanup;
        }

        if (length == 0)
            break;

        for (current_message = (struct nlmsghdr *)response;
             current_message && NLMSG_OK(current_message, static_cast<size_t>(length));
             current_message = NLMSG_NEXT(current_message, length)) {
            // QB_LOGV("next message... (type: %u)", current_message->nlmsg_type);
            switch (current_message->nlmsg_type) {
            /* See rtnetlink.h */
            case RTM_NEWLINK:
                // QB_LOGV("  dumping link...");
                addr = get_link_info(current_message);
                if (!addr || append_ifaddr(addr, ifaddrs_head, last_ifaddr) < 0) {
                    ret = -1;
                    goto cleanup;
                }
                // QB_LOGV("  done");
                break;

            case RTM_NEWADDR:
                // QB_LOGV("  got an address");
                addr = get_link_address(current_message, ifaddrs_head);
                if (!addr || append_ifaddr(addr, ifaddrs_head, last_ifaddr) < 0) {
                    ret = -1;
                    goto cleanup;
                }
                break;

            case NLMSG_DONE:
                // QB_LOGV("  message done");
                ret = 0;
                goto cleanup;
                break;

            default:
                // QB_LOGV("  message type: %u", current_message->nlmsg_type);
                break;
            }
        }
    }

cleanup:
    if (response)
        free(response);
    return ret;
}

static int
fill_sa_address(struct sockaddr **sa, struct ifaddrmsg *net_address, void *rta_data,
                size_t rta_payload_length) {
    assert(sa);
    assert(net_address);
    assert(rta_data);

    switch (net_address->ifa_family) {
    case AF_INET: {
        struct sockaddr_in *sa4;
        assert(rta_payload_length == 4); /* IPv4 address length */
        sa4 = (struct sockaddr_in *)calloc(1, sizeof(*sa4));
        if (!sa4)
            return -1;

        sa4->sin_family = AF_INET;
        memcpy(&sa4->sin_addr, rta_data, rta_payload_length);
        *sa = (struct sockaddr *)sa4;
        break;
    }

    case AF_INET6: {
        struct sockaddr_in6 *sa6;
        assert(rta_payload_length == 16); /* IPv6 address length */
        sa6 = (struct sockaddr_in6 *)calloc(1, sizeof(*sa6));
        if (!sa6)
            return -1;

        sa6->sin6_family = AF_INET6;
        memcpy(&sa6->sin6_addr, rta_data, rta_payload_length);
        if (IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr) ||
            IN6_IS_ADDR_MC_LINKLOCAL(&sa6->sin6_addr))
            sa6->sin6_scope_id = net_address->ifa_index;
        *sa = (struct sockaddr *)sa6;
        break;
    }

    default: {
        struct sockaddr *sagen;
        assert(rta_payload_length <= sizeof(sagen->sa_data));
        *sa = sagen = (struct sockaddr *)calloc(1, sizeof(*sagen));
        if (!sagen)
            return -1;

        sagen->sa_family = net_address->ifa_family;
        memcpy(&sagen->sa_data, rta_data, rta_payload_length);
        break;
    }
    }

    return 0;
}

static int
fill_ll_address(struct sockaddr_ll_extended **sa, struct ifinfomsg *net_interface,
                void *rta_data, size_t rta_payload_length) {
    assert(sa);
    assert(net_interface);

    /* Always allocate, do not free - caller may reuse the same variable */
    *sa = reinterpret_cast<sockaddr_ll_extended *>(calloc(1, sizeof(**sa)));
    if (!*sa)
        return -1;

    (*sa)->sll_family = AF_PACKET; /* Always for physical links */

    /* The assert can only fail for Iniband links, which are quite unlikely to be found
     * in any mobile devices
     */
    // QB_LOGV("rta_payload_length == %d; sizeof sll_addr == %d; hw type == 0x%X",
               static_cast<int>(rta_payload_length), static_cast<int>(sizeof((*sa)->sll_addr)),
               net_interface->ifi_type);
               if (rta_payload_length > sizeof((*sa)->sll_addr)) {
                   // QB_LOG("Address is too long to place in sockaddr_ll (%d > %d)",
                  static_cast<int>(rta_payload_length), static_cast<int>(sizeof((*sa)->sll_addr)));
                  free(*sa);
                  *sa = NULL;
                  return -1;
               }

               if (rta_payload_length > UCHAR_MAX) {
                   // QB_LOG("Payload length too big to fit in the address structure");
                   free(*sa);
                   *sa = NULL;
                   return -1;
               }

               (*sa)->sll_ifindex = net_interface->ifi_index;
               (*sa)->sll_hatype = net_interface->ifi_type;
               (*sa)->sll_halen = static_cast<unsigned char>(rta_payload_length);
               memcpy((*sa)->sll_addr, rta_data, rta_payload_length);

               return 0;
}

static struct ifaddrs *
find_interface_by_index(int index, struct ifaddrs **ifaddrs_head) {
    struct ifaddrs *cur;
    if (!ifaddrs_head || !*ifaddrs_head)
        return NULL;

    /* Normally expensive, but with the small amount of links in the chain we'll deal
     * with it's not worth the extra houskeeping and memory overhead
     */
    cur = *ifaddrs_head;
    while (cur) {
        if (cur->ifa_addr && cur->ifa_addr->sa_family == AF_PACKET &&
            ((struct sockaddr_ll_extended *)cur->ifa_addr)->sll_ifindex == index)
            return cur;
        if (cur == cur->ifa_next)
            break;
        cur = cur->ifa_next;
    }

    return NULL;
}

static char *
get_interface_name_by_index(int index, struct ifaddrs **ifaddrs_head) {
    struct ifaddrs *iface = find_interface_by_index(index, ifaddrs_head);
    if (!iface || !iface->ifa_name)
        return NULL;

    return iface->ifa_name;
}

static int
get_interface_flags_by_index(int index, struct ifaddrs **ifaddrs_head) {
    struct ifaddrs *iface = find_interface_by_index(index, ifaddrs_head);
    if (!iface)
        return 0;

    return static_cast<int>(iface->ifa_flags);
}

static int
calculate_address_netmask(struct ifaddrs *ifa, struct ifaddrmsg *net_address) {
    if (ifa->ifa_addr && ifa->ifa_addr->sa_family != AF_UNSPEC &&
        ifa->ifa_addr->sa_family != AF_PACKET) {
        uint32_t prefix_length = 0;
        uint32_t data_length = 0;
        unsigned char *netmask_data = NULL;

        switch (ifa->ifa_addr->sa_family) {
        case AF_INET: {
            struct sockaddr_in *sa =
                (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
            if (!sa)
                return -1;

            ifa->ifa_netmask = (struct sockaddr *)sa;
            prefix_length = net_address->ifa_prefixlen;
            if (prefix_length > 32)
                prefix_length = 32;
            data_length = sizeof(sa->sin_addr);
            netmask_data = (unsigned char *)&sa->sin_addr;
            break;
        }

        case AF_INET6: {
            struct sockaddr_in6 *sa =
                (struct sockaddr_in6 *)calloc(1, sizeof(struct sockaddr_in6));
            if (!sa)
                return -1;

            ifa->ifa_netmask = (struct sockaddr *)sa;
            prefix_length = net_address->ifa_prefixlen;
            if (prefix_length > 128)
                prefix_length = 128;
            data_length = sizeof(sa->sin6_addr);
            netmask_data = (unsigned char *)&sa->sin6_addr;
            break;
        }
        }

        if (ifa->ifa_netmask && netmask_data) {
            /* Fill the first X bytes with 255 */
            uint32_t prefix_bytes = prefix_length / 8;
            uint32_t postfix_bytes;

            if (prefix_bytes > data_length) {
                errno = EINVAL;
                return -1;
            }
            postfix_bytes = data_length - prefix_bytes;
            memset(netmask_data, 0xFF, prefix_bytes);
            if (postfix_bytes > 0)
                memset(netmask_data + prefix_bytes + 1, 0x00, postfix_bytes);
            // QB_LOGV(
                "   calculating netmask, prefix length is %u bits (%u bytes), data length is %u bytes",
                prefix_length, prefix_bytes, data_length);
                if (prefix_bytes + 2 < data_length)
                    /* Set the rest of the mask bits in the byte following the last 0xFF
                     * value */
                    netmask_data[prefix_bytes + 1] =
                        static_cast<unsigned char>(0xff << (8 - (prefix_length % 8)));
        }
    }

    return 0;
}

static struct ifaddrs *
get_link_address(const struct nlmsghdr *message, struct ifaddrs **ifaddrs_head) {
    ssize_t length = 0;
    struct rtattr *attribute;
    struct ifaddrmsg *net_address;
    struct ifaddrs *ifa = NULL;
    struct sockaddr **sa;
    size_t payload_size;

    assert(message);
    net_address = reinterpret_cast<ifaddrmsg *>(NLMSG_DATA(message));
    length = static_cast<ssize_t>(IFA_PAYLOAD(message));
    // QB_LOGV("   address data length: %u", (unsigned int)length);
    if (length <= 0) {
        goto error;
    }

    ifa = reinterpret_cast<ifaddrs *>(calloc(1, sizeof(*ifa)));
    if (!ifa) {
        goto error;
    }

    // values < 0 are never returned, the cast is safe
    ifa->ifa_flags = static_cast<unsigned int>(get_interface_flags_by_index(
        static_cast<int>(net_address->ifa_index), ifaddrs_head));

    attribute = IFA_RTA(net_address);
    // QB_LOGV("   reading attributes");
    while (RTA_OK(attribute, length)) {
        payload_size = RTA_PAYLOAD(attribute);
        // QB_LOGV("     attribute payload_size == %u", (unsigned int)payload_size);
        sa = NULL;

        switch (attribute->rta_type) {
        case IFA_LABEL: {
            size_t room_for_trailing_null = 0;

            // QB_LOGV("     attribute type: LABEL");
            if (payload_size > MAX_IFA_LABEL_SIZE) {
                payload_size = MAX_IFA_LABEL_SIZE;
                room_for_trailing_null = 1;
            }

            if (payload_size > 0) {
                ifa->ifa_name = (char *)malloc(payload_size + room_for_trailing_null);
                if (!ifa->ifa_name) {
                    goto error;
                }

                memcpy(ifa->ifa_name, RTA_DATA(attribute), payload_size);
                if (room_for_trailing_null)
                    ifa->ifa_name[payload_size] = '\0';
            }
            break;
        }

        case IFA_LOCAL:
            // QB_LOGV("     attribute type: LOCAL");
            if (ifa->ifa_addr) {
                /* P2P protocol, set the dst/broadcast address union from the original
                 * address. Since ifa_addr is set it means IFA_ADDRESS occured earlier
                 * and that address is indeed the P2P destination one.
                 */
                ifa->ifa_dstaddr = ifa->ifa_addr;
                ifa->ifa_addr = 0;
            }
            sa = &ifa->ifa_addr;
            break;

        case IFA_BROADCAST:
            // QB_LOGV("     attribute type: BROADCAST");
            if (ifa->ifa_dstaddr) {
                /* IFA_LOCAL happened earlier, undo its effect here */
                free(ifa->ifa_dstaddr);
                ifa->ifa_dstaddr = NULL;
            }
            sa = &ifa->ifa_broadaddr;
            break;

        case IFA_ADDRESS:
            // QB_LOGV("     attribute type: ADDRESS");
            if (ifa->ifa_addr) {
                /* Apparently IFA_LOCAL occured earlier and we have a P2P connection
                 * here. IFA_LOCAL carries the destination address, move it there
                 */
                ifa->ifa_dstaddr = ifa->ifa_addr;
                ifa->ifa_addr = NULL;
            }
            sa = &ifa->ifa_addr;
            break;

        case IFA_UNSPEC:
            // QB_LOGV("     attribute type: UNSPEC");
            break;

        case IFA_ANYCAST:
            // QB_LOGV("     attribute type: ANYCAST");
            break;

        case IFA_CACHEINFO:
            // QB_LOGV("     attribute type: CACHEINFO");
            break;

        case IFA_MULTICAST:
            // QB_LOGV("     attribute type: MULTICAST");
            break;

        default:
            // QB_LOGV("     attribute type: %u", attribute->rta_type);
            break;
        }

        if (sa) {
            if (fill_sa_address(sa, net_address, RTA_DATA(attribute),
                                RTA_PAYLOAD(attribute)) < 0) {
                goto error;
            }
        }

        attribute = RTA_NEXT(attribute, length);
    }

    /* glibc stores the associated interface name in the address if IFA_LABEL never
     * occured */
    if (!ifa->ifa_name) {
        char *name = get_interface_name_by_index(
            static_cast<int>(net_address->ifa_index), ifaddrs_head);
        // QB_LOGV("   address has no name/label, getting one from interface");
        ifa->ifa_name = name ? strdup(name) : NULL;
    }
    // QB_LOGV("   address label: %s", ifa->ifa_name);

    if (calculate_address_netmask(ifa, net_address) < 0) {
        goto error;
    }

    return ifa;

error : {
    /* errno may be modified by free, or any other call inside the
     * free_single_xamarin_ifaddrs function. We don't care about errors in there since it
     * is more important to know how we failed to obtain the link address and not that we
     * went OOM. Save and restore the value after the resources are freed.
     */
    int errno_save = errno;
    free_single_ifaddrs(&ifa);
    errno = errno_save;
    return NULL;
}
}

static struct ifaddrs *
get_link_info(const struct nlmsghdr *message) {
    ssize_t length;
    struct rtattr *attribute;
    struct ifinfomsg *net_interface;
    struct ifaddrs *ifa = NULL;
    struct sockaddr_ll_extended *sa = NULL;

    assert(message);
    net_interface = reinterpret_cast<ifinfomsg *>(NLMSG_DATA(message));
    length =
        static_cast<ssize_t>(message->nlmsg_len - NLMSG_LENGTH(sizeof(*net_interface)));
    if (length <= 0) {
        goto error;
    }

    ifa = reinterpret_cast<ifaddrs *>(calloc(1, sizeof(*ifa)));
    if (!ifa) {
        goto error;
    }

    ifa->ifa_flags = net_interface->ifi_flags;
    attribute = IFLA_RTA(net_interface);
    while (RTA_OK(attribute, length)) {
        switch (attribute->rta_type) {
        case IFLA_IFNAME:
            ifa->ifa_name = strdup(reinterpret_cast<const char *>(RTA_DATA(attribute)));
            if (!ifa->ifa_name) {
                goto error;
            }
            break;

        case IFLA_BROADCAST:
            // QB_LOGV("   interface broadcast (%u bytes)", (unsigned
            // int)RTA_PAYLOAD(attribute));
            if (fill_ll_address(&sa, net_interface, RTA_DATA(attribute),
                                RTA_PAYLOAD(attribute)) < 0) {
                goto error;
            }
            ifa->ifa_broadaddr = (struct sockaddr *)sa;
            break;

        case IFLA_ADDRESS:
            // QB_LOGV("   interface address (%u bytes)", (unsigned
            // int)RTA_PAYLOAD(attribute));
            if (fill_ll_address(&sa, net_interface, RTA_DATA(attribute),
                                RTA_PAYLOAD(attribute)) < 0) {
                goto error;
            }
            ifa->ifa_addr = (struct sockaddr *)sa;
            break;

        default:;
        }

        attribute = RTA_NEXT(attribute, length);
    }
    // QB_LOGV("link flags: 0x%X", ifa->ifa_flags);
    return ifa;

error:
    if (sa)
        free(sa);
    free_single_ifaddrs(&ifa);

    return NULL;
}
typedef int (*getifaddrs_impl_fptr)(struct ifaddrs **);
typedef void (*freeifaddrs_impl_fptr)(struct ifaddrs *ifa);

static getifaddrs_impl_fptr getifaddrs_impl = NULL;
static freeifaddrs_impl_fptr freeifaddrs_impl = NULL;

static void
getifaddrs_init() {
    get_ifaddrs_impl(&getifaddrs_impl, &freeifaddrs_impl);
}
} // namespace internal

/**
 * @brief Frees memory allocated by getifaddrs()
 * 
 * This function releases all memory allocated for the interface address
 * information returned by getifaddrs(). It traverses the linked list and
 * frees each structure and its associated data.
 * 
 * @param ifa Pointer to the interface address list to free
 */
inline void
freeifaddrs(struct ifaddrs *ifa) {
    struct ifaddrs *cur, *next;

    if (!ifa)
        return;

    if (internal::freeifaddrs_impl) {
        (*internal::freeifaddrs_impl)(ifa);
        return;
    }

    cur = ifa;
    while (cur) {
        next = cur->ifa_next;
        internal::free_single_ifaddrs(&cur);
        cur = next;
    }
}

/**
 * @brief Gets a list of network interfaces and their addresses
 * 
 * This function creates a linked list of structures describing the network
 * interfaces on the local system. For each interface that has addresses
 * associated with it, the structure describes the addresses and flags.
 * 
 * @param ifap Pointer to a pointer where the interface list will be stored
 * @return 0 on success, -1 on failure (with errno set appropriately)
 */
inline int
getifaddrs(struct ifaddrs **ifap) {
    static std::mutex _getifaddrs_init_lock;
    static bool _getifaddrs_initialized;

    if (!_getifaddrs_initialized) {
        std::lock_guard<std::mutex> lock(_getifaddrs_init_lock);
        if (!_getifaddrs_initialized) {
            internal::getifaddrs_init();
            _getifaddrs_initialized = true;
        }
    }

    int ret = -1;

    if (internal::getifaddrs_impl)
        return (*internal::getifaddrs_impl)(ifap);

    if (!ifap)
        return ret;

    *ifap = NULL;
    struct ifaddrs *ifaddrs_head = 0;
    struct ifaddrs *last_ifaddr = 0;
    internal::netlink_session session;

    if (internal::open_netlink_session(&session) < 0) {
        goto cleanup;
    }

    /* Request information about the specified link. In our case it will be all of them
       since we request the root of the link tree below
    */
    if ((internal::send_netlink_dump_request(&session, RTM_GETLINK) < 0) ||
        (internal::parse_netlink_reply(&session, &ifaddrs_head, &last_ifaddr) < 0) ||
        (internal::send_netlink_dump_request(&session, RTM_GETADDR) < 0) ||
        (internal::parse_netlink_reply(&session, &ifaddrs_head, &last_ifaddr) < 0)) {
        freeifaddrs(ifaddrs_head);
        goto cleanup;
    }

    ret = 0;
    *ifap = ifaddrs_head;

cleanup:
    if (session.sock_fd >= 0) {
        close(session.sock_fd);
        session.sock_fd = -1;
    }

    return ret;
}
} // namespace qb::io

#endif

#endif