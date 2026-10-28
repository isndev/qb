/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/session/varint-netstring-roundtrip.cpp
 * @brief `qb::protocol::varint_binary` and `qb::protocol::netstring` over a real loopback TCP session (Huly QB-80).
 *
 * The unit test (unit/protocol/varint-netstring-framing.cpp) proves the parsing arithmetic on a probe; this one
 * proves the two protocols through the whole `use<>::tcp::client/server` stack on loopback sockets driven by the
 * in-process libev loop: a client sends N framed messages, the server echoes each one framed the same way, both
 * sides tally EXACT per-message payloads and the final counts are asserted in the TEST BODY. Modelled on
 * text-protocol-roundtrip.cpp: ephemeral `listen_v4(0)`, the client on its own thread with its own loop, the
 * server pumped through `pump_until` to a completion condition, no sleeps.
 *
 * The varint echo also checks what no text protocol can: an empty payload travels (a one-byte frame) and a
 * payload longer than 127 bytes takes a two-byte prefix on the wire.
 */

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/protocol/netstring.h>
#include <qb/io/protocol/varint.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

constexpr std::size_t ITERATIONS       = 10;
constexpr std::size_t FRAMES_PER_ROUND = 3; // short, long, empty
constexpr std::size_t EXPECTED_FRAMES  = ITERATIONS * FRAMES_PER_ROUND;
const std::string     SHORT_PAYLOAD(40, 's'); // a one-byte varint prefix
const std::string     LONG_PAYLOAD(300, 'L'); // a two-byte varint prefix (AC 02)

class FramedRoundtripTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

} // namespace

// =============================================================================
// varint_binary echo: short, long and empty payloads
// =============================================================================

namespace varint_rt {

std::atomic<std::size_t> g_server_frames{0};
std::atomic<std::size_t> g_client_frames{0};
std::atomic<std::size_t> g_client_bytes{0};
std::atomic<std::size_t> g_client_empty{0};
std::atomic<std::size_t> g_bad{0};

class VarintServer;

class VarintSession : public use<VarintSession>::tcp::client<VarintServer> {
public:
    using Protocol = qb::protocol::varint_binary<VarintSession>;

    explicit VarintSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        // Echo the payload, framed the same way; the header the client sent is what the decoder found.
        if (msg.header_size != qb::protocol::varint::encoded_size(msg.size))
            g_bad.fetch_add(1);
        *this << Protocol::Header(msg.size).view() << std::string_view(msg.data, msg.size);
        g_server_frames.fetch_add(1);
    }
};

class VarintServer : public use<VarintServer>::tcp::server<VarintSession> {
public:
    void
    on(IOSession &) {}
};

class VarintClient : public use<VarintClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::varint_binary<VarintClient>;

    void
    on(Protocol::message &&msg) {
        const std::string_view payload(msg.data, msg.size);
        if (payload.empty())
            g_client_empty.fetch_add(1);
        else if (payload != SHORT_PAYLOAD && payload != LONG_PAYLOAD)
            g_bad.fetch_add(1);
        g_client_bytes.fetch_add(msg.size);
        g_client_frames.fetch_add(1);
    }
};

} // namespace varint_rt

TEST_F(FramedRoundtripTest, VarintBinaryEchoRoundTrip) {
    using namespace varint_rt;
    g_server_frames.store(0);
    g_client_frames.store(0);
    g_client_bytes.store(0);
    g_client_empty.store(0);
    g_bad.store(0);

    VarintServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    const std::size_t expected_bytes = ITERATIONS * (SHORT_PAYLOAD.size() + LONG_PAYLOAD.size());

    std::thread client_thread([port]() {
        async::init();
        VarintClient client;
        ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), SocketStatus::Done);
        client.start();

        using Protocol = VarintClient::Protocol;
        for (std::size_t i = 0; i < ITERATIONS; ++i) {
            client << Protocol::Header(SHORT_PAYLOAD.size()).view() << SHORT_PAYLOAD;
            client << Protocol::Header(LONG_PAYLOAD.size()).view() << LONG_PAYLOAD;
            client << Protocol::Header(0).view(); // the empty payload: a one-byte frame
        }
        (void) qb::io::test::pump_until([] { return g_server_frames.load() >= EXPECTED_FRAMES && g_client_frames.load() >= EXPECTED_FRAMES; },
                                        5s);
    });

    EXPECT_TRUE(pump_until([] { return g_server_frames.load() >= EXPECTED_FRAMES && g_client_frames.load() >= EXPECTED_FRAMES; }, 5s))
        << "varint round-trip did not complete";
    client_thread.join();

    EXPECT_EQ(g_server_frames.load(), EXPECTED_FRAMES);
    EXPECT_EQ(g_client_frames.load(), EXPECTED_FRAMES);
    EXPECT_EQ(g_client_bytes.load(), expected_bytes);
    EXPECT_EQ(g_client_empty.load(), ITERATIONS) << "every empty payload must travel as its own frame";
    EXPECT_EQ(g_bad.load(), 0u);
}

// =============================================================================
// netstring echo
// =============================================================================

namespace netstring_rt {

constexpr const char NETSTRING_MESSAGE[] = "Hello, netstring!";

std::atomic<std::size_t> g_server_frames{0};
std::atomic<std::size_t> g_client_frames{0};
std::atomic<std::size_t> g_bad{0};

class NetstringServer;

class NetstringSession : public use<NetstringSession>::tcp::client<NetstringServer> {
public:
    using Protocol = qb::protocol::netstring_view<NetstringSession>;

    explicit NetstringSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        if (msg.text != NETSTRING_MESSAGE)
            g_bad.fetch_add(1);
        *this << Protocol::Header(msg.size).view() << msg.text << Protocol::end; // echo, framed
        g_server_frames.fetch_add(1);
    }
};

class NetstringServer : public use<NetstringServer>::tcp::server<NetstringSession> {
public:
    void
    on(IOSession &) {}
};

class NetstringClient : public use<NetstringClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::netstring<NetstringClient>;

    void
    on(Protocol::message &&msg) {
        if (msg.text != NETSTRING_MESSAGE)
            g_bad.fetch_add(1);
        g_client_frames.fetch_add(1);
    }
};

} // namespace netstring_rt

TEST_F(FramedRoundtripTest, NetstringEchoRoundTrip) {
    using namespace netstring_rt;
    g_server_frames.store(0);
    g_client_frames.store(0);
    g_bad.store(0);

    NetstringServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread client_thread([port]() {
        async::init();
        NetstringClient client;
        ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), SocketStatus::Done);
        client.start();

        using Protocol = NetstringClient::Protocol;
        const std::string_view message{NETSTRING_MESSAGE};
        for (std::size_t i = 0; i < ITERATIONS; ++i)
            client << Protocol::Header(message.size()).view() << message << Protocol::end;
        (void) qb::io::test::pump_until([] { return g_server_frames.load() >= ITERATIONS && g_client_frames.load() >= ITERATIONS; }, 5s);
    });

    EXPECT_TRUE(pump_until([] { return g_server_frames.load() >= ITERATIONS && g_client_frames.load() >= ITERATIONS; }, 5s))
        << "netstring round-trip did not complete";
    client_thread.join();

    EXPECT_EQ(g_server_frames.load(), ITERATIONS);
    EXPECT_EQ(g_client_frames.load(), ITERATIONS);
    EXPECT_EQ(g_bad.load(), 0u);
}
