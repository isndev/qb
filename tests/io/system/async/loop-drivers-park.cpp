/**
 * @file qb/tests/io/system/async/loop-drivers-park.cpp
 * @brief The standalone loop drivers park in the backend instead of sleeping blind (Huly QB-1103).
 *
 * `run_until`, `run_sync` and `run_for` slept blind on a pass that did nothing -- 50 us, 1 ms and
 * 1 ms -- and a sub-millisecond sleep is served at the millisecond plus the kernel's coalescing on
 * Windows: a reply landing during that sleep was read 1-2.5 ms later, 2 ms per FETCH of a pgsql
 * cursor loop whose round trip costs 300 us. The three now park the loop in its own backend, so a
 * watcher ends the park at poll latency and the cap (1 ms) only bounds a wait that nothing ends.
 *
 * Two shapes, each red on the base of at least one platform:
 *  - a loopback round trip awaited on the loop, driven by `run_until` and by `run_sync`: the median
 *    of 200 is under 500 us (the Windows base reads ~2 ms under both drivers, the Linux base ~1.1 ms
 *    under `run_sync`);
 *  - a loop holding one idle io watcher and nothing to do, driven by `run_until` and by `run_for`:
 *    the loop makes at most two iterations per millisecond (libev's `ev_iteration`, read through
 *    `listener::current.loop().iteration()`) -- an idle pass is one NOWAIT iteration plus one parked
 *    iteration of at least the cap -- against ~9 per millisecond under the 50 us sleep on Linux and a
 *    hot spin under `run_for` on every platform (the Windows base's 2 ms sleep passes this half; the
 *    round-trip half is its red).
 *
 * The peer is a raw blocking thread with no loop of its own, so the only loop in play is the driver's.
 * Serial (LABELS serial): medians and wake counts are read on a quiet process.
 */

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace loop_drivers_park_test {

namespace async = qb::io::async;

// A raw blocking echo peer on loopback, on its own thread, with no event loop: it accepts one
// connection, echoes every byte until the client closes, and exits. The client closes first (the
// tests destroy their client before the peer), which is what ends the blocking read.
class EchoPeer {
public:
    EchoPeer() {
        if (_listener.listen_v4(0, "127.0.0.1") != 0)
            return;
        _listener.set_nonblocking(true);
        _port   = _listener.local_endpoint().port();
        _thread = std::thread([this] { serve(); });
    }
    ~EchoPeer() {
        _stop.store(true, std::memory_order_release);
        if (_thread.joinable())
            _thread.join();
        _listener.disconnect();
    }
    [[nodiscard]] uint16_t
    port() const noexcept {
        return _port;
    }

private:
    void
    serve() {
        qb::io::tcp::socket peer;
        while (!_stop.load(std::memory_order_acquire)) {
            if (_listener.accept(peer) == 0)
                break;
            std::this_thread::sleep_for(1ms);
        }
        if (!peer.is_open())
            return;
        peer.set_nonblocking(false);
        char buf[64];
        for (;;) {
            const int n = peer.read(buf, sizeof buf);
            if (n <= 0 || peer.write(buf, n) != n)
                break;
        }
        peer.disconnect();
    }

    qb::io::tcp::listener _listener;
    uint16_t              _port{0};
    std::thread           _thread;
    std::atomic<bool>     _stop{false};
};

// The client socket of a test: non-blocking, closed on destruction (before the peer, which is
// declared first), which ends the peer's blocking read.
struct Client {
    qb::io::tcp::socket sock;
    explicit Client(uint16_t port) {
        if (sock.connect_v4("127.0.0.1", port) == 0)
            sock.set_nonblocking(true);
    }
    ~Client() {
        sock.disconnect();
    }
};

// One request and its reply per round, the reply awaited on the loop: `wait_readable` arms an io
// watcher, the driver parks, the watcher ends the park. `running` is flipped on the loop thread,
// by this coroutine, once the rounds are done.
async::task<void>
round_trips(qb::io::tcp::socket &sock, int warmups, int rounds, std::vector<double> &us, bool &running) {
    char out = 'x';
    char in[8];
    for (int i = 0; i < warmups + rounds; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        if (sock.write(&out, 1) != 1) {
            ADD_FAILURE() << "write failed at round " << i;
            break;
        }
        int n = 0;
        do {
            co_await async::wait_readable(sock.native_handle());
            n = sock.read(in, sizeof in);
        } while (n < 0 && qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()));
        if (n <= 0) {
            ADD_FAILURE() << "read failed at round " << i;
            break;
        }
        if (i >= warmups)
            us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    running = false;
}

double
median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// An io watcher that never fires: registered for EV_READ on a connected socket nobody writes to.
// It keeps the loop's io count above zero, so every pass asks the backend -- what the 50 us
// sleep used to spin against, and what `run_for` used to spin on for its whole window.
struct IdleWatcher {
    void
    on(async::event::io &) {}
};

constexpr int    kWarmups     = 20;
constexpr int    kRounds      = 200;
constexpr double kMedianCapUs = 500.0;

class LoopDriversParkTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

TEST_F(LoopDriversParkTest, RunUntilReadsAReplyAtPollLatency) {
    EchoPeer peer;
    ASSERT_NE(peer.port(), 0);
    Client client(peer.port());
    ASSERT_TRUE(client.sock.is_open());

    std::vector<double> us;
    bool                running = true;
    async::coro_scheduler().spawn(round_trips(client.sock, kWarmups, kRounds, us, running));
    async::run_until(running);

    ASSERT_EQ(us.size(), static_cast<std::size_t>(kRounds));
    EXPECT_LT(median(us), kMedianCapUs) << "a reply landed during a blind sleep instead of ending the park";
}

TEST_F(LoopDriversParkTest, RunSyncResumesAnAwaitedReplyAtPollLatency) {
    EchoPeer peer;
    ASSERT_NE(peer.port(), 0);
    Client client(peer.port());
    ASSERT_TRUE(client.sock.is_open());

    std::vector<double> us;
    bool                running = true;
    async::run_sync(round_trips(client.sock, kWarmups, kRounds, us, running));

    ASSERT_EQ(us.size(), static_cast<std::size_t>(kRounds));
    EXPECT_FALSE(running);
    EXPECT_LT(median(us), kMedianCapUs) << "the pump slept a millisecond on every awaited reply";
}

TEST_F(LoopDriversParkTest, RunUntilWakesAnIdleLoopAtMostTwiceAMillisecond) {
    EchoPeer peer;
    ASSERT_NE(peer.port(), 0);
    Client client(peer.port());
    ASSERT_TRUE(client.sock.is_open());

    IdleWatcher watcher;
    auto       &idle = async::listener::current.registerEvent<async::event::io>(watcher, client.sock.native_handle(), EV_READ);
    idle.start();

    // The status is flipped on the loop thread, by a timer the loop runs.
    bool running = true;
    async::callback([&running] { running = false; }, 200ms);

    const auto before = async::listener::current.loop().iteration();
    const auto t0     = std::chrono::steady_clock::now();
    async::run_until(running);
    const double   ms         = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const unsigned iterations = async::listener::current.loop().iteration() - before;

    async::listener::current.unregisterEvent(idle._interface);

    EXPECT_GE(ms, 199.0);
    EXPECT_LE(static_cast<double>(iterations), 2.0 * ms + 10.0)
        << iterations << " loop iterations in " << ms << " ms: the idle pass slept blind (or spun) instead of parking";
}

TEST_F(LoopDriversParkTest, RunForWakesAnIdleLoopAtMostTwiceAMillisecond) {
    EchoPeer peer;
    ASSERT_NE(peer.port(), 0);
    Client client(peer.port());
    ASSERT_TRUE(client.sock.is_open());

    IdleWatcher watcher;
    auto       &idle = async::listener::current.registerEvent<async::event::io>(watcher, client.sock.native_handle(), EV_READ);
    idle.start();

    const auto before = async::listener::current.loop().iteration();
    const auto t0     = std::chrono::steady_clock::now();
    async::run_for(200ms);
    const double   ms         = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const unsigned iterations = async::listener::current.loop().iteration() - before;

    async::listener::current.unregisterEvent(idle._interface);

    EXPECT_GE(ms, 199.0);
    EXPECT_LE(static_cast<double>(iterations), 2.0 * ms + 10.0)
        << iterations << " loop iterations in " << ms << " ms: run_for spun on its registered watcher instead of parking";
}

} // namespace loop_drivers_park_test
