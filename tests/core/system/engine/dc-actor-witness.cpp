/**
 * SCRATCH, NEVER COMMITTED -- design-close (train 7): the actor case.
 *
 * An actor that IS a standalone tcp client ends its connection itself -- from its message handler, when its peer says
 * "go" -- and dies in on(event::disconnected&&): the actor is destroyed while the close is under way.
 *
 *   A  its core has no other actor and stops at once. The peer, silent since, then reads. What must hold: the engine
 *      joins at once (nothing waits for the close), nothing crashes, the peer reads our last bytes and a clean EOF, and
 *      the descriptor went with the core's loop (the peer's later writes fail).
 *   B  another actor keeps the core alive. The peer writes AFTER the actor is gone, and only then reads: does it still
 *      get our last bytes? (A close at the actor's destruction resets it on that write.)
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace dc_actor_witness_test {

constexpr std::string_view LAST = "LAST-BYTES-GOAWAY";

std::atomic<int>    g_disconnected{0};
std::atomic<int>    g_disposed{0};
std::atomic<int>    g_destroyed{0};
std::atomic<double> g_died_ms{-1};

std::chrono::steady_clock::time_point g_t0;

double
now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t0).count();
}

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
        g_died_ms = now_ms();
    }

    qb::io::async::task<bool>
    onInit() override {
        if (this->transport().connect_v4("127.0.0.1", _port) != qb::io::SocketStatus::Done)
            co_return false;
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
        kill(); // destroyed while the close is under way
    }

    void
    on(qb::io::async::event::dispose &&) {
        ++g_disposed;
    }
};

/// Keeps its core running for a while after the connection's actor is gone.
class Keeper : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(std::chrono::milliseconds{1200});
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
};

struct Seen {
    std::string got;
    char const *end   = "open";
    int         error = 0;
};

Seen
read_peer(qb::io::tcp::socket &peer, std::chrono::milliseconds budget) {
    Seen seen;
    peer.set_nonblocking(true);
    const auto until = std::chrono::steady_clock::now() + budget;
    char       buffer[256];
    while (std::chrono::steady_clock::now() < until) {
        const int n = peer.read(buffer, sizeof(buffer));
        if (n > 0) {
            seen.got.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        const int error = qb::io::socket::get_last_errno();
        if (error == 0) {
            seen.end = "EOF";
            break;
        }
        if (!qb::io::socket::not_recv_error(error)) {
            seen.end   = "RESET";
            seen.error = error;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    return seen;
}

void
reset_counters() {
    g_disconnected = g_disposed = g_destroyed = 0;
    g_died_ms                                 = -1;
}

} // namespace dc_actor_witness_test

using namespace dc_actor_witness_test;

TEST(DcActor, ATheActorDiesDuringTheCloseAndItsCoreStops) {
    reset_counters();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();

    qb::Main engine;
    engine.addActor<Conn>(0, port);
    engine.start();
    qb::io::tcp::socket peer;
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    std::this_thread::sleep_for(100ms); // our last bytes are in the peer's receive queue
    g_t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(peer.write("go\n", 3), 3);
    engine.join();
    const double joined_ms = now_ms();
    EXPECT_FALSE(engine.hasError());

    const Seen seen    = read_peer(peer, 1500ms);
    const int  wrote_1 = peer.write("x", 1);
    std::this_thread::sleep_for(100ms);
    const int wrote_2 = peer.write("x", 1);
    std::printf("DC-ACT A core stops: disconnected=%d disposed=%d destroyed=%d, the actor died %.1f ms after 'go', the engine joined at %.1f "
                "ms; the peer read '%s' then %s (error %d); its writes after the join: %d, %d\n",
                g_disconnected.load(), g_disposed.load(), g_destroyed.load(), g_died_ms.load(), joined_ms, seen.got.c_str(), seen.end,
                seen.error, wrote_1, wrote_2);
    EXPECT_EQ(g_destroyed.load(), 1);
    EXPECT_EQ(g_disconnected.load(), 1);
    EXPECT_EQ(g_disposed.load(), 1) << "on(event::dispose&&) never ran";
    EXPECT_LE(joined_ms, 1000.) << "the engine waited for the close";
    EXPECT_EQ(seen.got, LAST) << "our last bytes were lost";
    EXPECT_STREQ(seen.end, "EOF");
    EXPECT_EQ(wrote_2, -1) << "the descriptor outlived the core";
}

TEST(DcActor, BTheActorDiesDuringTheCloseAndItsCoreLivesOn) {
    reset_counters();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();

    qb::Main engine;
    engine.addActor<Conn>(0, port);
    engine.addActor<Keeper>(0);
    engine.start();
    qb::io::tcp::socket peer;
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    std::this_thread::sleep_for(100ms);
    g_t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(peer.write("go\n", 3), 3);
    std::this_thread::sleep_for(200ms); // the actor closed and died; its core runs on
    const double      died_ms = g_died_ms.load();
    const std::string late(4096, 'u');
    const int         wrote_late = peer.write(late.data(), late.size()); // after our EOF, before the peer reads anything
    std::this_thread::sleep_for(200ms);
    const Seen seen = read_peer(peer, 500ms);
    peer.close();
    engine.join();
    const double joined_ms = now_ms();
    EXPECT_FALSE(engine.hasError());
    std::printf("DC-ACT B core lives on: disposed=%d destroyed=%d, the actor died %.1f ms after 'go'; the peer wrote %d octets 200 ms "
                "later, then read '%s' then %s (error %d); the engine joined at %.0f ms\n",
                g_disposed.load(), g_destroyed.load(), died_ms, wrote_late, seen.got.c_str(), seen.end, seen.error, joined_ms);
    EXPECT_EQ(g_destroyed.load(), 1);
    EXPECT_EQ(g_disposed.load(), 1) << "on(event::dispose&&) never ran";
    EXPECT_GE(died_ms, 0.) << "the actor was still alive 200 ms after it ended its connection";
    EXPECT_EQ(seen.got, LAST) << "our last bytes were lost to the actor's destruction";
    EXPECT_STREQ(seen.end, "EOF");
}
