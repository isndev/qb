/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/close_after_peer_support.h
 * @brief What the tests of a connection THIS side ends share (Huly QB-1270): a standalone client that records what
 *        its two teardown hooks saw, a loopback pair, and the observations -- is a descriptor still open, how many
 *        sockets the loop is closing after their peer, what a peer reads and how its stream ends.
 *
 * A qb-io object that ends its own connection gives its descriptor to its thread's loop, which closes it after the
 * peer (`qb::io::async::detail::close_after_peer`). What a test can observe of that is on the wire -- what the peer
 * reads, and whether its stream ends with EOF or a reset -- and in the process: `closing_sockets()`, and whether a
 * descriptor number still names a socket. Everything here runs on the test's own thread, which is also the loop's.
 */

#ifndef QB_IO_TEST_CLOSE_AFTER_PEER_SUPPORT_H
#define QB_IO_TEST_CLOSE_AFTER_PEER_SUPPORT_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#if !defined(_WIN32)
#include <dirent.h>
#endif

namespace qb::io::test::close_after_peer {

/// What an object says last: 17 octets, the size of an HTTP/2 GOAWAY without debug data.
inline constexpr std::string_view LAST = "LAST-BYTES-GOAWAY";

using steady = std::chrono::steady_clock;

inline double
ms_since(steady::time_point since) {
    return std::chrono::duration<double, std::milli>(steady::now() - since).count();
}

/// Whether `s` still names an open socket of this process.
inline bool
descriptor_open(::socket_type s) {
    int       type = 0;
    socklen_t len  = sizeof(type);
    return ::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &len) == 0;
}

/// The sockets this thread's loop is closing after their peer.
inline std::size_t
closing() {
    return qb::io::async::detail::closing_sockets();
}

/// How many descriptors this process has open (-1 where it cannot tell: Windows).
inline int
open_descriptors() {
#if defined(_WIN32)
    return -1;
#else
    DIR *dir = ::opendir("/dev/fd");
    if (!dir)
        return -1;
    int entries = 0;
    while (::readdir(dir))
        ++entries;
    ::closedir(dir);
    return entries - 3; // ".", ".." and the directory's own descriptor
#endif
}

/// Runs the loop without blocking for `how_long`, a millisecond between passes.
inline void
pump(std::chrono::milliseconds how_long) {
    const auto until = steady::now() + how_long;
    while (steady::now() < until) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// How many events the loop dispatches in `passes` passes that do not block.
inline std::size_t
events_in(int passes) {
    std::size_t events = 0;
    for (int i = 0; i < passes; ++i) {
        qb::io::async::run(EVRUN_NOWAIT);
        events += qb::io::async::listener::current.nb_invoked_event();
    }
    return events;
}

/// What a peer read: the octets, then how the stream ended within the budget.
struct Read {
    std::string received;
    std::string end   = "open"; ///< "EOF", "reset", or "open" (nothing more within the budget)
    int         error = 0;
};

/**
 * @brief What `peer` reads within `budget`: up to a clean EOF, a reset or the budget.
 * @param pump_loop Whether the loop runs between the reads. False for a case whose point is that NO loop pass runs.
 */
inline Read
read_all(qb::io::tcp::socket &peer, std::chrono::milliseconds budget, bool pump_loop = true) {
    peer.set_nonblocking(true);
    Read                   out;
    const auto             deadline = steady::now() + budget;
    std::array<char, 4096> buffer{};
    do {
        if (pump_loop)
            qb::io::async::run(EVRUN_NOWAIT);
        const int n = peer.read(buffer.data(), buffer.size());
        if (n > 0) {
            out.received.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        // tcp::socket::read() reports the peer's clean close as -1 with the error cleared.
        const int error = qb::io::socket::get_last_errno();
        if (error == 0) {
            out.end = "EOF";
            return out;
        }
        if (!qb::io::socket::not_recv_error(error)) {
            out.end   = "reset";
            out.error = error;
            return out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (steady::now() < deadline);
    return out;
}

/// The backend the loop of this thread runs on ("epoll", "iouring", "poll", "select", "kqueue", ...).
inline std::string
backend_name() {
    return qb::io::async::listener::backend_name(qb::io::async::listener::current.backend());
}

/**
 * @brief When `QB_EV_BACKEND` pins a backend, the loop must be running on it: a run "under io_uring" that fell back
 *        to epoll would prove nothing about io_uring. Called by the fixtures; a no-op when the variable is unset.
 */
inline void
expect_the_backend_asked_for() {
    char const *asked = std::getenv("QB_EV_BACKEND");
    if (!asked || !*asked || std::string_view{asked} == "auto")
        return;
    std::string want{asked};
    if (want == "io_uring") // the alias the listener accepts; backend_name() spells it "iouring"
        want = "iouring";
    EXPECT_EQ(backend_name(), want) << "QB_EV_BACKEND=" << asked << " did not come up: this run is not a witness of it";
}

/**
 * @brief A standalone TCP client -- no server: the shape of every client that owns its connection -- that records
 *        what its two teardown hooks saw, IN the hooks: after the pump a closing socket that lived one pass is gone.
 */
class Client : public qb::io::use<Client>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<Client>;

    std::size_t disconnected_events = 0;
    std::size_t dispose_events      = 0;
    std::size_t messages            = 0;
    int         reason              = 0;

    // In on(event::disconnected&&): the transport as the connection left it.
    ::socket_type handle_at_disconnected  = qb::io::invalid_socket;
    bool          open_in_disconnected    = false;
    unsigned      peer_port_at_disconnect = 0;
    // In on(event::dispose&&): after the transport was ended.
    bool        transport_open_in_dispose  = true;
    bool        descriptor_open_in_dispose = true;
    unsigned    peer_port_in_dispose       = 0;
    std::size_t closing_in_dispose         = 99;

    std::function<void()> disconnected_hook; ///< Runs at the end of on(event::disconnected&&).
    std::function<void()> dispose_hook;      ///< Runs at the start of on(event::dispose&&).

    void
    on(Protocol::message &&) {
        ++messages;
    }

    void
    on(qb::io::async::event::disconnected &&event) {
        ++disconnected_events;
        reason                  = event.reason;
        handle_at_disconnected  = this->transport().native_handle();
        open_in_disconnected    = this->transport().is_open();
        peer_port_at_disconnect = this->transport().peer_endpoint().port();
        if (disconnected_hook)
            disconnected_hook();
    }

    void
    on(qb::io::async::event::dispose &&) {
        if (dispose_hook)
            dispose_hook();
        transport_open_in_dispose  = this->transport().is_open();
        descriptor_open_in_dispose = handle_at_disconnected != qb::io::invalid_socket && descriptor_open(handle_at_disconnected);
        peer_port_in_dispose       = this->transport().peer_endpoint().port();
        closing_in_dispose         = closing();
        ++dispose_events;
    }

    /// The reconnecting clients' shape (qbm-http's HTTP/2 client, qbm-redis, qbm-pgsql): clear what the previous
    /// connection left, install the next transport, start.
    void
    reconnect_over(qb::io::tcp::socket &&next) {
        this->reset_for_reconnect();
        this->template switch_protocol<Protocol>(*this);
        this->transport() = std::move(next);
        this->start();
    }

    /// The modules' teardown (qbm-redis, qbm-pgsql): it completes in the call when no dispatch is on the stack.
    void
    drop_now() {
        this->disconnect_now();
    }
};

/// A listener on a port the system chose, and the socket it accepted.
struct Pair {
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   peer;
    std::uint16_t         port = 0;
};

inline void
listen_on(Pair &pair) {
    ASSERT_EQ(pair.listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    pair.port = pair.listener.local_endpoint().port();
    ASSERT_NE(pair.port, 0u);
}

/// `client` connected to `pair.peer` over loopback, and started.
inline void
connect_pair(Client &client, Pair &pair) {
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    ASSERT_EQ(client.transport().connect_v4("127.0.0.1", pair.port), qb::io::SocketStatus::Done);
    ASSERT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    client.start();
}

/// Runs the loop until `client` has been disposed (both hooks ran: they run in the same `dispose()`).
[[nodiscard]] inline bool
pump_until_disposed(Client const &client, std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
    const auto deadline = steady::now() + budget;
    while (client.dispose_events == 0u && steady::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (client.dispose_events == 0u)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return client.dispose_events != 0u;
}

/// Runs the loop until `pred()` or the budget.
template <typename Pred>
[[nodiscard]] inline bool
pump_while_not(Pred &&pred, std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
    const auto deadline = steady::now() + budget;
    while (!pred() && steady::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (!pred())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

/**
 * @brief The fixture of every case: a loop that starts clean and must end with nothing left -- no socket the loop is
 *        still closing once it is cleared, and, where the process can count them, no descriptor.
 * @details The descriptor census is taken AFTER the loop's first use: the first loop of a process opens descriptors
 *          it keeps (the backend's own, the wake's), so a count taken before would read as one leaked by whichever
 *          case ran first -- the order dependence the first witness of the hundred closes had.
 */
class CleanLoop : public ::testing::Test {
protected:
    int descriptors_at_start = -1;

    void
    SetUp() override {
        qb::io::async::init();
        qb::io::async::run(EVRUN_NOWAIT); // the loop's own descriptors exist from here on
        expect_the_backend_asked_for();
        descriptors_at_start = open_descriptors();
    }

    void
    TearDown() override {
        qb::io::async::listener::current.clear();
        EXPECT_EQ(closing(), 0u) << "a cleared loop still holds a closing socket";
        if (descriptors_at_start >= 0 && !::testing::Test::HasFailure()) {
            EXPECT_EQ(open_descriptors(), descriptors_at_start) << "the case left a descriptor open";
        }
    }
};

} // namespace qb::io::test::close_after_peer

#endif // QB_IO_TEST_CLOSE_AFTER_PEER_SUPPORT_H
