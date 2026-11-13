/**
 * @file qb/tests/core/system/engine/actor-close-after-peer.cpp
 * @brief An actor that is a tcp client ends its own connection and dies while the close is under way: what its
 *        core's loop does with the descriptor (Huly QB-1270).
 *
 * The descriptor of a connection an object ended itself leaves the object for its thread's loop, which closes it after
 * the peer (`detail::close_after_peer`). For an actor that loop is its core's:
 *
 *   - the actor may die at once -- in `on(event::disconnected&&)`, or because its connection ended from its own
 *     destructor -- and nothing of the close is left to it;
 *   - a core that lives on keeps the close: the peer may write after our EOF and only then read, and still reads our
 *     last bytes and a clean EOF, where a close at the actor's destruction reset it (on Windows the reset destroys
 *     what the peer has not read yet);
 *   - an engine never waits for a close: a core with no actor left stops at once, and the end of its loop closes
 *     what it held, before `join()` returns -- on a worker thread and on the caller's (`start(false)`) alike.
 *
 * The shape of the last two cases is qbm-redis's `~node_pool()`: a connection ended from the destructor of an actor,
 * in the call (`disconnect_now()`), the engine stopped from outside with the actor alive.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Core
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace actor_close_after_peer_test {

/// What the actor says last: 17 octets, the size of an HTTP/2 GOAWAY without debug data.
constexpr std::string_view LAST = "LAST-BYTES-GOAWAY";

using steady = std::chrono::steady_clock;

std::atomic<int>           g_disconnected{0};
std::atomic<int>           g_disposed{0};
std::atomic<int>           g_destroyed{0};
std::atomic<bool>          g_release_keeper{false};
std::atomic<::socket_type> g_handle{qb::io::invalid_socket};
std::atomic<std::size_t>   g_closing_after_destructor{99};

void
reset_counters() {
    g_disconnected             = 0;
    g_disposed                 = 0;
    g_destroyed                = 0;
    g_release_keeper           = false;
    g_handle                   = qb::io::invalid_socket;
    g_closing_after_destructor = 99;
}

/// Whether `s` still names an open socket of this process.
bool
descriptor_open(::socket_type s) {
    int       type = 0;
    socklen_t len  = sizeof(type);
    return ::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &len) == 0;
}

/// Polls (the engine runs elsewhere) until `pred` holds or `budget` elapses.
template <typename Pred>
bool
wait_for(Pred pred, std::chrono::milliseconds budget) {
    const auto until = steady::now() + budget;
    while (!pred()) {
        if (steady::now() >= until)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// Whether `peer` holds at least `size` octets (at most 64) to read, without reading them.
bool
has_bytes(qb::io::tcp::socket &peer, std::size_t size) {
    peer.set_nonblocking(true);
    char buffer[64];
    return qb::io::socket::recv(peer.native_handle(), buffer, static_cast<int>(size), MSG_PEEK) >= static_cast<int>(size);
}

/// An actor that IS a tcp client: it says its last bytes at once, ends the connection when its peer says "go", and
/// dies in on(event::disconnected&&), the close still under way.
class Conn
    : public qb::Actor
    , public qb::io::use<Conn>::tcp::client<> {
    std::uint16_t _port;

public:
    using Protocol = qb::protocol::text::command<Conn>;

    explicit Conn(std::uint16_t port)
        : _port(port) {}

    ~Conn() {
        ++g_destroyed;
    }

    qb::io::async::task<bool>
    onInit() override {
        if (this->transport().connect_v4("127.0.0.1", _port) != qb::io::SocketStatus::Done)
            co_return false;
        g_handle = this->transport().native_handle();
        this->set_linger(20s); // only the peer or the core's end can end this close within a case
        this->start();
        *this << LAST;
        co_return true;
    }

    void
    on(Protocol::message &&) { // the peer's "go": our last bytes left long ago
        this->disconnect();
    }

    void
    on(qb::io::async::event::disconnected &&) {
        ++g_disconnected;
        kill();
    }

    void
    on(qb::io::async::event::dispose &&) {
        ++g_disposed;
    }
};

/// Keeps its core running until the case lets it go.
class Keeper : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            while (!g_release_keeper.load())
                co_await ctx.sleep(std::chrono::milliseconds{5});
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
};

/// An actor that IS a tcp client and ends its connection from its DESTRUCTOR, in the call (`disconnect_now()`): the
/// shape of qbm-redis's `~node_pool()`. It dies when the engine is stopped from outside.
class DestructorConn
    : public qb::Actor
    , public qb::io::use<DestructorConn>::tcp::client<> {
    std::uint16_t _port;

public:
    using Protocol = qb::protocol::text::command<DestructorConn>;

    explicit DestructorConn(std::uint16_t port)
        : _port(port) {}

    ~DestructorConn() {
        this->disconnect_now();
        g_closing_after_destructor = qb::io::async::detail::closing_sockets();
        ++g_destroyed;
    }

    qb::io::async::task<bool>
    onInit() override {
        if (this->transport().connect_v4("127.0.0.1", _port) != qb::io::SocketStatus::Done)
            co_return false;
        g_handle = this->transport().native_handle();
        this->start();
        *this << LAST;
        co_return true;
    }

    void
    on(Protocol::message &&) {}
};

/// What a peer read: the octets, then how the stream ended within the budget.
struct Read {
    std::string received;
    std::string end   = "open"; ///< "EOF", "reset", or "open" (nothing more within the budget)
    int         error = 0;
};

/// What `peer` reads within `budget` (the engine runs on its own threads: nothing to pump here).
Read
read_all(qb::io::tcp::socket &peer, std::chrono::milliseconds budget) {
    peer.set_nonblocking(true);
    Read       out;
    const auto deadline = steady::now() + budget;
    char       buffer[256];
    do {
        const int n = peer.read(buffer, sizeof(buffer));
        if (n > 0) {
            out.received.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
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
        std::this_thread::sleep_for(1ms);
    } while (steady::now() < deadline);
    return out;
}

/// The engine of the destructor cases: the actor connects, a thread accepts its peer and stops the engine 200 ms
/// later, the actor still alive.
void
run_destructor_case(bool async, qb::io::tcp::socket &peer) {
    reset_counters();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();

    qb::Main engine;
    engine.addActor<DestructorConn>(0, port);
    std::thread stopper([&] {
        static_cast<void>(listener.accept(peer));
        std::this_thread::sleep_for(200ms);
        qb::Main::stop(); // the actor is alive: the stop kills it
    });
    engine.start(async);
    engine.join();
    stopper.join();
    EXPECT_FALSE(engine.hasError());
    EXPECT_EQ(g_destroyed.load(), 1);
}

/// An actor whose onInit() fails: its core never enters its workflow, and every other actor of the core is still
/// alive when the core is torn down.
class FailingInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return false;
    }
};

std::atomic<int> g_spawned_ran{0};

/// Ends its connection from its destructor -- a closing socket on the loop -- AND spawns a coroutine there.
class SpawnInDestructor
    : public qb::Actor
    , public qb::io::use<SpawnInDestructor>::tcp::client<> {
    std::uint16_t _port;

public:
    using Protocol = qb::protocol::text::command<SpawnInDestructor>;

    explicit SpawnInDestructor(std::uint16_t port)
        : _port(port) {}

    ~SpawnInDestructor() {
        qb::io::async::listener::current.coro_scheduler().spawn([]() -> qb::io::async::task<void> {
            ++g_spawned_ran; // runs only if something resumes the frame after the core's actors are gone
            co_return;
        });
        this->disconnect_now();
    }

    qb::io::async::task<bool>
    onInit() override {
        if (this->transport().connect_v4("127.0.0.1", _port) != qb::io::SocketStatus::Done)
            co_return false;
        this->start();
        co_return true;
    }

    void
    on(Protocol::message &&) {}
};

std::atomic<int> g_released{0};

/// Spawns a coroutine when it is released: state a deferred callback may hold.
struct SpawnsWhenReleased {
    SpawnsWhenReleased()                                      = default;
    SpawnsWhenReleased(SpawnsWhenReleased const &)            = delete;
    SpawnsWhenReleased &operator=(SpawnsWhenReleased const &) = delete;

    ~SpawnsWhenReleased() {
        ++g_released;
        qb::io::async::listener::current.coro_scheduler().spawn([]() -> qb::io::async::task<void> {
            ++g_spawned_ran; // runs only if a later pass of this thread's loop finds the frame
            co_return;
        });
    }
};

/// Defers a callback on its core's loop from its destructor; the callback holds a SpawnsWhenReleased.
class DeferInDestructor : public qb::Actor {
public:
    ~DeferInDestructor() {
        qb::io::async::defer([held = std::make_shared<SpawnsWhenReleased>()] { static_cast<void>(held); });
    }

    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

} // namespace actor_close_after_peer_test

using namespace actor_close_after_peer_test;

// The actor's core has no other actor: it stops as soon as the actor dies, the close still under way. The engine joins
// at once -- nothing waits for the close --, the peer, silent until then, reads our last bytes and a clean EOF, and the
// descriptor went with the core's loop: the peer's later writes fail.
TEST(ActorCloseAfterPeer, AnActorThatEndsItsConnectionAndDiesLetsItsCoreStopAtOnce) {
    reset_counters();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();

    qb::Main engine;
    engine.addActor<Conn>(0, port);
    engine.start();
    qb::io::tcp::socket peer;
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    ASSERT_TRUE(wait_for([&] { return has_bytes(peer, LAST.size()); }, 2000ms)) << "our last bytes never reached the peer";
    std::atomic<bool> joined{false};
    std::thread       watchdog([&] { // an engine that waits for the close is stopped, and the case fails, instead of hanging
        if (!wait_for([&] { return joined.load(); }, 10000ms))
            qb::Main::stop();
    });
    const auto        go_at = steady::now();
    const int         wrote = peer.write("go\n", 3);
    engine.join();
    const double joined_ms = std::chrono::duration<double, std::milli>(steady::now() - go_at).count();
    joined                 = true;
    watchdog.join();
    ASSERT_EQ(wrote, 3);
    EXPECT_FALSE(engine.hasError());
    EXPECT_EQ(g_disconnected.load(), 1);
    EXPECT_EQ(g_disposed.load(), 1) << "on(event::dispose&&) never ran";
    EXPECT_EQ(g_destroyed.load(), 1);
    EXPECT_LT(joined_ms, 5000.) << "the engine waited for the close (linger 20 s)";

    const Read got = read_all(peer, 1500ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_EQ(got.end, "EOF") << "then a clean EOF, not a reset (error " << got.error << ")";
    static_cast<void>(peer.write("x", 1)); // draws the reset of a closed descriptor
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(peer.write("x", 1), -1) << "the descriptor outlived the core";
}

// Another actor keeps the core alive. The peer writes AFTER the actor is gone, and only then reads: our last bytes and
// a clean EOF. The descriptor is the core's loop's until the peer closes.
TEST(ActorCloseAfterPeer, AnActorThatEndsItsConnectionAndDiesLeavesTheCloseToItsCoresLoop) {
    reset_counters();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();

    qb::Main engine;
    engine.addActor<Conn>(0, port);
    engine.addActor<Keeper>(0);
    engine.start();
    struct Release {
        ~Release() {
            g_release_keeper = true; // however the case ends: the engine's destructor then stops and joins it
        }
    };
    [[maybe_unused]] Release release;

    qb::io::tcp::socket peer;
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    ASSERT_TRUE(wait_for([&] { return has_bytes(peer, LAST.size()); }, 2000ms)) << "our last bytes never reached the peer";
    ASSERT_EQ(peer.write("go\n", 3), 3);
    ASSERT_TRUE(wait_for([&] { return g_destroyed.load() == 1; }, 2000ms)) << "the actor did not die after it ended its connection";
    EXPECT_EQ(g_disposed.load(), 1) << "on(event::dispose&&) never ran";
    const ::socket_type handle = g_handle.load();
    EXPECT_TRUE(descriptor_open(handle)) << "the descriptor closed with the actor: a reset for a peer that writes again, which on "
                                            "Windows destroys our last bytes";

    const std::string late(4096, 'u');
    EXPECT_EQ(peer.write(late.data(), late.size()), static_cast<int>(late.size())) << "the peer's write after our EOF failed";
    std::this_thread::sleep_for(200ms); // the reset that write would draw from a closed socket
    const Read got = read_all(peer, 1000ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost to the actor's destruction";
    EXPECT_EQ(got.end, "EOF") << "then a clean EOF, not a reset (error " << got.error << ")";

    peer.close();
    EXPECT_TRUE(wait_for([&] { return !descriptor_open(handle); }, 5000ms)) << "the core's loop kept the descriptor after the peer closed";
    g_release_keeper = true;
    engine.join();
    EXPECT_FALSE(engine.hasError());
}

// The engine runs on the caller's thread (`start(false)`) and is stopped with the actor alive: the close its destructor
// made is the core's loop's -- this very thread's -- and the core's end closes it, before join() returns, with nothing
// left in the caller's loop. The silent peer reads our last bytes and a clean EOF.
TEST(ActorCloseAfterPeer, AnActorThatEndsItsConnectionFromItsDestructorOnTheCallersThreadLeavesNothingBehind) {
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(run_destructor_case(false, peer));
    const ::socket_type handle = g_handle.load();
    EXPECT_EQ(g_closing_after_destructor.load(), 1u) << "the destructor's close was not given to the loop";
    EXPECT_FALSE(descriptor_open(handle)) << "the descriptor outlived the core that ran on this thread";
    EXPECT_EQ(qb::io::async::detail::closing_sockets(), 0u) << "the core's end left a close in the caller's loop";
    const Read got = read_all(peer, 1000ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_EQ(got.end, "EOF") << "then a clean EOF, not a reset (error " << got.error << ")";
    qb::io::async::listener::current.clear();
}

// The same on a worker thread (`start(true)`): the core's loop closes it when the core ends, before join() returns, and
// the caller's loop holds nothing.
TEST(ActorCloseAfterPeer, AnActorThatEndsItsConnectionFromItsDestructorOnAWorkerThreadLeavesNothingBehind) {
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(run_destructor_case(true, peer));
    const ::socket_type handle = g_handle.load();
    EXPECT_EQ(g_closing_after_destructor.load(), 1u) << "the destructor's close was not given to the loop";
    EXPECT_FALSE(descriptor_open(handle)) << "the descriptor outlived the core's thread";
    EXPECT_EQ(qb::io::async::detail::closing_sockets(), 0u) << "the close landed in the caller's loop";
    const Read got = read_all(peer, 1000ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_EQ(got.end, "EOF") << "then a clean EOF, not a reset (error " << got.error << ")";
}

// A core torn down with actors still alive -- another actor's onInit() failed -- destroys them AFTER it has cleared its
// loop. One that ends its connection in its destructor then gives its descriptor to a loop already cleared: on the
// caller's thread (start(false)) that loop outlives the engine, and nothing would run it again. The core clears it
// once more when its last actor is gone: the engine leaves no closing socket and no descriptor behind.
TEST(ActorCloseAfterPeer, AnActorStillAliveWhenItsCoreIsTornDownLeavesNoCloseInTheCallersLoop) {
    reset_counters();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();
    qb::io::tcp::socket peer;
    {
        qb::Main engine;
        engine.addActor<DestructorConn>(0, port);
        engine.addActor<FailingInit>(0);
        engine.start(false); // the init fails: the core is torn down on this thread, its actors still alive
        engine.join();
        EXPECT_TRUE(engine.hasError());
    }
    // Read before the peer is accepted: nothing has been opened since the engine ended, so the number still names the
    // actor's descriptor or nothing -- an accept would take the number the engine's close just freed.
    EXPECT_EQ(g_destroyed.load(), 1);
    EXPECT_EQ(g_closing_after_destructor.load(), 1u) << "the destructor's close was not given to the loop: the case proves nothing";
    EXPECT_EQ(qb::io::async::detail::closing_sockets(), 0u) << "the core's end left a close in the caller's loop";
    EXPECT_FALSE(descriptor_open(g_handle.load())) << "the descriptor outlived the engine that ran on this thread";
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    qb::io::async::listener::current.clear();
}

// The core's second clear() must not RUN what its teardown queued: clear() makes loop passes, and a pass resumes ready
// coroutines. A coroutine an actor's destructor spawned during the teardown is destroyed with the core, never run --
// as before the core cleared its loop again -- and the destructor's closing socket is gone all the same.
TEST(ActorCloseAfterPeer, ACoroutineADestructorSpawnedDuringTheCoresTeardownIsDestroyedNotRun) {
    g_spawned_ran = 0;
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();
    qb::io::tcp::socket peer;
    {
        qb::Main engine;
        engine.addActor<SpawnInDestructor>(0, port);
        engine.addActor<FailingInit>(0);
        engine.start(false); // the init fails: the core is torn down on this thread, its actors still alive
        engine.join();
        EXPECT_TRUE(engine.hasError());
    }
    EXPECT_EQ(g_spawned_ran.load(), 0) << "a frame spawned during the core's teardown was run after its actors were destroyed";
    EXPECT_EQ(qb::io::async::detail::closing_sockets(), 0u) << "the core's end left a close in the caller's loop";
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    qb::io::async::listener::current.clear();
}

// What the core's second clear() releases may queue a coroutine in its turn: the callback an actor's destructor
// deferred is dropped by that clear(), never run, and the state it held spawns when it is released. Under start(false)
// the thread outlives the engine, so the frame would wait in the caller's scheduler for the next pass of that loop --
// the next engine's, or the program's own -- and run there, after everything it was spawned for is gone. The core
// recycles its scheduler once more after the clear(): the frame is destroyed with the core.
TEST(ActorCloseAfterPeer, ACoroutineQueuedWhileTheCoreClearsItsLoopIsDestroyedWithTheCore) {
    g_spawned_ran = 0;
    g_released    = 0;
    {
        qb::Main engine;
        engine.addActor<DeferInDestructor>(0);
        engine.addActor<FailingInit>(0);
        engine.start(false); // the init fails: the core is torn down on this thread, its actors still alive
        engine.join();
        EXPECT_TRUE(engine.hasError());
    }
    ASSERT_EQ(g_released.load(), 1) << "the core's end did not drop the callback its actor's destructor deferred: the case proves nothing";
    for (int pass = 0; pass < 4; ++pass)
        qb::io::async::run(EVRUN_NOWAIT); // the thread's loop, run again after the engine
    EXPECT_EQ(g_spawned_ran.load(), 0) << "a frame queued by the core's last clear() outlived the core and was run";
    qb::io::async::listener::current.clear();
    qb::io::async::listener::current.recycle_coro_scheduler();
}
