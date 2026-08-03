/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/udp-identity.cpp
 * @brief `qb::io::transport::udp::identity` — value equality + hashing of a UDP peer address.
 *
 * `identity` (qb/io/transport/udp.h) extends `qb::io::endpoint` with an `operator==`/`operator!=`
 * over the meaningful `sockaddr` bytes and a `hasher` over the same span, so the UDP transport can
 * key a per-peer session map. This is a pure value type — NO socket bind, NO datagram, NO event
 * loop (the datagram round-trip lives in system/udp/) — so it is a strict `unit` test.
 *
 * Migrated from system/test-io.cpp::UDPTransport.{IdentityEquality,IdentityHashConsistency,
 * IdentityInUnorderedSet} (spec D4). The equality and hash contracts are asserted together so the
 * type's container-key invariant (equal ⇒ equal-hash, distinct address/port ⇒ distinct identity)
 * is pinned in one place.
 */

#include <unordered_set>

#include <gtest/gtest.h>

#include <qb/io/system/sys__socket.h>
#include <qb/io/transport/udp.h>

namespace {
using identity = qb::io::transport::udp::identity;

identity
make(const char *ip, unsigned short port) {
    return identity{qb::io::endpoint(ip, port)};
}
} // namespace

// =============================================================================
// EQUALITY
// =============================================================================

/**
 * @test Two identities compare equal iff their address AND port match.
 * @brief A differing port or a differing address each break equality.
 */
TEST(UdpIdentity, EqualityComparesAddressAndPort) {
    const identity a         = make("127.0.0.1", 5000);
    const identity b         = make("127.0.0.1", 5000);
    const identity diff_port = make("127.0.0.1", 5001);
    const identity diff_addr = make("192.168.1.1", 5000);

    EXPECT_EQ(a, b);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);

    EXPECT_NE(a, diff_port);
    EXPECT_TRUE(a != diff_port);
    EXPECT_NE(a, diff_addr);
}

// =============================================================================
// HASHING
// =============================================================================

/**
 * @test The hasher is consistent with equality: equal identities hash equal; a distinct address
 *       hashes distinct.
 * @brief A distinct-hash on distinct input is not *required* by the hash contract in general, but
 *        for these well-separated addresses it holds and was asserted in the original suite.
 */
TEST(UdpIdentity, HasherIsConsistentWithEquality) {
    const identity a    = make("127.0.0.1", 5000);
    const identity b    = make("127.0.0.1", 5000);
    const identity diff = make("10.0.0.1", 5000);

    identity::hasher h;
    EXPECT_EQ(h(a), h(b)) << "equal identities must hash equal";
    EXPECT_NE(h(a), h(diff));
}

// =============================================================================
// CONTAINER KEY
// =============================================================================

/**
 * @test `identity` works as an `unordered_set` key: duplicates collapse, distinct peers persist.
 */
TEST(UdpIdentity, DeduplicatesInUnorderedSet) {
    std::unordered_set<identity, identity::hasher> id_set;

    id_set.insert(make("127.0.0.1", 5000));
    id_set.insert(make("127.0.0.1", 5001));
    id_set.insert(make("127.0.0.1", 5000)); // duplicate of the first

    EXPECT_EQ(id_set.size(), 2u);
    EXPECT_EQ(id_set.count(make("127.0.0.1", 5000)), 1u);
    EXPECT_EQ(id_set.count(make("127.0.0.1", 5001)), 1u);
    EXPECT_EQ(id_set.count(make("127.0.0.1", 9999)), 0u);
}
