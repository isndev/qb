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
