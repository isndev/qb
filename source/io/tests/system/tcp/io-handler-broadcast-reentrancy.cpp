/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the specific terms.
 */

/**
 * @file system/tcp/io-handler-broadcast-reentrancy.cpp
 * @brief `qb::io::async::io_handler` — the NESTED-broadcast guard branches and the disconnected()
 *        erase path that the wave-1 fan-out test (transport-accept-loopback.cpp, PART B) does not reach.
 *
 * transport-accept-loopback.cpp drives the *outer* (first-entry) branch of `stream()` / `stream_if()`:
 * `_broadcast_in_progress` is false, so the broadcaster snapshots into the reusable `_broadcast_scratch`
 * member, fans out, and clears the flag. It never re-enters a broadcast while one is in flight, so the
 * `if (_broadcast_in_progress) { ...local_snapshot... }` re-entrancy branch at the TOP of both
 * `stream()` and `stream_if()` stays uncovered. That branch exists for exactly one situation: a handler
 * invoked *during* a fan-out triggers another broadcast on the same handler — the nested call must NOT
 * reuse the member scratch vector (the outer call is mid-iteration over it) and must instead allocate a
 * fresh local snapshot.
 *
 * We reach it deterministically — no threads racing for the broadcast itself — by driving the nested
 * broadcast from inside a `stream_if` predicate. `stream_if(func, args...)` sets
 * `_broadcast_in_progress = true` and THEN calls `func(*session)` per session; a predicate that calls
 * `server.stream(...)` therefore re-enters `stream()` with the flag already set, taking its
 * local-snapshot branch. The symmetric case has the predicate call `server.stream_if(...)` instead,
 * re-entering `stream_if()`'s own local-snapshot branch. Each nested fan-out is observed end-to-end:
 * the extra line actually reaches the clients, proving the local-snapshot path delivered (not just that
 * it didn't crash).
 *
 * Also covers the `disconnected(uuid)` → `_sessions.erase()` accounting: when a client drops, the
 * server's session_count() falls and the dropped id no longer resolves — the registry self-heals.
 *
 * Topology mirrors the proven wave-1 / session-json layout: the SERVER and all broadcasts live on the
 * MAIN thread's event loop; the probe CLIENTS run on a dedicated worker thread with its own
 * `async::init()` loop. No external daemon, ephemeral `:0` ports, `pump_until`-bounded. `REQUIRES
 * network`.
 *
 * Signatures relied on:
 *   io_handler: _Derived& stream(_Args&&...); _Derived& stream_if(_Func const&, _Args&&...);
 *               std::size_t session_count() const; session_map_t& sessions();
 *               std::shared_ptr<_Session> session(uuid); void disconnected(uuid) (protected, via dispose()).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

class ReentrantServer;

// Newline-delimited echo-style session. The server's broadcasts also land here
// (the fan-out writes through `*session << ...`).
class ReentrantSession : public qb::io::use<ReentrantSession>::tcp::client<ReentrantServer> {
public:
    using Protocol = qb::protocol::text::command<ReentrantSession>;

    explicit ReentrantSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        *this << msg.text << Protocol::end;
    }
};

class ReentrantServer : public qb::io::use<ReentrantServer>::tcp::server<ReentrantSession> {
public:
    void
    on(IOSession &) {}
};

// Throwaway client that connects and counts the lines it receives.
class ProbeClient : public qb::io::use<ProbeClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<ProbeClient>;
    std::atomic<std::size_t> received{0};

    void
    on(Protocol::message &&) {
        ++received;
    }
};

// Drives N probe clients on a dedicated worker thread (its own event loop), exactly
// like the wave-1 / session-json harness. The server lives on the main thread; the
// worker only connects + pumps the clients so the main thread can issue server-side
// broadcasts and observe their delivery via the shared atomics.
class ClientWorker {
public:
    ClientWorker(unsigned short port, int count)
        : _count(count) {
        _thread = std::thread([this, port] {
            qb::io::async::init();
            std::vector<std::unique_ptr<ProbeClient>> clients;
            for (int i = 0; i < _count; ++i) {
                auto c = std::make_unique<ProbeClient>();
                if (c->transport().connect_v4("127.0.0.1", port) == qb::io::SocketStatus::Done) {
                    c->start();
                    clients.push_back(std::move(c));
                    ++_connected;
                }
            }
            while (!_stop.load()) {
                qb::io::async::run_for(5ms);
                std::size_t sum = 0;
                for (auto &c : clients)
                    sum += c->received.load();
                _total_received.store(sum);
            }
        });
    }

    ClientWorker(const ClientWorker &)            = delete;
    ClientWorker &operator=(const ClientWorker &) = delete;

    [[nodiscard]] std::size_t
    total_received() const noexcept {
        return _total_received.load();
    }

    void
    stop_and_join() {
        if (_thread.joinable()) {
            _stop.store(true);
            _thread.join();
        }
    }

    ~ClientWorker() {
        stop_and_join();
    }

private:
    int                      _count;
    std::thread              _thread;
    std::atomic<int>         _connected{0};
    std::atomic<std::size_t> _total_received{0};
    std::atomic<bool>        _stop{false};
};

} // namespace

// ===========================================================================
// stream() re-entered from inside a stream_if() predicate: the nested stream()
// sees _broadcast_in_progress == true and takes its local-snapshot branch. Both
// the outer (filtered) line AND the nested (broadcast) line must reach the clients.
// ===========================================================================
TEST(IoHandlerBroadcastReentrancy, StreamNestedInsideStreamIfPredicateDelivers) {
    qb::io::async::init();

    ReentrantServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    constexpr int kClients = 3;
    ClientWorker  worker(port, kClients);

    // All clients land in the server registry.
    EXPECT_TRUE(pump_until([&] { return server.session_count() == static_cast<std::size_t>(kClients); }, 3s))
        << "not all clients registered; session_count=" << server.session_count();

    // The predicate fires once per session WHILE _broadcast_in_progress is true.
    // On the FIRST invocation it re-enters stream() (the nested broadcast), which
    // must take the local-snapshot branch. Gated so the nested broadcast happens
    // exactly once (one nested line per client), keeping the arithmetic exact.
    std::atomic<bool> nested_fired{false};
    server.stream_if(
        [&](ReentrantSession &) {
            if (!nested_fired.exchange(true)) {
                server.stream(std::string("nested"), '\n');
            }
            return true; // every session also gets the outer filtered line
        },
        std::string("outer"), '\n');

    // Each client must receive BOTH the outer filtered line and the one nested
    // broadcast line => 2 lines per client.
    EXPECT_TRUE(pump_until([&] { return worker.total_received() >= static_cast<std::size_t>(2 * kClients); }, 3s))
        << "nested stream()-in-stream_if() did not deliver both lines; total=" << worker.total_received();
    EXPECT_TRUE(nested_fired.load()) << "the nested broadcast never fired";

    worker.stop_and_join();
    for (int i = 0; i < 20; ++i)
        qb::io::async::run_for(5ms);
    qb::io::async::listener::current.clear();
}

// ===========================================================================
// stream_if() re-entered from inside another stream_if() predicate: the nested
// stream_if() sees _broadcast_in_progress == true and takes ITS local-snapshot
// branch. The nested filtered line must reach the clients alongside the outer one.
// ===========================================================================
TEST(IoHandlerBroadcastReentrancy, StreamIfNestedInsideStreamIfPredicateDelivers) {
    qb::io::async::init();

    ReentrantServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    constexpr int kClients = 2;
    ClientWorker  worker(port, kClients);

    EXPECT_TRUE(pump_until([&] { return server.session_count() == static_cast<std::size_t>(kClients); }, 3s))
        << "not all clients registered; session_count=" << server.session_count();

    // The outer predicate, on its first invocation, re-enters stream_if() while
    // _broadcast_in_progress is already true -> the nested stream_if() takes its
    // local-snapshot branch (it must NOT reuse the member scratch the outer call
    // is mid-iteration over). Gated so the nested fan-out happens exactly once.
    std::atomic<bool> nested_fired{false};
    server.stream_if(
        [&](ReentrantSession &) {
            if (!nested_fired.exchange(true)) {
                server.stream_if([](ReentrantSession &) { return true; }, std::string("nested-if"), '\n');
            }
            return true; // every session also gets the outer filtered line
        },
        std::string("outer"), '\n');

    // Each client receives: the outer filtered line + the one nested-if line => 2.
    EXPECT_TRUE(pump_until([&] { return worker.total_received() >= static_cast<std::size_t>(2 * kClients); }, 3s))
        << "nested stream_if()-in-stream_if() did not deliver both lines; total=" << worker.total_received();
    EXPECT_TRUE(nested_fired.load()) << "the nested stream_if never fired";

    worker.stop_and_join();
    for (int i = 0; i < 20; ++i)
        qb::io::async::run_for(5ms);
    qb::io::async::listener::current.clear();
}

// ===========================================================================
// disconnected() erase path: when a registered client drops its connection, the
// server's session_count() falls and the dropped id stops resolving — the registry
// self-heals via io_handler::disconnected(uuid) -> _sessions.erase().
// ===========================================================================
TEST(IoHandlerBroadcastReentrancy, ClientDropShrinksRegistryViaDisconnected) {
    qb::io::async::init();

    ReentrantServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    constexpr int kClients = 3;

    // A controllable worker: it exposes a flag to drop exactly one client mid-run,
    // so the main thread can observe the server erase that session.
    std::atomic<bool> drop_one{false};
    std::atomic<int>  connected{0};
    std::atomic<bool> stop{false};
    std::thread       worker([&] {
        qb::io::async::init();
        std::vector<std::unique_ptr<ProbeClient>> clients;
        for (int i = 0; i < kClients; ++i) {
            auto c = std::make_unique<ProbeClient>();
            if (c->transport().connect_v4("127.0.0.1", port) == qb::io::SocketStatus::Done) {
                c->start();
                clients.push_back(std::move(c));
                ++connected;
            }
        }
        bool dropped = false;
        while (!stop.load()) {
            if (drop_one.load() && !dropped && !clients.empty()) {
                clients.front()->transport().disconnect();
                clients.erase(clients.begin());
                dropped = true;
            }
            qb::io::async::run_for(5ms);
        }
    });

    EXPECT_TRUE(pump_until([&] { return server.session_count() == static_cast<std::size_t>(kClients); }, 3s))
        << "not all clients registered; session_count=" << server.session_count();

    // Capture the id of one registered session, then drop a client and prove the
    // server erases it. (We can't predict WHICH session the worker drops, so assert
    // on the count shrinking + that the registry shed exactly one entry.)
    ASSERT_FALSE(server.sessions().empty());
    drop_one.store(true);

    EXPECT_TRUE(pump_until([&] { return server.session_count() == static_cast<std::size_t>(kClients - 1); }, 3s))
        << "server never erased the dropped session; session_count=" << server.session_count();
    EXPECT_EQ(server.session_count(), static_cast<std::size_t>(kClients - 1))
        << "exactly one session must have been erased by disconnected()";

    stop.store(true);
    worker.join();
    for (int i = 0; i < 20; ++i)
        qb::io::async::run_for(5ms);
    qb::io::async::listener::current.clear();
}
