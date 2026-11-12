/**
 * SCRATCH, NEVER COMMITTED -- the challenger's witnesses against the close-semantics dossier (train 7, QB-1270 item 3).
 * One binary, run unchanged against the base (qb 50ed9117) and against prototype B. Every case prints a `CH-W` line.
 *
 *   C1a  TLS: the exact hand-over sequence of prototype B's end_transport() leaves the SSL object bound to the number
 *   C1b  TLS: a write on the transport of a client that ended its own connection, after the number was reused
 *   C2   the number closed by the loop and taken by the object's next connection, 300 times, per backend
 *   C3   many closes against a peer that never closes: descriptors and closing nodes (Linux, a forked peer)
 *   C4a  a dispose on a thread whose loop is not run again
 *   C4b  a thread that ends with a close pending
 *   C5   a blocking async::run() after the last connection ended
 *   C7   what on(event::disconnected) and on(event::dispose) see of the transport
 */
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
#define CH_HAS_CLOSER 1
#endif

#ifdef QB_HAS_SSL
#include <openssl/ssl.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>
#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_pump.h"
#endif

#if defined(__linux__)
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace ch_close_witness_test {

constexpr std::string_view LAST = "LAST-BYTES-GOAWAY";

double
ms_since(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

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
    return n - 3;
#else
    return -1;
#endif
}

std::size_t
closing_now() {
#ifdef CH_HAS_CLOSER
    return qb::io::async::detail::closing_sockets();
#else
    return 0;
#endif
}

char const *
tree() {
#ifdef CH_HAS_CLOSER
    return "B";
#else
    return "base";
#endif
}

char const *
backend() {
    return qb::io::async::listener::backend_name(qb::io::async::listener::current.backend());
}

void
pump(std::chrono::milliseconds how_long) {
    const auto until = std::chrono::steady_clock::now() + how_long;
    while (std::chrono::steady_clock::now() < until) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
}

class Client : public qb::io::use<Client>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<Client>;

    std::size_t disconnected_events = 0;
    std::size_t dispose_events      = 0;
    std::size_t messages            = 0;
    std::string seen_in_disconnected;
    std::string seen_in_dispose;

    void
    on(Protocol::message &&) {
        ++messages;
    }
    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
        seen_in_disconnected = describe();
    }
    void
    on(qb::io::async::event::dispose &&) {
        ++dispose_events;
        seen_in_dispose = describe();
    }

    [[nodiscard]] std::string
    describe() {
        char line[160];
        std::snprintf(line, sizeof(line), "open=%d handle=%lld peer_port=%u local_port=%u", this->transport().is_open() ? 1 : 0,
                      static_cast<long long>(this->transport().native_handle()),
                      static_cast<unsigned>(this->transport().peer_endpoint().port()),
                      static_cast<unsigned>(this->transport().local_endpoint().port()));
        return line;
    }

    void
    reconnect_over(qb::io::tcp::socket &&next) {
        this->reset_for_reconnect();
        this->template switch_protocol<Protocol>(*this);
        this->transport() = std::move(next);
        this->start();
    }

    /// The modules' shape (qbm-redis, qbm-pgsql): the teardown completes in the call.
    void
    drop_now() {
        this->disconnect_now();
    }
};

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

/// What `peer` reads without blocking within `budget`; the loop is pumped only when `pump_loop`.
std::string
read_some(qb::io::tcp::socket &peer, std::chrono::milliseconds budget, bool pump_loop, char const **end = nullptr) {
    peer.set_nonblocking(true);
    std::string            out;
    const auto             deadline = std::chrono::steady_clock::now() + budget;
    std::array<char, 4096> buffer{};
    if (end)
        *end = "open";
    do {
        if (pump_loop)
            qb::io::async::run(EVRUN_NOWAIT);
        const int n = peer.read(buffer.data(), buffer.size());
        if (n > 0) {
            out.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        const int error = qb::io::socket::get_last_errno();
        if (error == 0) {
            if (end)
                *end = "EOF";
            return out;
        }
        if (!qb::io::socket::not_recv_error(error)) {
            if (end)
                *end = "RESET";
            return out;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return out;
}

class ChClose : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

#ifdef QB_HAS_SSL
class TlsClient : public qb::io::use<TlsClient>::tcp::ssl::client<> {
public:
    using Protocol                  = qb::protocol::text::command<TlsClient>;
    std::size_t disconnected_events = 0;
    void
    on(Protocol::message &&) {}
    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
    }
};

SSL_CTX *
server_context() {
    return qb::io::ssl::create_server_context(TLS_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                              qb::io::test::ssl_resource_path("key.pem"));
}

/// A plain TCP pair whose connecting side gets the descriptor number `wanted` (Linux hands out the lowest free number).
struct Victim {
    qb::io::tcp::listener            listener;
    qb::io::tcp::socket              connected;
    qb::io::tcp::socket              accepted;
    std::vector<qb::io::tcp::socket> spare;
    bool                             got = false;
};

/// Before the number is released: the listener must not be the one that takes it.
void
listen_victim(Victim &victim) {
    ASSERT_EQ(victim.listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
}

void
open_victim_on(Victim &victim, ::socket_type wanted) {
    const auto port = victim.listener.local_endpoint().port();
    for (int i = 0; i < 64 && !victim.got; ++i) {
        qb::io::tcp::socket s;
        ASSERT_EQ(s.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        qb::io::tcp::socket a;
        ASSERT_EQ(victim.listener.accept(a), qb::io::SocketStatus::Done);
        if (s.native_handle() == wanted) {
            victim.connected = std::move(s);
            victim.accepted  = std::move(a);
            victim.got       = true;
        } else {
            victim.spare.push_back(std::move(s));
            victim.spare.push_back(std::move(a));
        }
    }
}
#endif

} // namespace ch_close_witness_test

using namespace ch_close_witness_test;

#ifdef QB_HAS_SSL
// C1a -- the mechanism, at the socket: what prototype B's end_transport() does to an ssl::socket, call for call
// (release_handle(); disconnect(); close();), then what its closer does at the end (shutdown, close). The SSL object is
// still in the socket, its BIO still names the number. Any later write() on that socket is an SSL_write() on the number.
TEST_F(ChClose, C1aTheHandOverSequenceLeavesTheSslObjectBoundToTheNumber) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    qb::io::tcp::ssl::listener listener;
    listener.init(server_context());
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();

    qb::io::test::tls_pair pair;
    pair.client.set_insecure();
    ASSERT_TRUE(qb::io::test::open_tls_connection(listener, port, pair));
    ASSERT_TRUE(qb::io::test::complete_tls_handshake(pair));
    ASSERT_TRUE(qb::io::test::tls_round_trip(pair, "ping", "pong"));
    Victim victim;
    ASSERT_NO_FATAL_FAILURE(listen_victim(victim));

    // prototype B, teardown.h: release_handle(), disconnect(), close(), close_after_peer(released, linger)
    const auto released = pair.client.release_handle();
    static_cast<void>(pair.client.disconnect());
    pair.client.close();
    const int rfd_after = SSL_get_rfd(pair.client.ssl_handle());
    const int wfd_after = SSL_get_wfd(pair.client.ssl_handle());
    // ...and the closer's end: the half-close, the peer closes, the descriptor is closed.
    static_cast<void>(::shutdown(released, QB_SD_SEND));
    pair.server.close();
    static_cast<void>(::QB_CLOSESOCKET(released));

    ASSERT_NO_FATAL_FAILURE(open_victim_on(victim, released));
    const bool        handshake_complete_before = pair.client.handshake_complete();
    const int         wrote                     = pair.client.write("STRAY", 5);
    const bool        handshake_complete_after  = pair.client.handshake_complete();
    char const       *end                       = nullptr;
    const std::string got                       = victim.got ? read_some(victim.accepted, 200ms, false, &end) : std::string{};
    std::printf("CH-W C1a [%s] released=%lld; the SSL object after the hand-over: rfd=%d wfd=%d; handshake_complete before the stray "
                "write=%d after=%d; the number reused by an unrelated socket=%d; write() returned %d; the unrelated peer received %zu "
                "octet(s)%s\n",
                tree(), static_cast<long long>(released), rfd_after, wfd_after, handshake_complete_before ? 1 : 0,
                handshake_complete_after ? 1 : 0, victim.got ? 1 : 0, wrote, got.size(),
                got.size() >= 3 && static_cast<unsigned char>(got[0]) == 0x17 ? " (a TLS application-data record)" : "");
    // Does freeing that SSL object later (the next transport's move-assignment, the destructor) close the number?
    pair.client                           = qb::io::tcp::ssl::socket{}; // release_ssl_(): SSL_free
    const bool victim_open_after_ssl_free = victim.got && descriptor_open(released);
    std::printf("CH-W C1a [%s] after SSL_free of the stale object: the unrelated socket on the number is still open=%d (BIO_NOCLOSE)\n", tree(),
                victim_open_after_ssl_free ? 1 : 0);
    EXPECT_TRUE(!victim.got || victim_open_after_ssl_free) << "SSL_free closed a descriptor it does not own";
    EXPECT_NE(rfd_after, static_cast<int>(released)) << "the SSL object still names the descriptor the transport gave away";
    EXPECT_LE(wrote, 0) << "a write on a transport that owns no descriptor reached a descriptor";
    EXPECT_EQ(got.size(), 0u) << "a TLS record of the ended connection was written into an unrelated connection";
}

// C1b -- the same through the io object, tree against tree: a client that ended its own connection, its peer closed,
// an unrelated socket took the number, and the application writes once on the transport it still holds.
TEST_F(ChClose, C1bAWriteOnTheTransportAfterAnOwnEndedDispose) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    qb::io::tcp::ssl::listener listener;
    listener.init(server_context());
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();

    qb::io::test::tls_pair pair;
    pair.client.set_insecure();
    ASSERT_TRUE(qb::io::test::open_tls_connection(listener, port, pair));
    ASSERT_TRUE(qb::io::test::complete_tls_handshake(pair));

    Victim victim;
    ASSERT_NO_FATAL_FAILURE(listen_victim(victim));
    TlsClient client;
    client.transport() = std::move(pair.client);
    const auto handle  = client.transport().native_handle();
    client.start();
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    pair.server.close();
    pump(50ms); // B: the closer reads the peer's EOF and closes the number
    const bool open_after = descriptor_open(handle);

    ASSERT_NO_FATAL_FAILURE(open_victim_on(victim, handle));
    const int         wrote = client.transport().write("STRAY", 5);
    char const       *end   = nullptr;
    const std::string got   = victim.got ? read_some(victim.accepted, 200ms, false, &end) : std::string{};
    std::printf("CH-W C1b [%s] handle=%lld; after the dispose and the peer's close: transport open=%d, descriptor open=%d closing=%zu; the "
                "number reused by an unrelated socket=%d; transport().write() returned %d; the unrelated peer received %zu octet(s)%s\n",
                tree(), static_cast<long long>(handle), client.transport().is_open() ? 1 : 0, open_after ? 1 : 0, closing_now(),
                victim.got ? 1 : 0, wrote, got.size(),
                got.size() >= 3 && static_cast<unsigned char>(got[0]) == 0x17 ? " (a TLS application-data record)" : "");
    EXPECT_EQ(got.size(), 0u) << "a TLS record of the ended connection was written into an unrelated connection";
}
#endif

// C2 -- the loop closes the number, the object's next connection takes it at once: does its watcher see the new socket?
TEST_F(ChClose, C2TheNumberReusedByTheNextConnection) {
    Client                client;
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port   = listener.local_endpoint().port();
    int        reused = 0, rounds = 0;
    long long  previous = -1;
    const int  kRounds  = 300;
    for (int i = 0; i < kRounds; ++i) {
        qb::io::tcp::socket next;
        ASSERT_EQ(next.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done) << "round " << i;
        qb::io::tcp::socket peer;
        ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
        const long long now_handle = static_cast<long long>(next.native_handle());
        if (now_handle == previous)
            ++reused;
        previous = now_handle;
        if (i == 0) {
            client.transport() = std::move(next);
            client.start();
        } else {
            client.reconnect_over(std::move(next));
        }
        ASSERT_EQ(peer.write("pong\n", 5), 5);
        ASSERT_TRUE(qb::io::test::pump_until([&] { return client.messages == static_cast<std::size_t>(i + 1); }, 2000ms))
            << "round " << i << " (handle " << now_handle << ", reused=" << reused << "): the connection on the reused number reads nothing";
        client.disconnect();
        ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == static_cast<std::size_t>(i + 1); }));
        peer.close();
        // B: let the loop close the old number, then connect again WITHOUT another pass in between.
        ASSERT_TRUE(qb::io::test::pump_until([&] { return closing_now() == 0u; }, 1000ms)) << "round " << i;
        ++rounds;
    }
    std::printf("CH-W C2 [%s] backend=%s rounds=%d of %d, the next connection took the number just closed %d time(s)\n", tree(), backend(),
                rounds, kRounds, reused);
    EXPECT_EQ(rounds, kRounds);
}

#if defined(__linux__)
// C3 -- the bounds. A peer process accepts and holds: it never reads, never closes. We connect, say our last bytes,
// disconnect and destroy the object, as fast as we can, under a descriptor limit of 256.
TEST_F(ChClose, C3ManyClosesAgainstAPeerThatNeverCloses) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ::fflush(nullptr);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        struct rlimit lim{};
        ::getrlimit(RLIMIT_NOFILE, &lim);
        lim.rlim_cur = lim.rlim_max;
        ::setrlimit(RLIMIT_NOFILE, &lim);
        const int lfd = static_cast<int>(listener.native_handle());
        for (;;) {
            const int a = ::accept(lfd, nullptr, nullptr);
            if (a < 0)
                ::usleep(1000);
        }
    }
    static_cast<void>(::close(static_cast<int>(listener.release_handle()))); // never shutdown(): the child shares the socket
    struct rlimit saved{};
    ::getrlimit(RLIMIT_NOFILE, &saved);
    struct rlimit low = saved;
    low.rlim_cur      = 256;
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &low), 0);

    const int   before = open_descriptors();
    const auto  start  = std::chrono::steady_clock::now();
    int         done = 0, first_failure = -1, failures = 0, first_errno = 0, max_fds = before;
    std::size_t max_closing      = 0;
    double      first_failure_ms = -1;
    const int   kCloses          = 600;
    for (int i = 0; i < kCloses; ++i) {
        auto client = std::make_unique<Client>();
        if (client->transport().connect_v4("127.0.0.1", port) != qb::io::SocketStatus::Done) {
            if (first_failure < 0) {
                first_failure    = i;
                first_errno      = errno;
                first_failure_ms = ms_since(start);
            }
            ++failures;
            continue;
        }
        client->start();
        client->publish(LAST);
        qb::io::async::run(EVRUN_NOWAIT);
        client->disconnect();
        while (client->disconnected_events == 0u)
            qb::io::async::run(EVRUN_NOWAIT);
        client.reset();
        ++done;
        max_fds     = std::max(max_fds, open_descriptors());
        max_closing = std::max(max_closing, closing_now());
    }
    const double loop_ms = ms_since(start);
    std::printf("CH-W C3 [%s] limit=256: %d of %d connect+close done in %.0f ms; first connect failure at #%d (errno %d, %.0f ms), %d "
                "failure(s); descriptors before=%d max=%d; closing nodes max=%zu\n",
                tree(), done, kCloses, loop_ms, first_failure, first_errno, first_failure_ms, failures, before, max_fds, max_closing);
    // how long until the process can open a descriptor again
    double recovered_ms = -1;
    while (ms_since(start) < loop_ms + 4000.) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (closing_now() == 0u) {
            recovered_ms = ms_since(start);
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::printf("CH-W C3 [%s] every closing node gone at %.0f ms; descriptors now=%d\n", tree(), recovered_ms, open_descriptors());
    ::setrlimit(RLIMIT_NOFILE, &saved);
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    EXPECT_EQ(failures, 0) << "the process ran out of descriptors: the closes it had asked for were still holding them";
}
#endif

// C4a -- the teardown completes in the call (disconnect_now(), the modules' shape), the object is destroyed, and this
// thread's loop is not run again. Who closes the descriptor, and when?
TEST_F(ChClose, C4aADisposeOnAThreadWhoseLoopIsNotRunAgain) {
    auto client = std::make_unique<Client>();
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(*client, pair));
    const auto handle = client->transport().native_handle();
    client->drop_now();
    ASSERT_EQ(client->disconnected_events, 1u);
    client.reset();
    const bool  open_after_destruction = descriptor_open(handle);
    char const *end                    = nullptr;
    static_cast<void>(read_some(pair.peer, 200ms, false, &end));
    pair.peer.close();
    std::this_thread::sleep_for(2500ms); // longer than linger(); no loop pass
    const bool        open_after_bound = descriptor_open(handle);
    const std::size_t closing_then     = closing_now();
    qb::io::async::run(EVRUN_NOWAIT);
    qb::io::async::run(EVRUN_NOWAIT);
    const bool open_after_a_pass = descriptor_open(handle);
    std::printf("CH-W C4a [%s] the peer read %s; descriptor open after the object's destruction=%d; 2.5 s later, the peer closed, no loop "
                "pass: open=%d closing=%zu; after one loop pass: open=%d closing=%zu\n",
                tree(), end, open_after_destruction ? 1 : 0, open_after_bound ? 1 : 0, closing_then, open_after_a_pass ? 1 : 0, closing_now());
    EXPECT_FALSE(open_after_bound) << "a descriptor nobody owns outlived linger(): the loop that holds it is not running";
}

// C4b -- a thread makes a connection, ends it, and returns with the close pending.
TEST_F(ChClose, C4bAThreadThatEndsWithAClosePending) {
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    const int              before = open_descriptors();
    std::atomic<long long> handle{-1};
    std::atomic<int>       closing_in_thread{-1};
    std::thread            worker([&] {
        qb::io::async::init();
        Client client;
        if (client.transport().connect_v4("127.0.0.1", pair.port) != qb::io::SocketStatus::Done)
            return;
        handle = static_cast<long long>(client.transport().native_handle());
        client.start();
        client.disconnect();
        while (client.disconnected_events == 0u)
            qb::io::async::run(EVRUN_NOWAIT);
        closing_in_thread = static_cast<int>(closing_now());
    });
    ASSERT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    worker.join();
    const bool  open_after_join = handle >= 0 && descriptor_open(static_cast<::socket_type>(handle.load()));
    char const *end             = nullptr;
    static_cast<void>(read_some(pair.peer, 300ms, false, &end));
    const int after = open_descriptors();
    std::printf("CH-W C4b [%s] closing in the thread at its end=%d; after join: descriptor open=%d; the peer reads %s; descriptors before=%d "
                "after=%d (the accepted peer counts for 1)\n",
                tree(), closing_in_thread.load(), open_after_join ? 1 : 0, end, before, after);
    EXPECT_FALSE(open_after_join);
}

// C5 -- the documented standalone main: work, end the connection, block in run() until the loop is empty.
TEST_F(ChClose, C5ABlockingRunAfterTheLastConnectionEnded) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    client.publish(LAST);
    qb::io::async::run(EVRUN_NOWAIT);
    client.disconnect();
    const auto start = std::chrono::steady_clock::now();
    qb::io::async::run(); // blocks while a referenced watcher is active; the peer is silent
    const double returned_ms = ms_since(start);
    std::printf("CH-W C5 [%s] a blocking async::run() after disconnect(), the peer silent: returned after %.0f ms (disconnected=%zu, "
                "closing=%zu)\n",
                tree(), returned_ms, client.disconnected_events, closing_now());
    EXPECT_LE(returned_ms, 500.) << "the program's exit waits for a peer that does not close";
}

// C7 -- what the two hooks see of the transport, own-ended and peer-ended.
TEST_F(ChClose, C7WhatTheHooksSeeOfTheTransport) {
    {
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.disconnect();
        ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
        std::printf("CH-W C7 [%s] own-ended : in on(disconnected) %s | in on(dispose) %s | after %s\n", tree(),
                    client.seen_in_disconnected.c_str(), client.seen_in_dispose.c_str(), client.describe().c_str());
    }
    {
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        pair.peer.close();
        ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
        std::printf("CH-W C7 [%s] peer-ended: in on(disconnected) %s | in on(dispose) %s | after %s\n", tree(),
                    client.seen_in_disconnected.c_str(), client.seen_in_dispose.c_str(), client.describe().c_str());
    }
}

// C8 -- a property of the loop, not of the prototype (a control for C9): a socket closed BETWEEN two passes -- what a
// timer callback, another watcher's callback or a destructor does -- while the backend still has a poll on it, and
// its number taken at once by a new connection that starts reading before the next pass. Does the new one hear?
TEST_F(ChClose, C8ANumberClosedBetweenPassesAndReusedAtOnce) {
    Client old_client;
    Pair   old_pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(old_client, old_pair));
    pump(20ms); // the backend's poll on the old socket is pending
    const auto            handle = old_client.transport().native_handle();
    qb::io::tcp::listener listener; // before the close: it must not be the one that takes the number
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    old_client.stop();
    old_client.transport().close();

    std::vector<qb::io::tcp::socket> spare;
    qb::io::tcp::socket              next, peer;
    bool                             got = false;
    for (int i = 0; i < 256 && !got; ++i) {
        qb::io::tcp::socket s;
        ASSERT_EQ(s.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        qb::io::tcp::socket a;
        ASSERT_EQ(listener.accept(a), qb::io::SocketStatus::Done);
        if (s.native_handle() == handle) {
            next = std::move(s);
            peer = std::move(a);
            got  = true;
        } else {
            spare.push_back(std::move(s));
            spare.push_back(std::move(a));
        }
    }
    Client fresh;
    bool   heard = false;
    if (got) {
        fresh.transport() = std::move(next);
        fresh.start();
        ASSERT_EQ(peer.write("pong\n", 5), 5);
        heard = qb::io::test::pump_until([&] { return fresh.messages == 1u; }, 2000ms);
    }
    std::printf("CH-W C8 [%s] backend=%s: the number %lld closed between two passes was taken by a new connection=%d; the new connection "
                "heard its peer within 2 s=%d\n",
                tree(), backend(), static_cast<long long>(handle), got ? 1 : 0, heard ? 1 : 0);
    EXPECT_TRUE(!got || heard) << "the new connection's watcher never fires: its registration was lost with the old socket's";
}

#ifdef CH_HAS_CLOSER
// C9 -- prototype B: the BOUND ends a close (its timer's callback closes the socket), and in the same pass a later
// callback opens the object's next connection, which takes the number.
TEST_F(ChClose, C9TheBoundEndsACloseAndTheNumberIsReusedInTheSamePass) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(40ms);
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
    const std::size_t closing_before = closing_now(); // the peer is silent: a node holds the socket for 40 ms

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto                       port = listener.local_endpoint().port();
    std::vector<qb::io::tcp::socket> spare;
    qb::io::tcp::socket              peer;
    bool                             got = false, ran = false;
    std::size_t                      closing_in_callback = 99;
    // fires after the closer's timer, in the same pass (the loop is not run for 80 ms, then once)
    qb::io::async::callback(
        [&] {
            ran                 = true;
            closing_in_callback = closing_now();
            for (int i = 0; i < 256 && !got; ++i) {
                qb::io::tcp::socket s;
                if (s.connect_v4("127.0.0.1", port) != qb::io::SocketStatus::Done)
                    return;
                qb::io::tcp::socket a;
                if (listener.accept(a) != qb::io::SocketStatus::Done)
                    return;
                if (s.native_handle() == handle) {
                    client.reconnect_over(std::move(s));
                    peer = std::move(a);
                    got  = true;
                } else {
                    spare.push_back(std::move(s));
                    spare.push_back(std::move(a));
                }
            }
        },
        60ms);
    std::this_thread::sleep_for(80ms);
    qb::io::async::run(EVRUN_NOWAIT); // both timers are due: the closer's (40 ms) first, then the callback (60 ms)
    bool heard = false;
    if (got) {
        const int wrote = peer.write("pong\n", 5);
        EXPECT_EQ(wrote, 5);
        heard = qb::io::test::pump_until([&] { return client.messages == 1u; }, 2000ms);
    }
    std::printf("CH-W C9 [%s] backend=%s: closing before=%zu, in the callback=%zu (0: the bound had closed the socket in the same pass); "
                "callback ran=%d; the next connection took the number %lld=%d; it heard its peer within 2 s=%d\n",
                tree(), backend(), closing_before, closing_in_callback, ran ? 1 : 0, static_cast<long long>(handle), got ? 1 : 0,
                heard ? 1 : 0);
    EXPECT_TRUE(!got || heard) << "the object's next connection never reads: its registration was lost with the closed socket's";
}
#endif

#ifdef QB_HAS_SSL
// C1c -- the control for C1b: the same stray write after a PEER-ended dispose, which closes the transport outright on
// both trees (teardown.h:101-107). Is the SSL object bound to the closed number there too?
TEST_F(ChClose, C1cTheSameWriteAfterAPeerEndedDispose) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    qb::io::tcp::ssl::listener listener;
    listener.init(server_context());
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();

    qb::io::test::tls_pair pair;
    pair.client.set_insecure();
    ASSERT_TRUE(qb::io::test::open_tls_connection(listener, port, pair));
    ASSERT_TRUE(qb::io::test::complete_tls_handshake(pair));

    Victim victim;
    ASSERT_NO_FATAL_FAILURE(listen_victim(victim));
    TlsClient client;
    client.transport() = std::move(pair.client);
    const auto handle  = client.transport().native_handle();
    client.start();
    pair.server.close(); // the peer ends it
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    const bool open_after = descriptor_open(handle);

    ASSERT_NO_FATAL_FAILURE(open_victim_on(victim, handle));
    const int         wrote = client.transport().write("STRAY", 5);
    char const       *end   = nullptr;
    const std::string got   = victim.got ? read_some(victim.accepted, 200ms, false, &end) : std::string{};
    std::printf("CH-W C1c [%s] handle=%lld; after the PEER-ended dispose: transport open=%d, descriptor open=%d; the number reused by an "
                "unrelated socket=%d; transport().write() returned %d; the unrelated peer received %zu octet(s)%s\n",
                tree(), static_cast<long long>(handle), client.transport().is_open() ? 1 : 0, open_after ? 1 : 0, victim.got ? 1 : 0, wrote,
                got.size(), got.size() >= 3 && static_cast<unsigned char>(got[0]) == 0x17 ? " (a TLS application-data record)" : "");
    EXPECT_EQ(got.size(), 0u) << "a TLS record of the ended connection was written into an unrelated connection";
}
#endif
