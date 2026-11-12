/**
 * SCRATCH, NEVER COMMITTED -- design-close (train 7): how a qb-io connection ends when WE end it.
 *
 * One binary, run unchanged against the base and against each prototype. Every case prints a `DC-W` line with what it
 * observed, then asserts the contract the design is after:
 *   W1  a peer that keeps writing after our disconnect() is cut within linger() + a margin, and our descriptor closes;
 *   W2  a peer that closes on our EOF ends the close at once (no wait for the bound);
 *   W3  our last bytes reach a peer although its own bytes were left unread -- read late, and after it wrote again;
 *   W4  the object destroyed right after its dispose: same delivery, no descriptor left;
 *   W5  the object reconnected right after its dispose: the old peer still gets our last bytes and a clean EOF, the
 *       new connection works, the old descriptor closes;
 *   W6  no descriptor leaks over the whole run (Linux: /proc/self/fd), no socket left half-closed (ss).
 */
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#if __has_include(<qb/io/async/lingering_close.h>)
#include <qb/io/async/lingering_close.h>
#define DC_HAS_CLOSER 1
#endif

#if defined(__linux__)
#include <dirent.h>
#endif

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace dc_close_witness_test {

constexpr std::string_view LAST = "LAST-BYTES-GOAWAY"; // 17 octets: an HTTP/2 GOAWAY without debug data

class Client : public qb::io::use<Client>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<Client>;

    std::size_t disconnected_events = 0;
    std::size_t dispose_events      = 0;
    std::size_t messages            = 0;

    void
    on(Protocol::message &&) {
        ++messages;
    }
    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
    }
    void
    on(qb::io::async::event::dispose &&) {
        ++dispose_events;
    }

    /// The reconnecting clients' shape (qbm-http's HTTP/2 client, qbm-pgsql): clear what the previous connection left,
    /// install the next transport, start.
    void
    reconnect_over(qb::io::tcp::socket &&next) {
        this->reset_for_reconnect();
        this->template switch_protocol<Protocol>(*this);
        this->transport() = std::move(next);
        this->start();
    }
};

double
ms_since(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

/// Whether `s` still names an open socket of this process.
bool
descriptor_open(::socket_type s) {
    int       type = 0;
    socklen_t len  = sizeof(type);
    return ::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &len) == 0;
}

int
open_descriptors() {
#if defined(__linux__)
    int  n = 0;
    DIR *d = ::opendir("/proc/self/fd");
    if (!d)
        return -1;
    while (::readdir(d))
        ++n;
    ::closedir(d);
    return n - 3; // ".", "..", the directory's own descriptor
#else
    return -1;
#endif
}

void
dump_sockets(char const *when, std::uint16_t port) {
#if defined(__linux__)
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "ss -tanH '( sport = :%u or dport = :%u )' 2>/dev/null", port, port);
    if (FILE *p = ::popen(cmd, "r")) {
        char line[512];
        int  n = 0;
        while (std::fgets(line, sizeof(line), p)) {
            std::printf("DC-W   ss[%s] %s", when, line);
            ++n;
        }
        ::pclose(p);
        std::printf("DC-W   ss[%s] %d socket(s) on port %u\n", when, n, port);
    }
#else
    (void) when;
    (void) port;
#endif
}

struct Pair {
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   peer;
    std::uint16_t         port = 0;
};

void
listen_on(Pair &pair) {
    ASSERT_EQ(pair.listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    pair.port = pair.listener.local_endpoint().port();
    ASSERT_NE(pair.port, 0u);
}

void
connect_pair(Client &client, Pair &pair) {
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    ASSERT_EQ(client.transport().connect_v4("127.0.0.1", pair.port), qb::io::SocketStatus::Done);
    ASSERT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    client.start();
}

void
pump(std::chrono::milliseconds how_long) {
    const auto until = std::chrono::steady_clock::now() + how_long;
    while (std::chrono::steady_clock::now() < until) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
}

struct Read {
    std::string received;
    char const *end   = "open"; // "EOF", "RESET", "open" (nothing more within the budget)
    int         error = 0;
};

/// What `peer` reads within `budget`, the loop pumped meanwhile: up to a clean EOF, a reset, or the budget.
Read
read_all(qb::io::tcp::socket &peer, std::chrono::milliseconds budget = 1000ms) {
    peer.set_nonblocking(true);
    Read                  out;
    const auto            deadline = std::chrono::steady_clock::now() + budget;
    std::array<char, 512> buffer{};
    do {
        qb::io::async::run(EVRUN_NOWAIT);
        const int n = peer.read(buffer.data(), buffer.size());
        if (n > 0) {
            out.received.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        const int error = qb::io::socket::get_last_errno();
        if (error == 0) {
            out.end = "EOF";
            return out;
        }
        if (!qb::io::socket::not_recv_error(error)) {
            out.end   = "RESET";
            out.error = error;
            return out;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return out;
}

std::size_t
closing_now() {
#ifdef DC_HAS_CLOSER
    return qb::io::async::detail::closing_sockets();
#else
    return 0;
#endif
}

class DcClose : public ::testing::Test {
protected:
    int fds_before = -1;
    void
    SetUp() override {
        qb::io::async::init();
        fds_before = open_descriptors();
    }
    void
    TearDown() override {
        const int after = open_descriptors();
        std::printf("DC-W   descriptors before=%d after=%d (before clear()) closing=%zu\n", fds_before, after, closing_now());
        qb::io::async::listener::current.clear();
        std::printf("DC-W   descriptors after clear()=%d closing=%zu\n", open_descriptors(), closing_now());
    }
};

} // namespace dc_close_witness_test

using namespace dc_close_witness_test;

// W1 -- the part 6 witness: after our disconnect() the peer writes 64 KiB chunks and never reads.
TEST_F(DcClose, W1APeerThatKeepsWritingIsCutWithinTheBound) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    const auto bound  = std::chrono::duration_cast<std::chrono::milliseconds>(client.linger());

    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    std::printf("DC-W W1 disposed: transport open=%d descriptor open=%d closing=%zu linger=%lldms\n", client.transport().is_open() ? 1 : 0,
                descriptor_open(handle) ? 1 : 0, closing_now(), static_cast<long long>(bound.count()));

    pair.peer.set_nonblocking(true);
    const auto              start = std::chrono::steady_clock::now();
    const std::vector<char> chunk(64 * 1024, 'w');
    std::size_t             accepted    = 0;
    double                  first_block = -1, failed_at = -1, closed_at = -1, last_progress = 0;
    int                     fail_errno = 0;
    while (ms_since(start) < static_cast<double>(bound.count()) + 3000.) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (closed_at < 0 && !descriptor_open(handle))
            closed_at = ms_since(start);
        const int n = pair.peer.write(chunk.data(), chunk.size());
        if (n > 0) {
            accepted += static_cast<std::size_t>(n);
            last_progress = ms_since(start);
            continue;
        }
        const int error = qb::io::socket::get_last_errno();
        if (n < 0 && !qb::io::socket::not_send_error(error)) {
            failed_at  = ms_since(start);
            fail_errno = error;
            break;
        }
        if (first_block < 0)
            first_block = ms_since(start);
        std::this_thread::sleep_for(1ms);
    }
    if (closed_at < 0 && !descriptor_open(handle))
        closed_at = ms_since(start);
    std::printf("DC-W W1 accepted=%zu first_would_block_ms=%.0f last_progress_ms=%.0f write_failed_ms=%.0f errno=%d "
                "descriptor_closed_ms=%.0f closing=%zu\n",
                accepted, first_block, last_progress, failed_at, fail_errno, closed_at, closing_now());
    dump_sockets("W1 end", pair.port);
    EXPECT_GE(failed_at, 0.) << "a peer that keeps writing must see its writes fail: it blocked for ever";
    EXPECT_LE(failed_at, static_cast<double>(bound.count()) + 1000.) << "within the bound and a margin";
    EXPECT_GE(closed_at, 0.) << "the descriptor must be closed by then";
}

// W2 -- the common case: the peer closes when it reads our EOF. Nothing may wait for the bound.
TEST_F(DcClose, W2APeerThatClosesEndsTheCloseAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();

    client.publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.out().size() == 0u; }));
    client.disconnect();
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    const double disconnected_ms = ms_since(start);
    const Read   got             = read_all(pair.peer);
    pair.peer.close();
    double closed_at = -1, disposed_ms = client.dispose_events ? disconnected_ms : -1;
    while (ms_since(start) < 1500.) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (disposed_ms < 0 && client.dispose_events)
            disposed_ms = ms_since(start);
        if (!descriptor_open(handle)) {
            closed_at = ms_since(start);
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    pump(20ms);
    if (disposed_ms < 0 && client.dispose_events)
        disposed_ms = ms_since(start);
    std::printf("DC-W W2 disconnected_ms=%.1f dispose_ms=%.1f peer read '%s' then %s; descriptor_closed_ms=%.1f closing=%zu\n", disconnected_ms,
                disposed_ms, got.received.c_str(), got.end, closed_at, closing_now());
    dump_sockets("W2 end", pair.port);
    EXPECT_EQ(got.received, LAST);
    EXPECT_STREQ(got.end, "EOF");
    EXPECT_GE(closed_at, 0.) << "the peer closed: our descriptor must close at once, not at the bound or never";
    EXPECT_LE(closed_at, 500.);
    EXPECT_EQ(closing_now(), 0u);
}

// W3 -- the GOAWAY case: the peer's own bytes are unread when we end, it writes again, and only then reads.
TEST_F(DcClose, W3OurLastBytesReachAPeerWhoseInputWeLeftUnread) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();

    client.publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.out().size() == 0u; }));
    const std::vector<char> unread(4096, 'u');
    ASSERT_EQ(pair.peer.write(unread.data(), unread.size()), 4096); // no loop pass reads it: the disconnect is handled first
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    pump(300ms); // a reset, if the close sent one, has reached the peer
    const int wrote_again = pair.peer.write("again", 5);
    pump(300ms); // and the reset its own write may have drawn
    const Read got = read_all(pair.peer);
    std::printf("DC-W W3 peer wrote again=%d; read '%s' then %s (error %d); descriptor open=%d closing=%zu\n", wrote_again,
                got.received.c_str(), got.end, got.error, descriptor_open(handle) ? 1 : 0, closing_now());
    pair.peer.close();
    pump(100ms);
    std::printf("DC-W W3 after the peer closed: descriptor open=%d closing=%zu\n", descriptor_open(handle) ? 1 : 0, closing_now());
    dump_sockets("W3 end", pair.port);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_STREQ(got.end, "EOF") << "then a clean EOF, not a reset";
    EXPECT_FALSE(descriptor_open(handle)) << "nothing left once the peer closed";
}

// W4 -- the object destroyed right after its dispose, the close still under way.
TEST_F(DcClose, W4TheObjectDestroyedDuringTheClose) {
    auto client = std::make_unique<Client>();
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(*client, pair));
    const auto handle = client->transport().native_handle();

    client->publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client->out().size() == 0u; }));
    const std::vector<char> unread(4096, 'u');
    ASSERT_EQ(pair.peer.write(unread.data(), unread.size()), 4096);
    client->disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client->disconnected_events == 1u; }));
    client.reset(); // gone
    pump(300ms);
    const int wrote_again = pair.peer.write("again", 5);
    pump(300ms);
    const Read got = read_all(pair.peer);
    std::printf("DC-W W4 after destruction: peer wrote again=%d; read '%s' then %s (error %d); descriptor open=%d closing=%zu\n", wrote_again,
                got.received.c_str(), got.end, got.error, descriptor_open(handle) ? 1 : 0, closing_now());
    pair.peer.close();
    pump(100ms);
    std::printf("DC-W W4 after the peer closed: descriptor open=%d closing=%zu\n", descriptor_open(handle) ? 1 : 0, closing_now());
    dump_sockets("W4 end", pair.port);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_STREQ(got.end, "EOF");
    EXPECT_FALSE(descriptor_open(handle));
}

// W5 -- the object reconnected right after its dispose (the HTTP/2 client's, redis's, pgsql's shape).
TEST_F(DcClose, W5TheObjectReconnectedDuringTheClose) {
    Client client;
    Pair   old_pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, old_pair));
    const auto old_handle = client.transport().native_handle();

    client.publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.out().size() == 0u; }));
    const std::vector<char> unread(4096, 'u');
    ASSERT_EQ(old_pair.peer.write(unread.data(), unread.size()), 4096);
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));

    Pair new_pair;
    ASSERT_NO_FATAL_FAILURE(listen_on(new_pair));
    qb::io::tcp::socket next;
    ASSERT_EQ(next.connect_v4("127.0.0.1", new_pair.port), qb::io::SocketStatus::Done);
    ASSERT_EQ(new_pair.listener.accept(new_pair.peer), qb::io::SocketStatus::Done);
    const auto start = std::chrono::steady_clock::now();
    client.reconnect_over(std::move(next));
    const double reconnect_ms = ms_since(start);
    const auto   new_handle   = client.transport().native_handle();
    client.publish(std::string_view{"next"});
    pump(300ms);
    const int wrote_again = old_pair.peer.write("again", 5);
    pump(300ms);
    const Read old_got = read_all(old_pair.peer);
    const Read new_got = read_all(new_pair.peer, 200ms);
    ASSERT_EQ(new_pair.peer.write("pong\n", 5), 5);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.messages == 1u; })) << "the new connection reads nothing";
    std::printf("DC-W W5 reconnect took %.2f ms (handles old=%lld new=%lld); old peer wrote again=%d, read '%s' then %s (error %d); new peer "
                "read '%s'; old descriptor open=%d closing=%zu disposes=%zu\n",
                reconnect_ms, static_cast<long long>(old_handle), static_cast<long long>(new_handle), wrote_again, old_got.received.c_str(),
                old_got.end, old_got.error, new_got.received.c_str(), descriptor_open(old_handle) && old_handle != new_handle ? 1 : 0,
                closing_now(), client.dispose_events);
    old_pair.peer.close();
    pump(100ms);
    std::printf("DC-W W5 after the old peer closed: old descriptor open=%d closing=%zu; the new connection connected=%d\n",
                descriptor_open(old_handle) && old_handle != new_handle ? 1 : 0, closing_now(), client.is_connected() ? 1 : 0);
    dump_sockets("W5 end (old)", old_pair.port);
    EXPECT_EQ(old_got.received, LAST) << "the old connection's last bytes were lost to the reconnection";
    EXPECT_STREQ(old_got.end, "EOF");
    EXPECT_EQ(new_got.received, "next");
    EXPECT_TRUE(client.is_connected());
    EXPECT_EQ(client.dispose_events, 1u) << "the old connection's close must not dispose the new one";
    EXPECT_LE(reconnect_ms, 50.) << "a reconnection must not wait for the old connection's close";
}

// W7 -- when does on(event::dispose&&) run against a peer that neither closes nor writes? (the contract A moves)
TEST_F(DcClose, W7TheDisposeHookAgainstASilentPeer) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    const auto bound  = std::chrono::duration_cast<std::chrono::milliseconds>(client.linger());
    client.disconnect();
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    const double disconnected_ms = ms_since(start);
    double       disposed_ms = -1, closed_ms = -1;
    while (ms_since(start) < static_cast<double>(bound.count()) + 1500.) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (disposed_ms < 0 && client.dispose_events)
            disposed_ms = ms_since(start);
        if (closed_ms < 0 && !descriptor_open(handle))
            closed_ms = ms_since(start);
        if (disposed_ms >= 0 && closed_ms >= 0)
            break;
        std::this_thread::sleep_for(1ms);
    }
    std::printf("DC-W W7 silent peer: disconnected_ms=%.1f dispose_ms=%.1f descriptor_closed_ms=%.1f (linger %lld ms) closing=%zu\n",
                disconnected_ms, disposed_ms, closed_ms, static_cast<long long>(bound.count()), closing_now());
    EXPECT_GE(disposed_ms, 0.);
    EXPECT_LE(disposed_ms, 100.) << "on(event::dispose&&) waited for the peer";
    EXPECT_GE(closed_ms, 0.) << "the descriptor was never closed";
    EXPECT_LE(closed_ms, static_cast<double>(bound.count()) + 1000.);
}

// W8 -- the loop goes first (listener::clear(), a thread ending) with a close still pending: the descriptor must go too.
TEST_F(DcClose, W8TheLoopClearedWithAClosePending) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    pump(20ms);
    const bool        open_before    = descriptor_open(handle);
    const std::size_t closing_before = closing_now();
    qb::io::async::listener::current.clear();
    const bool        open_after    = descriptor_open(handle);
    const std::size_t closing_after = closing_now();
    std::printf("DC-W W8 before clear(): descriptor open=%d closing=%zu; after: descriptor open=%d closing=%zu; transport open=%d\n",
                open_before ? 1 : 0, closing_before, open_after ? 1 : 0, closing_after, client.transport().is_open() ? 1 : 0);
    EXPECT_FALSE(open_after && !client.transport().is_open()) << "a descriptor nobody owns any more survived the loop";
    EXPECT_EQ(closing_after, 0u);
}

// W9 -- set_linger(zero): the close is immediate, as the server sessions' knob says.
TEST_F(DcClose, W9ALingerOfZeroClosesAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(qb::duration::zero());
    client.disconnect();
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
    const bool open_after = descriptor_open(handle);
    const Read got        = read_all(pair.peer, 300ms);
    std::printf("DC-W W9 linger 0: dispose at %.1f ms, descriptor open right after=%d closing=%zu; the peer then reads %s\n", ms_since(start),
                open_after ? 1 : 0, closing_now(), got.end);
    EXPECT_FALSE(open_after) << "a zero linger must close in the dispose pass";
    EXPECT_EQ(closing_now(), 0u);
}

// W6 -- many closes in a row leave nothing behind (run last: the fixture prints the descriptor census of every case).
TEST_F(DcClose, W6AHundredClosesLeaveNoDescriptorBehind) {
    const int before = open_descriptors();
    for (int i = 0; i < 100; ++i) {
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.publish(LAST);
        client.disconnect();
        ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
        const Read got = read_all(pair.peer, 200ms);
        ASSERT_STREQ(got.end, "EOF");
        pair.peer.close();
        pump(2ms);
    }
    pump(50ms);
    const int after = open_descriptors();
    std::printf("DC-W W6 descriptors before=%d after=%d closing=%zu\n", before, after, closing_now());
    if (before >= 0) {
        EXPECT_EQ(after, before) << "a descriptor leaked";
    }
    EXPECT_EQ(closing_now(), 0u);
}
