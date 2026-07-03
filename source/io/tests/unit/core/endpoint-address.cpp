/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/endpoint-address.cpp
 * @brief `qb::io::endpoint` — IPv4/IPv6 address parse, format, mutate, compare. No live I/O.
 *
 * `endpoint` (qb::io::endpoint == qb::io::inet::ip::endpoint, from qb/io/system/sys__socket.h) is
 * the framework's `sockaddr` value type: it parses dotted-quad / bracketed-IPv6 / numeric host
 * addresses, formats them back (`to_string()` with the IPv6 brackets), exposes family/port/ip
 * setters and getters, and orders/compares by the meaningful sockaddr bytes. This is a pure
 * parse/format value test — NO socket, NO bind, NO event loop — so it is a strict `unit` test.
 *
 * Renamed from system/test-inet-endpoint.cpp (spec §2) and extended with the spec additions:
 *   - invalid-IP / empty-string input (the parse leaves the endpoint invalid, no throw);
 *   - `operator==` / `operator!=` (the std-namespace comparators that key ordered containers);
 *   - the default (AF_UNSPEC) endpoint's `to_string()` is empty;
 *   - an IPv4-mapped IPv6 address (`::ffff:a.b.c.d`) is an AF_INET6 endpoint that formats back;
 *   - move-construction preserves the address.
 * Folds in system/test-io.cpp::SocketUtils.EndpointOnInvalidFd (the two static endpoint factories
 * over an invalid fd return invalid endpoints) — it is a pure-logic endpoint contract with no live
 * connection, so it belongs here rather than in the loopback system suite.
 */

#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include <gtest/gtest.h>

// qb/io/tcp/socket.h pulls in qb/io/config.h (the portable socket vocabulary: AF_UNSPEC, AF_INET,
// AF_INET6, INADDR_LOOPBACK, ...) plus the `endpoint` type and its static fd factories. This is a
// pure parse/format/compare test, so no POSIX-only raw-syscall headers are needed.
#include <qb/io/tcp/socket.h>

using qb::io::endpoint;

// =============================================================================
// CONSTRUCTION / VALIDITY
// =============================================================================

TEST(EndpointAddress, DefaultIsInvalidAndStringifiesEmpty) {
    endpoint e;
    EXPECT_FALSE(static_cast<bool>(e)) << "operator bool() is is_valid() (af != AF_UNSPEC)";
    EXPECT_EQ(e.af(), AF_UNSPEC);
    // Spec addition: an unset endpoint has no address to format.
    EXPECT_EQ(e.to_string(), "") << "a default (AF_UNSPEC) endpoint must stringify to empty";
}

/**
 * @test An empty or malformed IP string leaves the endpoint invalid (no throw, no partial state).
 * @brief Spec addition. `endpoint(addr, port)` parses via inet_pton; on failure the family stays
 *        AF_UNSPEC, so `operator bool()` is false and there is no exception.
 */
TEST(EndpointAddress, InvalidOrEmptyAddressLeavesEndpointInvalid) {
    EXPECT_FALSE(static_cast<bool>(endpoint("", 80))) << "empty address string does not parse";
    EXPECT_FALSE(static_cast<bool>(endpoint("not-an-ip", 80)));
    EXPECT_FALSE(static_cast<bool>(endpoint("999.999.999.999", 80))) << "out-of-range octets must not parse";
    EXPECT_FALSE(static_cast<bool>(endpoint("::ffff::1", 80))) << "malformed IPv6 must not parse";
    // A valid one for contrast.
    EXPECT_TRUE(static_cast<bool>(endpoint("127.0.0.1", 80)));
}

// =============================================================================
// IPv4 / IPv6 PARSE + FORMAT
// =============================================================================

TEST(EndpointAddress, IPv4ParseAndFormat) {
    endpoint e("127.0.0.1", 8080);
    EXPECT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_EQ(e.port(), 8080);
    EXPECT_EQ(e.ip(), "127.0.0.1");
    EXPECT_EQ(e.to_string(), "127.0.0.1:8080");
}

TEST(EndpointAddress, IPv6ParseAndFormat) {
    endpoint e("::1", 443);
    EXPECT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET6);
    EXPECT_EQ(e.port(), 443);
    EXPECT_EQ(e.ip(), "::1");
    EXPECT_EQ(e.to_string(), "[::1]:443") << "IPv6 must be bracketed";
}

/**
 * @test `to_string()` keeps the final IPv6 hextet (the `]`-written-one-byte-early regression).
 */
TEST(EndpointAddress, IPv6ToStringKeepsLastChar) {
    endpoint e("2001:db8::abcd", 8080);
    EXPECT_EQ(e.to_string(), "[2001:db8::abcd]:8080");
}

/**
 * @test An IPv4-mapped IPv6 address (`::ffff:a.b.c.d`) is an AF_INET6 endpoint that formats back.
 * @brief Spec addition. inet_pton recognizes the v4-mapped form as IPv6, so the family is AF_INET6
 *        and `to_string()` round-trips the canonical mapped representation.
 */
TEST(EndpointAddress, IPv4MappedIPv6IsInet6AndFormats) {
    endpoint e("::ffff:192.168.0.1", 8080);
    ASSERT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET6) << "an IPv4-mapped address is parsed as IPv6";
    EXPECT_EQ(e.port(), 8080);
    EXPECT_EQ(e.to_string(), "[" + e.ip() + "]:8080") << "the mapped address must round-trip through to_string()";
}

// =============================================================================
// MUTATORS
// =============================================================================

TEST(EndpointAddress, PortSetterLeavesAddressUntouched) {
    endpoint e("10.0.0.5", 1);
    e.port(static_cast<unsigned short>(9999));
    EXPECT_EQ(e.port(), 9999);
    EXPECT_EQ(e.ip(), "10.0.0.5");
}

TEST(EndpointAddress, IpSetterRoundTripsV4AndV6) {
    endpoint e4;
    e4.ip("192.168.1.42");
    EXPECT_EQ(e4.af(), AF_INET);
    EXPECT_EQ(e4.ip(), "192.168.1.42");

    endpoint e6;
    e6.ip("2001:db8::1");
    EXPECT_EQ(e6.af(), AF_INET6);
    EXPECT_EQ(e6.ip(), "2001:db8::1");
}

TEST(EndpointAddress, AfSetterMarksEndpointValid) {
    endpoint e;
    e.af(AF_INET);
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_TRUE(static_cast<bool>(e)) << "valid once a family is set";
}

#if defined(QB_ENABLE_UDS) && QB__HAS_UDS
// =============================================================================
// @test An unnamed AF_UNIX endpoint (kernel addrlen == 2, i.e. the family field
//       alone) stringifies to empty without reading past the address union.
//       Regression for the size_t underflow in to_string()'s AF_UNIX arm: the old
//       `len() - offsetof(sun_path) - 1` wrapped to ~4 GiB for len()==2 and
//       `assign(sun_path, ~4 GiB)` then read far past the ~110-byte union (SIGSEGV
//       / ASan OOB). Every unbound UDS peer reports addrlen == 2 via
//       getsockname()/getpeername(), so this is ordinary reachable input.
// =============================================================================
TEST(EndpointAddress, UnnamedUnixEndpointToStringDoesNotUnderflow) {
    endpoint e;
    e.af(AF_UNIX); // sets sa_family only; leaves len() at 0
    e.len(2);      // the kernel-reported addrlen of an unbound UDS peer
    const std::string s = e.to_string();
    EXPECT_TRUE(s.empty()) << "an unnamed UDS endpoint must stringify empty, not read past the union";
}
#endif

TEST(EndpointAddress, Uint32CtorIsInetWithPort) {
    // endpoint(uint32_t) takes a HOST-order IPv4 address (addr_v4 applies htonl internally).
    endpoint e(static_cast<uint32_t>(INADDR_LOOPBACK), static_cast<unsigned short>(53));
    EXPECT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_EQ(e.port(), 53);
    EXPECT_EQ(e.ip(), "127.0.0.1");
}

// =============================================================================
// COPY / MOVE
// =============================================================================

TEST(EndpointAddress, CopyConstructPreservesAllFields) {
    endpoint a("172.16.0.1", 22);
    endpoint b(a);
    EXPECT_EQ(b.af(), a.af());
    EXPECT_EQ(b.port(), a.port());
    EXPECT_EQ(b.ip(), a.ip());
    EXPECT_EQ(b.to_string(), a.to_string());
}

/**
 * @test Move-construction preserves the address (the value type copies its trivial storage).
 * @brief Spec addition. `endpoint` is a flat sockaddr union, so move and copy are equivalent; this
 *        pins that the moved-into endpoint is fully usable.
 */
TEST(EndpointAddress, MoveConstructPreservesAddress) {
    endpoint src("203.0.113.7", 4242);
    endpoint moved(std::move(src));
    EXPECT_EQ(moved.af(), AF_INET);
    EXPECT_EQ(moved.port(), 4242);
    EXPECT_EQ(moved.ip(), "203.0.113.7");
    EXPECT_EQ(moved.to_string(), "203.0.113.7:4242");
}

// =============================================================================
// EQUALITY / ORDERING (std-namespace comparators)
// =============================================================================

/**
 * @test Two endpoints are identical iff their family + address + port match; a differing port,
 *       address, or family each breaks identity. (qb::io::endpoint exposes no operator==, so we
 *       compare its observable form — to_string() encodes the address:port, af() the family.)
 */
TEST(EndpointAddress, EqualityComparesAddressPortAndFamily) {
    const endpoint a("10.0.0.1", 8080);
    const endpoint same("10.0.0.1", 8080);
    const endpoint diff_port("10.0.0.1", 8081);
    const endpoint diff_addr("10.0.0.2", 8080);
    const endpoint v6("::1", 8080);

    auto identical = [](const endpoint &x, const endpoint &y) {
        return x.af() == y.af() && x.to_string() == y.to_string();
    };
    EXPECT_TRUE(identical(a, same));
    EXPECT_FALSE(identical(a, diff_port)) << "a differing port must break identity";
    EXPECT_FALSE(identical(a, diff_addr)) << "a differing address must break identity";
    EXPECT_FALSE(identical(a, v6)) << "endpoints of different families are never identical";
}

// =============================================================================
// STATIC fd FACTORIES — invalid descriptor yields invalid endpoints
// =============================================================================

/**
 * @test `local_endpoint`/`peer_endpoint` over an invalid fd return invalid endpoints (no throw).
 * @brief Folded from system/test-io.cpp::SocketUtils.EndpointOnInvalidFd. The live-connection
 *        variant (a real loopback accept) lives in the system tcp suite; this pure invalid-fd
 *        contract is a logic test and belongs here.
 */
TEST(EndpointAddress, StaticFactoriesOnInvalidFdReturnInvalid) {
    const auto local = qb::io::socket::local_endpoint(qb::io::inet::invalid_socket);
    EXPECT_FALSE(static_cast<bool>(local));

    const auto peer = qb::io::socket::peer_endpoint(qb::io::inet::invalid_socket);
    EXPECT_FALSE(static_cast<bool>(peer));
}

// =============================================================================
// CONSTRUCTION FROM RAW sockaddr / family+addr (the non-string ctors)
// =============================================================================

/**
 * @test `endpoint(const sockaddr*)` copies an AF_INET sockaddr into the union and is fully usable.
 * @brief Drives the `endpoint(const sockaddr*)` ctor and the `as_is(const sockaddr*)` AF_INET arm
 *        (memcpy of sockaddr_in + len()). We build the sockaddr_in by hand so there is no live I/O.
 */
TEST(EndpointAddress, ConstructFromSockaddrInet) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(static_cast<unsigned short>(1234));
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET, "8.8.4.4", &sin.sin_addr), 1);

    endpoint e(reinterpret_cast<const sockaddr *>(&sin));
    ASSERT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_EQ(e.port(), 1234);
    EXPECT_EQ(e.ip(), "8.8.4.4");
    EXPECT_EQ(e.to_string(), "8.8.4.4:1234");
}

/**
 * @test `endpoint(const sockaddr*)` copies an AF_INET6 sockaddr into the union (the AF_INET6 arm).
 */
TEST(EndpointAddress, ConstructFromSockaddrInet6) {
    sockaddr_in6 sin6{};
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons(static_cast<unsigned short>(5555));
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET6, "2001:db8::1", &sin6.sin6_addr), 1);

    endpoint e(reinterpret_cast<const sockaddr *>(&sin6));
    ASSERT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET6);
    EXPECT_EQ(e.port(), 5555);
    EXPECT_EQ(e.ip(), "2001:db8::1");
    EXPECT_EQ(e.to_string(), "[2001:db8::1]:5555");
}

/**
 * @test `endpoint(int family, const void* addr, port)` builds an AF_INET endpoint from a raw in_addr.
 * @brief Drives the family+addr ctor and the `as_in(family, addr, port)` AF_INET arm (memcpy of the
 *        raw network-order in_addr). The port is supplied separately and round-trips.
 */
TEST(EndpointAddress, ConstructFromFamilyAndRawInet) {
    in_addr a4{};
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET, "203.0.113.9", &a4), 1);

    endpoint e(AF_INET, &a4, static_cast<unsigned short>(80));
    ASSERT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET);
    EXPECT_EQ(e.port(), 80);
    EXPECT_EQ(e.ip(), "203.0.113.9");
}

/**
 * @test The family+addr ctor's AF_INET6 arm copies a raw in6_addr.
 */
TEST(EndpointAddress, ConstructFromFamilyAndRawInet6) {
    in6_addr a6{};
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET6, "fe80::dead:beef", &a6), 1);

    endpoint e(AF_INET6, &a6, static_cast<unsigned short>(9000));
    ASSERT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.af(), AF_INET6);
    EXPECT_EQ(e.port(), 9000);
    EXPECT_EQ(e.ip(), "fe80::dead:beef");
}

// =============================================================================
// addr_v4() GETTER + format_v4()
// =============================================================================

/**
 * @test `addr_v4()` returns the host-order IPv4 address (the inverse of the uint32_t ctor's htonl).
 * @brief Drives the `addr_v4() const` getter (the network->host ntohl read). We set the address via
 *        the host-order uint32_t ctor and read it straight back.
 */
TEST(EndpointAddress, AddrV4GetterReturnsHostOrder) {
    const uint32_t host_addr = 0x7f000001u; // 127.0.0.1
    endpoint       e(host_addr, static_cast<unsigned short>(0));
    EXPECT_EQ(e.addr_v4(), host_addr) << "addr_v4() must round-trip the host-order address";
    EXPECT_EQ(e.ip(), "127.0.0.1");
}

/**
 * @test `format_v4()` substitutes the address/port byte tokens (%N %H %L %M low/high port byte).
 * @brief Drives the `format_v4()` template-free formatter. Address 1.2.3.4 maps the four octets to
 *        %N..%M, and the port's low/high bytes to %l/%h. 0x0102 (host 258) => high 0x01, low 0x02
 *        in network order, so %h=1, %l=2.
 */
TEST(EndpointAddress, FormatV4SubstitutesAddressAndPortBytes) {
    // Port 0x0102 in network byte order is the wire pair {0x01, 0x02}. format_v4 maps the two
    // port bytes positionally: token "%l" -> first port byte (0x01 = 1), "%h" -> second (0x02 = 2).
    endpoint e("1.2.3.4", static_cast<unsigned short>(0x0102));
    EXPECT_EQ(e.format_v4("%N.%H.%L.%M"), "1.2.3.4") << "the four octets fill %N %H %L %M in order";
    EXPECT_EQ(e.format_v4("a=%l b=%h"), "a=1 b=2") << "%l/%h are the network-order port bytes";
    // A format with no tokens is returned verbatim.
    EXPECT_EQ(e.format_v4("literal"), "literal");
}

// =============================================================================
// ip() FALLTHROUGH + inaddr_to_csv_nl (global vs loopback/linklocal)
// =============================================================================

/**
 * @test `ip()` on an AF_UNSPEC endpoint hits the inaddr_to_string nullptr return and yields "".
 * @brief Drives the default (no-family) arm of `inaddr_to_string` (neither AF_INET nor AF_INET6),
 *        which returns nullptr, so `ip()` resizes the buffer to 0.
 */
TEST(EndpointAddress, IpOnUnspecEndpointIsEmpty) {
    endpoint e; // AF_UNSPEC
    EXPECT_EQ(e.ip(), "") << "no family => inaddr_to_string returns nullptr => empty ip";
}

/**
 * @test `inaddr_to_csv_nl` appends a globally-routable address but skips loopback / link-local.
 * @brief Drives the member `inaddr_to_csv_nl` plus the `is_global_in4_addr` predicate: a global v4
 *        address is appended as "addr,"; loopback (127.x) and link-local (169.254.x) are filtered
 *        out, leaving the csv untouched.
 */
TEST(EndpointAddress, InaddrToCsvNlFiltersNonGlobalV4) {
    {
        endpoint    glob("203.0.113.50", 0);
        std::string csv;
        glob.inaddr_to_csv_nl(csv);
        EXPECT_EQ(csv, "203.0.113.50,") << "a global address is appended with a trailing comma";
    }
    {
        endpoint    loop("127.0.0.1", 0);
        std::string csv = "seed,";
        loop.inaddr_to_csv_nl(csv);
        EXPECT_EQ(csv, "seed,") << "loopback must be filtered out";
    }
    {
        endpoint    link("169.254.1.1", 0);
        std::string csv;
        link.inaddr_to_csv_nl(csv);
        EXPECT_EQ(csv, "") << "link-local must be filtered out";
    }
}

/**
 * @test `inaddr_to_csv_nl` also filters non-global IPv6 (loopback ::1) and keeps a global v6 addr.
 * @brief Drives the AF_INET6 arm of `inaddr_to_string` under the `is_global_in6_addr` predicate.
 */
TEST(EndpointAddress, InaddrToCsvNlFiltersNonGlobalV6) {
    {
        endpoint    glob("2001:db8::99", 0);
        std::string csv;
        glob.inaddr_to_csv_nl(csv);
        EXPECT_EQ(csv, "2001:db8::99,");
    }
    {
        endpoint    loop("::1", 0);
        std::string csv;
        loop.inaddr_to_csv_nl(csv);
        EXPECT_EQ(csv, "") << "IPv6 loopback is not global";
    }
}

/**
 * @test The static `inaddr_to_csv_nl(const sockaddr*, csv)` overload constructs+filters in one shot.
 * @brief Drives the static sockaddr overload (line 714) which builds a temporary endpoint.
 */
TEST(EndpointAddress, StaticInaddrToCsvNlFromSockaddr) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET, "198.51.100.7", &sin.sin_addr), 1);

    std::string csv;
    endpoint::inaddr_to_csv_nl(reinterpret_cast<const sockaddr *>(&sin), csv);
    EXPECT_EQ(csv, "198.51.100.7,");

    // loopback sockaddr is filtered.
    sockaddr_in lo{};
    lo.sin_family = AF_INET;
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET, "127.0.0.5", &lo.sin_addr), 1);
    std::string csv2 = "x,";
    endpoint::inaddr_to_csv_nl(reinterpret_cast<const sockaddr *>(&lo), csv2);
    EXPECT_EQ(csv2, "x,");
}

/**
 * @test The static `inaddr_to_csv_nl(int family, const void*, csv)` overload (raw in_addr).
 * @brief Drives the static family+rawaddr overload (line 722).
 */
TEST(EndpointAddress, StaticInaddrToCsvNlFromFamilyAndRaw) {
    in_addr a4{};
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET, "192.0.2.123", &a4), 1);

    std::string csv;
    endpoint::inaddr_to_csv_nl(AF_INET, &a4, csv);
    EXPECT_EQ(csv, "192.0.2.123,");
}

/**
 * @test The free `is_global_in4_addr` / `is_global_in6_addr` predicates classify the address.
 * @brief Drives the standalone predicate functions directly (they back inaddr_to_csv_nl).
 */
TEST(EndpointAddress, GlobalAddressPredicates) {
    in_addr glob4{}, loop4{};
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET, "203.0.113.1", &glob4), 1);
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET, "127.0.0.1", &loop4), 1);
    EXPECT_TRUE(qb::io::inet::ip::is_global_in4_addr(&glob4));
    EXPECT_FALSE(qb::io::inet::ip::is_global_in4_addr(&loop4)) << "loopback is not global";

    in6_addr glob6{}, loop6{};
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET6, "2001:db8::1", &glob6), 1);
    ASSERT_EQ(qb::io::inet::compat::inet_pton(AF_INET6, "::1", &loop6), 1);
    EXPECT_TRUE(qb::io::inet::ip::is_global_in6_addr(&glob6));
    EXPECT_FALSE(qb::io::inet::ip::is_global_in6_addr(&loop6)) << "IPv6 loopback is not global";
}

// =============================================================================
// UNIX DOMAIN SOCKET path (as_un + to_string) — pure value logic, no bind
// =============================================================================

#if defined(QB_ENABLE_UDS) && QB__HAS_UDS
/**
 * @test `as_un(path)` stores a filesystem path as an AF_UNIX endpoint that stringifies to the path.
 * @brief Drives the UDS `as_un` success arm (sun_path fill + len) and the AF_UNIX `to_string` arm.
 *        No bind / connect — this is purely the value-type packing of the path.
 */
TEST(EndpointAddress, UnixDomainPathRoundTrips) {
    endpoint e;
    e.as_un("/tmp/qb-endpoint-address.sock");
    EXPECT_EQ(e.af(), AF_UNIX);
    EXPECT_TRUE(static_cast<bool>(e)) << "an AF_UNIX endpoint is valid";
    EXPECT_EQ(e.to_string(), "/tmp/qb-endpoint-address.sock") << "the AF_UNIX to_string() arm returns the raw sun_path";
}
#endif

// =============================================================================
// std-NAMESPACE ORDERING / EQUALITY OPERATORS (key ordered containers)
// =============================================================================

/**
 * @test `std::operator<` orders by family first, then by IPv4 address, then by IPv4 port.
 * @brief Drives every arm of the IPv4 strict-weak-ordering path: the family-differs branch
 *        (AF_INET < AF_INET6), the address-differs branch, and the same-address/port-differs
 *        branch. The header doc warns the old `addr+port` sum collided — these assertions pin the
 *        fix (distinct endpoints are strictly ordered).
 */
// The endpoint comparators are defined in `namespace std` (a portability workaround so ordered
// containers can key on endpoint). They are NOT reachable by ADL from here, and `std::less<endpoint>`
// cannot find them either (two-phase lookup can't see operators added to std after <functional>).
// A `using std::operator<;` / `using std::operator==;` declaration pulls them into scope so the
// natural `a < b` / `a == b` resolves and exercises the operator bodies in sys__socket.h.
TEST(EndpointAddress, StdLessOrdersInetByFamilyAddrPort) {
    using std::operator<;
    const endpoint v4("10.0.0.1", 8080);
    const endpoint v6("::1", 8080);
    EXPECT_NE(v4.af(), v6.af());
    EXPECT_EQ(v4 < v6, v4.af() < v6.af()) << "different families order by af()";

    // same family, address differs => the address decides, regardless of port (strict antisymmetry).
    const endpoint a1("10.0.0.1", 9999);
    const endpoint a2("10.0.0.2", 1);
    EXPECT_NE(a1 < a2, a2 < a1) << "two distinct addresses are strictly ordered exactly one way";

    // same address, port differs (the addr+port-sum collision the SWO fix addresses).
    const endpoint p_lo("10.0.0.1", 1000);
    const endpoint p_hi("10.0.0.1", 2000);
    EXPECT_NE(p_lo < p_hi, p_hi < p_lo) << "same address, different port => strictly ordered, never equal";
}

/**
 * @test `std::operator<` falls back to a length-bounded memcmp for the non-INET (IPv6) path.
 */
TEST(EndpointAddress, StdLessOrdersInet6ByBytes) {
    using std::operator<;
    const endpoint a("2001:db8::1", 443);
    const endpoint b("2001:db8::2", 443);
    EXPECT_NE(a < b, b < a) << "distinct IPv6 addresses are strictly ordered";

    const endpoint a2("2001:db8::1", 443);
    EXPECT_FALSE(a < a2);
    EXPECT_FALSE(a2 < a) << "identical IPv6 endpoints are unordered";
}

/**
 * @test `std::operator==` is `!(a<b) && !(b<a)` — equal iff family+address+port all match.
 */
TEST(EndpointAddress, StdEqualityComparesFamilyAddrPort) {
    using std::operator==;
    const endpoint a("10.0.0.1", 8080);
    const endpoint same("10.0.0.1", 8080);
    const endpoint diff_port("10.0.0.1", 8081);
    const endpoint diff_addr("10.0.0.2", 8080);
    const endpoint v6("::1", 8080);
    const endpoint v6_same("::1", 8080);

    EXPECT_TRUE(a == same);
    EXPECT_FALSE(a == diff_port) << "the addr+port-sum collision is fixed: these are not equal";
    EXPECT_FALSE(a == diff_addr);
    EXPECT_FALSE(a == v6) << "different families are never equal";
    EXPECT_TRUE(v6 == v6_same);
}

// =============================================================================
// tcp::socket VALUE TRAITS (no fd, no connect)
// =============================================================================

/**
 * @test `tcp::socket::is_secure()` is a constexpr false (the plaintext TCP transport trait).
 * @brief Drives the trivial constexpr accessor on qb::io::tcp::socket without opening a socket.
 */
TEST(EndpointAddress, TcpSocketIsNotSecure) {
    static_assert(!qb::io::tcp::socket::is_secure(), "plaintext tcp::socket must report is_secure() == false");
    EXPECT_FALSE(qb::io::tcp::socket::is_secure());
}
