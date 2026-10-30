/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/connector-clock.cpp
 * @brief The async connector's deadline is measured on the monotonic clock, not on the time of day (Huly QB-1194).
 *
 * The connector took its deadline as `ev_time() + timeout` when it was built and armed the time left,
 * `deadline - ev_time()`, when an attempt started: two readings of the system's wall clock, so a step of the time of
 * day between them -- during the name resolution `run()` makes first, for one -- moved the deadline by the step. Here
 * the time of day is INJECTED, never changed on the system: every `ev_time()` the connector's header makes, compiled in
 * this file (its template is instantiated here), reads the system's wall clock plus a step the test sets. The TCP
 * connect never completes -- its descriptor, a listening socket, never turns writable -- so a deadline alone
 * ends it, and it must end it `timeout` after the connector was built, on `qb::mono_now()`, whatever the time of day
 * did in between: not early after a step forward, not late after a step back, through each of the four constructors;
 * over two addresses, the first gives way after its share of the deadline on the same clock; and a timeout past the
 * clock's end arms no timer at all.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// qev's header first: it declares `ev_time` (under its link name, `qb_ev_time`).
#include <qb/io/async/listener.h>

namespace connector_clock_test {

/// The step of the time of day a test injects, in seconds; the system's clock is never changed.
inline double time_of_day_step = 0.;

/// When each connect attempt started, on `qb::mono_now()`.
inline std::vector<qb::mono_time> attempt_starts;

/// How often the loop reported a silent descriptor ready: never, or the fake did not hold a connect in progress.
inline int readiness_events = 0;

/// How much less than its span a deadline measured here may read: the conversions to the loop's double seconds,
/// nothing else. A test reads `qb::mono_now()` before the connector does (its deadline: that reading plus the timeout,
/// to the nanosecond); the connector arms each deadline for what is left of it on `qb::mono_now()`, through
/// `async::callback`, whose `event::timer::start` refreshes the loop's clock after that reading; the loop fires it on a
/// fresh reading at least the arm plus the span; the completion reads `qb::mono_now()` after it. One clock
/// (ListenerTimerGate.TheLoopsClockIsTheClockOfMonoTime, to 0.1 us), read in that order: a deadline ends no earlier
/// than its span on any host, a load only delays it. A slack of a sixteenth let an early fire through -- a deadline
/// armed against the loop's cached clock, as stale as the passes the core skipped (Huly QB-190), up to 187.5 ms here.
inline constexpr qb::duration kClockRounding = std::chrono::microseconds{1};

/// The system's wall clock, as the loop library reads it, plus the injected step.
[[nodiscard]] inline double
stepped_time_of_day() noexcept {
    return ev_time() + time_of_day_step;
}

} // namespace connector_clock_test

// The connector's header -- and the headers it is the first to include -- compiled with every `ev_time()` reading the
// stepped time of day; the injection is lifted right after it.
#ifdef QB_IO_ASYNC_TCP_CONNECTOR_H
#error "qb/io/async/tcp/connector.h was included before the time of day was injected: its connector would read the real one"
#endif
#pragma push_macro("ev_time")
#undef ev_time
#define ev_time() connector_clock_test::stepped_time_of_day()
#include <qb/io/async/tcp/connector.h>
#pragma pop_macro("ev_time")

#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace connector_clock_test {

// A TCP connect that never completes, on a real descriptor the loop can watch: `n_connect` says it is in progress, and
// the descriptor is a listening TCP socket, which never turns writable -- a socket that is not connected cannot send
// (XNU's `filt_sowrite`, Linux's `inet_csk_listen_poll`) -- so to the connector the attempt stays in progress until a
// deadline ends it: the whole connect's, or the attempt's share. Nobody connects to it, so it never turns readable
// either. It connects to an endpoint (the address is never used): over a list of them the connector falls back, each
// attempt within its share.
class SilentPeerSocket {
    std::shared_ptr<qb::io::tcp::listener> _listener;

    // A listening loopback socket; null when the system refuses one. Its own function: the listener's construction and
    // teardown set errno, which `n_connect` must leave as it says.
    [[nodiscard]] static std::shared_ptr<qb::io::tcp::listener>
    silent_descriptor() {
        auto made = std::make_shared<qb::io::tcp::listener>();
        return made->listen_v4(0, "127.0.0.1") == qb::io::SocketStatus::Done ? made : nullptr;
    }

public:
    [[nodiscard]] bool
    is_open() const noexcept {
        return _listener && _listener->is_open();
    }

    [[nodiscard]] auto
    native_handle() const noexcept {
        return _listener->native_handle();
    }

    int
    n_connect(qb::io::uri const &) {
        return n_connect(qb::io::endpoint{});
    }

    int
    n_connect(qb::io::endpoint const &) {
        attempt_starts.push_back(qb::mono_now());
        _listener = silent_descriptor();
        if (!_listener)
            ADD_FAILURE() << "the system refused a listening loopback socket: no connect is held in progress";
        qb::io::socket::set_last_errno(_listener ? EINPROGRESS : ECONNREFUSED);
        return -1;
    }

    // What the connector reads once the descriptor is ready, which it never is: counted, so a test sees the plant held.
    [[nodiscard]] int
    handshake_status() const noexcept {
        ++readiness_events;
        return 0;
    }

    template <typename T>
    int
    get_optval(int, int, T &out) const noexcept {
        ++readiness_events;
        out = T{};
        return 0;
    }

    void
    disconnect() noexcept {
        _listener.reset();
    }
};

/// The connector's four constructors: a URI or a list of endpoints, each with or without a socket handed over (one
/// not open yet, so the connector opens -- and over a list replaces -- its own).
enum class form { uri, socket_uri, endpoints, socket_endpoints };

[[nodiscard]] inline char const *
name(form const f) noexcept {
    switch (f) {
        case form::uri:
            return "connector(func, uri, timeout)";
        case form::socket_uri:
            return "connector(func, socket, uri, timeout)";
        case form::endpoints:
            return "connector(func, endpoints, host, timeout)";
        case form::socket_endpoints:
            return "connector(func, socket, endpoints, host, timeout)";
    }
    return "?";
}

class ConnectorClock : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
        time_of_day_step = 0.;
        attempt_starts.clear();
        readiness_events = 0;
    }

    void
    TearDown() override {
        time_of_day_step = 0.;
        qb::io::async::listener::current.clear();
    }

    /// What the completion callback saw: shared with it, so a connect still in flight at the end owns its own copy.
    struct completion {
        bool          ended = false;
        bool          open  = true; ///< what it delivered was an open socket
        qb::mono_time at{};
    };

    struct outcome {
        bool         ended     = false; ///< the connect completed within the pump's budget
        bool         open      = true;  ///< what it delivered was an open socket
        qb::duration after     = {};    ///< from the connector's construction to its completion, on `qb::mono_now()`
        double       landed    = 0.;    ///< how far the injected time of day read from the system's when it was stepped
        int          timers    = 0;     ///< the timers the connector armed when it ran: its deadline's
        std::size_t  attempts  = 0;     ///< the attempts started by the end
        qb::duration second_at = {};    ///< from the construction to the second attempt, when there was one
        int          readiness = 0;     ///< readiness events the silent descriptors reported (the plant held: 0)
    };

    [[nodiscard]] static int
    active_timers() noexcept {
        return *ev_timer_count_addr(static_cast<struct ev_loop *>(qb::io::async::listener::current.loop()));
    }

    // Build the connector in form `f` -- over `addresses` endpoints for a list form --, step the time of day by `step`
    // seconds, run it, and pump until `done_when` holds or `budget` runs out.
    template <typename Until>
    static outcome
    connect_across_a_step(form const f, qb::duration const timeout, double const step, std::chrono::milliseconds const budget,
                          std::size_t const addresses, Until &&done_when) {
        outcome    result;
        auto const done = std::make_shared<completion>();
        auto       on   = [done](SilentPeerSocket &&socket) {
            done->open  = socket.is_open();
            done->at    = qb::mono_now();
            done->ended = true;
        };
        using connector_t = qb::io::async::tcp::connector<SilentPeerSocket, decltype(on)>;
        std::vector<qb::io::endpoint> const endpoints(addresses, qb::io::endpoint().as_in("127.0.0.1", 9));
        qb::io::uri const                   remote{"tcp://127.0.0.1:9"};
        double const                        seconds = qb::detail::to_ev_seconds(timeout);
        auto const                          built   = qb::mono_now();
        std::shared_ptr<connector_t>        op;
        switch (f) {
            case form::uri:
                op = std::make_shared<connector_t>(std::move(on), remote, seconds);
                break;
            case form::socket_uri:
                op = std::make_shared<connector_t>(std::move(on), SilentPeerSocket{}, remote, seconds);
                break;
            case form::endpoints:
                op = std::make_shared<connector_t>(std::move(on), endpoints, "127.0.0.1", seconds);
                break;
            case form::socket_endpoints:
                op = std::make_shared<connector_t>(std::move(on), SilentPeerSocket{}, endpoints, "127.0.0.1", seconds);
                break;
        }
        time_of_day_step  = step;
        result.landed     = stepped_time_of_day() - qb::detail::to_ev_seconds(qb::wall_now().time_since_epoch());
        int const timers0 = active_timers();
        op->run();
        result.timers = active_timers() - timers0;
        op.reset(); // the connector keeps itself alive while its connect is in flight
        (void) pump_until([&] { return done_when(*done); }, budget);
        result.ended     = done->ended;
        result.open      = done->open;
        result.after     = done->ended ? done->at - built : qb::duration::zero();
        result.attempts  = attempt_starts.size();
        result.second_at = result.attempts >= 2 ? attempt_starts[1] - built : qb::duration::zero();
        result.readiness = readiness_events;
        time_of_day_step = 0.;
        attempt_starts.clear();
        readiness_events = 0;
        qb::io::async::listener::current.clear(); // a connect still in flight goes with its loop, not into the next form
        return result;
    }

    static outcome
    until_it_ends(form const f, qb::duration const timeout, double const step, std::chrono::milliseconds const budget = 3s) {
        return connect_across_a_step(f, timeout, step, budget, 1, [](completion const &c) { return c.ended; });
    }
};

constexpr form kEveryForm[] = {form::uri, form::socket_uri, form::endpoints, form::socket_endpoints};

} // namespace connector_clock_test

using connector_clock_test::ConnectorClock;

TEST_F(ConnectorClock, ATimeOfDayStepForwardDoesNotCutTheDeadlineShort) {
    // 0.4 s forward on a 0.6 s deadline: measured on the time of day, 0.2 s would be left.
    using namespace connector_clock_test;
    constexpr auto kTimeout = 600ms;
    for (form const f : kEveryForm) {
        SCOPED_TRACE(name(f));
        auto const r = until_it_ends(f, kTimeout, 0.4);
        ASSERT_GT(r.landed, 0.3) << "the injection did not move the time of day this file reads";
        ASSERT_EQ(r.readiness, 0) << "a silent descriptor turned ready: the connect was not held in progress";
        EXPECT_EQ(r.timers, 1) << "the connector armed no deadline, or more than its one";
        ASSERT_TRUE(r.ended) << "the connect never ended";
        EXPECT_FALSE(r.open);
        EXPECT_GE(r.after, kTimeout - kClockRounding)
            << "the deadline fired early, by " << (kTimeout - r.after).count() << " ns: the step forward was counted as time spent";
    }
}

TEST_F(ConnectorClock, ATimeOfDayStepBackDoesNotExtendTheDeadline) {
    // 30 s back on a 0.6 s deadline: measured on the time of day, the connect would wait 30.6 s.
    using namespace connector_clock_test;
    constexpr auto kTimeout = 600ms;
    for (form const f : kEveryForm) {
        SCOPED_TRACE(name(f));
        auto const r = until_it_ends(f, kTimeout, -30.);
        ASSERT_LT(r.landed, -15.) << "the injection did not move the time of day this file reads";
        ASSERT_EQ(r.readiness, 0) << "a silent descriptor turned ready: the connect was not held in progress";
        EXPECT_EQ(r.timers, 1) << "the connector armed no deadline, or more than its one";
        ASSERT_TRUE(r.ended) << "the deadline waited for the time of day to catch up with the step";
        EXPECT_FALSE(r.open);
        EXPECT_GE(r.after, kTimeout - kClockRounding) << "the deadline fired early, by " << (kTimeout - r.after).count() << " ns";
    }
}

TEST_F(ConnectorClock, ATimeoutPastTheClocksEndArmsNoTimer) {
    // `qb::duration::max()` from now is past the end of the monotonic clock (past `qb::duration`'s own range once the
    // loop's seconds round it): no deadline, so no timer -- the connect waits as long as the system lets it -- and,
    // over two addresses, no share of a deadline either. On the time of day the connector armed a deadline for the 292
    // years left, and over two addresses the first one's share too.
    using namespace connector_clock_test;
    for (form const f : kEveryForm) {
        SCOPED_TRACE(name(f));
        auto const r = connect_across_a_step(f, qb::duration::max(), 0., 300ms, 2, [](completion const &c) { return c.ended; });
        ASSERT_EQ(r.readiness, 0) << "a silent descriptor turned ready: the connect was not held in progress";
        EXPECT_EQ(r.timers, 0) << "a timeout no clock can reach armed a deadline, or an attempt's share of one";
        EXPECT_FALSE(r.ended) << "a timeout no clock can reach ended the connect";
    }
}

TEST_F(ConnectorClock, ATimeOfDayStepForwardDoesNotCutAnAttemptsShareShort) {
    // Two addresses that never answer and a 6 s deadline: the first gives way after its share, max(6 s / 2, 2 s) = 3 s
    // of what is left when it starts (`tcp::connect_attempt_budget`). Measured on the time of day after a 2 s step
    // forward, 4 s would be left and the share 2 s.
    using namespace connector_clock_test;
    constexpr auto kTimeout = 6s;
    for (form const f : {form::endpoints, form::socket_endpoints}) {
        SCOPED_TRACE(name(f));
        auto const r =
            connect_across_a_step(f, kTimeout, 2., 6000ms, 2, [](completion const &c) { return c.ended || attempt_starts.size() >= 2; });
        ASSERT_EQ(r.readiness, 0) << "a silent descriptor turned ready: the connect was not held in progress";
        ASSERT_GE(r.attempts, 2u) << "the first address never gave way";
        EXPECT_GE(r.second_at, 3000ms - kClockRounding) << "the first address gave way early, by " << (3000ms - r.second_at).count()
                                                        << " ns: the step forward was counted against its share";
        EXPECT_FALSE(r.ended) << "the connect ended before its second attempt";
    }
}
