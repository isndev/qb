/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/async/async-transport-traits.cpp
 * @brief Compile-time transport/event contracts + the handshake protocol's no-side-effect probe.
 *
 * These are the QB_IO_PLAN findings that can be proven WITHOUT a live socket or the event loop:
 *   - the four shipped transports expose `is_secure()` as a `static constexpr` (so generic code can
 *     branch on security at compile time without instantiating a transport);
 *   - `async::event::disconnect_reason` stays an `int`-backed enum with its ABI-frozen reason codes;
 *   - `IProtocol::kNoMessage` is the named `0` sentinel; and `event::eof` is a backward-compatible
 *     alias of `event::input_drained`;
 *   - `protocol::handshake::onMessage()` consumes the size cached by the prior `getMessageSize()`
 *     WITHOUT re-driving the SSL state machine (proven against a mock transport whose
 *     `do_handshake()` counts its own invocations).
 *
 * Everything here is a pure compile-time assertion or a mock-driven call sequence — NO `async::init`,
 * NO socket, NO bind — so this is a strict `unit` test. Extracted from system/test-io-plan.cpp (spec
 * §2): the live-network contracts (UDP write, DoS-reject, input_drained dispatch, scoped_callback
 * timer) stay in the system async suite; only the static contracts + the handshake mock move here.
 * The `event::eof == event::input_drained` static_assert that test-io-plan.cpp stated twice (file
 * scope + inside the dispatch test) is asserted once here.
 */

#include <cstddef>
#include <type_traits>

#include <gtest/gtest.h>

#include <qb/io/async/event/disconnected.h>
#include <qb/io/async/event/eof.h>
#include <qb/io/async/protocol.h>
#include <qb/io/transport/accept.h>
#include <qb/io/transport/tcp.h>
#include <qb/io/transport/udp.h>

#ifdef QB_HAS_SSL
#include <qb/io/protocol/handshake.h>
#include <qb/io/transport/saccept.h>
#endif

using namespace qb::io;

// =============================================================================
// COMPILE-TIME CONTRACTS (the 11 static_asserts, de-duplicated)
// =============================================================================

// Finding 2.2 — every shipped transport's `is_secure()` is a static constexpr so security can be
// probed at compile time without instantiating a transport object.
static_assert(transport::tcp::is_secure() == false, "transport::tcp::is_secure() must be static constexpr false");
static_assert(transport::udp::is_secure() == false, "transport::udp::is_secure() must be static constexpr false");
static_assert(transport::accept::is_secure() == false, "transport::accept::is_secure() must be static constexpr false");
#ifdef QB_HAS_SSL
static_assert(transport::saccept::is_secure() == true, "transport::saccept::is_secure() must be static constexpr true");
#endif

// Finding 2.16/2.17 — `disconnect_reason` is int-backed with ABI-frozen reason codes so callers
// storing reason codes as plain `int` keep working across versions.
static_assert(std::is_same_v<std::underlying_type_t<async::event::disconnect_reason>, int>,
              "disconnect_reason must remain ABI-compatible with int");
static_assert(static_cast<int>(async::event::disconnect_reason::peer_closed) == 0,
              "disconnect_reason::peer_closed must keep value 0");
static_assert(static_cast<int>(async::event::disconnect_reason::user_initiated) == 1,
              "disconnect_reason::user_initiated must keep value 1");
static_assert(static_cast<int>(async::event::disconnect_reason::protocol_error) == -1,
              "disconnect_reason::protocol_error must keep value -1");
static_assert(static_cast<int>(async::event::disconnect_reason::message_too_large) == -2,
              "disconnect_reason::message_too_large must keep value -2");

// Finding 2.21 — `IProtocol::kNoMessage` must be the named 0 sentinel.
static_assert(async::IProtocol::kNoMessage == 0, "IProtocol::kNoMessage must equal 0");

// Finding 2.22 — `event::eof` is a backward-compatible alias of `event::input_drained`
// (asserted ONCE here; test-io-plan.cpp restated it twice).
static_assert(std::is_same_v<async::event::eof, async::event::input_drained>,
              "event::eof must be an alias for event::input_drained");

/**
 * @test A runtime no-op whose existence makes the file a non-empty translation unit and surfaces
 *       the compile-time contracts to the test runner as a passing case.
 * @brief The real proof is the `static_assert`s above (they fail the build, not a test). This case
 *        lets the suite report the contract group as exercised.
 */
TEST(AsyncTransportTraits, CompileTimeContractsHold) {
    SUCCEED() << "all transport / event compile-time contracts are enforced by static_assert above";
}

// =============================================================================
// HANDSHAKE PROTOCOL — onMessage consumes the cached size, no SSL re-drive
// =============================================================================

#ifdef QB_HAS_SSL

namespace {

// Mock IO for the handshake protocol: its transport().do_handshake() counts calls and returns a
// caller-controlled result (0 → not done; >0 → done with that many bytes), so the test can assert
// exactly how many times the SSL state machine is driven.
struct fake_handshake_io {
    struct fake_transport {
        int do_handshake_calls = 0;
        int next_result        = 0;
        int
        do_handshake() noexcept {
            ++do_handshake_calls;
            return next_result;
        }
    };

    using base_io_t = fake_handshake_io; // satisfies AProtocol's friend requirement
    fake_transport _t;
    int            handshake_events = 0;

    fake_transport &
    transport() noexcept {
        return _t;
    }
    void
    on(qb::io::async::event::handshake) noexcept {
        ++handshake_events;
    }
};

} // namespace

/**
 * @test `getMessageSize()` drives `do_handshake()` once per probe; `onMessage()` consumes the cached
 *       size WITHOUT re-driving it; after completion `getMessageSize()` short-circuits; `reset()`
 *       re-arms.
 * @brief Finding 2.19. This is the contract that keeps `getMessageSize()` a pure-query in the
 *        protocol dispatch loop — re-driving the SSL machine from `onMessage()` would double-step
 *        the handshake. Mock-driven, so no SSL socket and no event loop are needed.
 */
TEST(AsyncTransportTraits, HandshakeOnMessageConsumesCachedSizeWithoutRedrive) {
    fake_handshake_io                              io;
    qb::io::protocol::handshake<fake_handshake_io> proto(io);

    // First probe: handshake not done → 0, no event.
    io._t.next_result = 0;
    EXPECT_EQ(proto.getMessageSize(), 0u);
    EXPECT_EQ(io._t.do_handshake_calls, 1);
    EXPECT_EQ(io.handshake_events, 0);

    // Second probe: handshake "done" → returns the size; onMessage() must consume the cached value
    // without invoking do_handshake() again.
    io._t.next_result          = 7;
    const auto reported        = proto.getMessageSize();
    const int  calls_after_probe = io._t.do_handshake_calls;
    EXPECT_EQ(reported, 7u);
    EXPECT_EQ(calls_after_probe, 2);

    proto.onMessage(reported);
    EXPECT_EQ(io._t.do_handshake_calls, calls_after_probe) << "onMessage() must not re-drive the SSL state machine";
    EXPECT_EQ(io.handshake_events, 1) << "onMessage() dispatches exactly one handshake event";

    // Third probe: handshake is done → short-circuits to 0 without poking the transport.
    const int locked_calls = io._t.do_handshake_calls;
    EXPECT_EQ(proto.getMessageSize(), 0u);
    EXPECT_EQ(io._t.do_handshake_calls, locked_calls) << "a completed handshake must not poll do_handshake() again";

    // reset() returns the protocol to its freshly-constructed state.
    proto.reset();
    io._t.next_result = 0;
    EXPECT_EQ(proto.getMessageSize(), 0u);
    EXPECT_EQ(io._t.do_handshake_calls, locked_calls + 1) << "after reset() the protocol probes again";
}

#endif // QB_HAS_SSL
