/**
 * @file qb/tests/io/system/tcp/close-after-peer-posix.cpp
 * @brief The loop's close after the peer at the process's descriptor limit, and over AF_UNIX stream sockets
 *        (Huly QB-1270) -- the cases that need POSIX: fork(), setrlimit(RLIMIT_NOFILE), unix-domain sockets.
 *
 * A connection this side ended leaves its object for the thread's loop, which closes the descriptor after the peer
 * (`detail::close_after_peer`): on the peer's EOF, a socket error, `linger()` or 16 MiB dropped. Those descriptors
 * belong to nobody else, so two things keep them from costing the process the ones it needs:
 *
 *   - a cap: a loop closes at most `closing_sockets_cap()` sockets after their peer at the same time, and one more
 *     closes the oldest at once. Under a descriptor limit of 256, against a peer that never closes, 600 closes in a
 *     row all succeed, the loop never holding more than the cap (without it the 250th connect failed with EMFILE);
 *   - the descriptors themselves: when the system refuses a socket (`tcp::socket::init()`, so every connect) or an
 *     accepted connection (`tcp::listener::accept()`, so every acceptor) for lack of descriptors, the calling
 *     thread's closing sockets are closed at once, oldest first, and the call is made again. A connect at the limit
 *     succeeds, and an acceptor at the limit accepts the connection waiting in its backlog at once -- not when the
 *     closes' bound has elapsed, which is when it would get a descriptor otherwise.
 *
 * And the close is the same over a unix-domain stream socket (qbm-redis and qbm-pgsql connect over them): the peer
 * reads what was sent, then EOF; what it still writes is read and dropped; the descriptor closes when the peer
 * closes, or at `linger()` against a silent one.
 *
 * The descriptor limit is restored, every descriptor taken to reach it closed, and every child reaped, by guards that
 * run however a case ends.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/close_after_peer_support.h"
#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace close_after_peer_posix_test {

using namespace qb::io::test::close_after_peer;

/// The descriptors this process has open, by number (`/dev/fd`: Linux and macOS), the listing's own left out.
std::vector<int>
open_descriptor_numbers() {
    std::vector<int> numbers;
    DIR             *dir = ::opendir("/dev/fd");
    if (!dir)
        return numbers;
    const int own = ::dirfd(dir);
    while (const dirent *entry = ::readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        const int fd = std::atoi(entry->d_name);
        if (fd != own)
            numbers.push_back(fd);
    }
    ::closedir(dir);
    return numbers;
}

int
highest_descriptor() {
    const auto numbers = open_descriptor_numbers();
    return numbers.empty() ? -1 : *std::max_element(numbers.begin(), numbers.end());
}

class AcceptServer;

/// The session of the async acceptor below: it only has to exist.
class AcceptSession : public qb::io::use<AcceptSession>::tcp::client<AcceptServer> {
public:
    using Protocol = qb::protocol::text::command<AcceptSession>;

    explicit AcceptSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&) {}
};

class AcceptServer : public qb::io::use<AcceptServer>::tcp::server<AcceptSession> {};

/// Sets this process's soft descriptor limit, and puts the previous one back at its end.
class DescriptorLimit {
    rlimit _saved{};
    bool   _set = false;

public:
    explicit DescriptorLimit(rlim_t soft) {
        if (::getrlimit(RLIMIT_NOFILE, &_saved) != 0)
            return;
        rlimit lowered   = _saved;
        lowered.rlim_cur = std::min(soft, _saved.rlim_max);
        _set             = ::setrlimit(RLIMIT_NOFILE, &lowered) == 0;
    }
    DescriptorLimit(DescriptorLimit const &)            = delete;
    DescriptorLimit &operator=(DescriptorLimit const &) = delete;
    ~DescriptorLimit() {
        if (_set)
            static_cast<void>(::setrlimit(RLIMIT_NOFILE, &_saved));
    }

    [[nodiscard]] bool
    set() const noexcept {
        return _set;
    }
};

/// Takes every descriptor the process has left under its limit (copies of /dev/null), and closes them at its end.
class Hogs {
    std::vector<int> _fds;
    int              _error = 0;

public:
    Hogs() {
        const int first = ::open("/dev/null", O_RDONLY);
        if (first < 0) {
            _error = errno;
            return;
        }
        _fds.push_back(first);
        for (int i = 0; i < 65536; ++i) {
            const int fd = ::dup(first);
            if (fd < 0) {
                _error = errno;
                return;
            }
            _fds.push_back(fd);
        }
    }
    Hogs(Hogs const &)            = delete;
    Hogs &operator=(Hogs const &) = delete;
    ~Hogs() {
        for (const int fd : _fds)
            ::close(fd);
    }

    /// Why the last descriptor could not be had: EMFILE once the table is full.
    [[nodiscard]] int
    error() const noexcept {
        return _error;
    }
};

/// A forked process: killed and reaped at the end, however the case ends.
class Child {
    pid_t _pid;

public:
    explicit Child(pid_t pid) noexcept
        : _pid(pid) {}
    Child(Child const &)            = delete;
    Child &operator=(Child const &) = delete;
    ~Child() {
        if (_pid > 0) {
            static_cast<void>(::kill(_pid, SIGKILL));
            int status = 0;
            static_cast<void>(::waitpid(_pid, &status, 0));
        }
    }
};

/// A unix-domain socket path of this process, removed before and after.
class UnixPath {
public:
    std::string path;

    explicit UnixPath(std::string_view name)
        : path("/tmp/qb-close-after-peer-" + std::to_string(::getpid()) + "-" + std::string(name) + ".sock") {
        static_cast<void>(::unlink(path.c_str()));
    }
    UnixPath(UnixPath const &)            = delete;
    UnixPath &operator=(UnixPath const &) = delete;
    ~UnixPath() {
        static_cast<void>(::unlink(path.c_str()));
    }
};

/// `count` closes the loop holds, each against a peer of `peers` that stays open and silent for `linger`.
void
make_pending_closes(qb::io::tcp::listener &listener, std::vector<qb::io::tcp::socket> &peers, std::size_t count, qb::duration linger) {
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    for (std::size_t i = 0; i < count; ++i) {
        auto client = std::make_unique<Client>();
        client->set_linger(linger);
        ASSERT_EQ(client->transport().connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        peers.emplace_back();
        ASSERT_EQ(listener.accept(peers.back()), qb::io::SocketStatus::Done);
        client->start();
        client->disconnect();
        ASSERT_TRUE(qb::io::test::pump_until([&] { return client->dispose_events == 1u; })) << "close " << i << " never ran";
    } // each client is gone: only the loop holds its descriptor
}

/// A client connected over a unix-domain stream socket to `peer`, its watcher started.
void
connect_unix(Client &client, qb::io::tcp::listener &listener, qb::io::tcp::socket &peer, UnixPath const &path) {
    ASSERT_EQ(listener.listen_un(path.path), qb::io::SocketStatus::Done);
    ASSERT_EQ(client.transport().connect_un(path.path), qb::io::SocketStatus::Done);
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    client.start();
}

class CloseAfterPeerPosix : public ::testing::Test {
protected:
    int descriptors_before = -1;

    void
    SetUp() override {
        // listener::current builds its loop on first use (Huly QB-1246): ask for it. The backend's own descriptors
        // (epoll's on Linux, the wake's on the first run) live as long as the process: the count is taken after them.
        (void) qb::io::async::listener::current.loop();
        qb::io::async::run(EVRUN_NOWAIT);
        expect_the_backend_asked_for(); // QB_EV_BACKEND, when it pins one: a run on another backend proves nothing of it
        descriptors_before = open_descriptors();
    }

    void
    TearDown() override {
        qb::io::async::listener::current.clear();
        EXPECT_EQ(closing(), 0u) << "listener::clear() left a closing socket";
        EXPECT_EQ(open_descriptors(), descriptors_before) << "the case left a descriptor open";
    }
};

} // namespace close_after_peer_posix_test

using namespace close_after_peer_posix_test;

// A peer process accepts and holds: it never reads, never closes. Connect, say our last bytes, disconnect and destroy
// the object, 600 times in a row, under a descriptor limit of 256: every close but the cap's worth is closed at once,
// and nothing fails.
TEST_F(CloseAfterPeerPosix, TheLoopHoldsNoMoreThanItsCapUnderALowDescriptorLimit) {
    constexpr int closes = 600;
    std::uint16_t port   = 0;
    pid_t         pid    = -1;
    {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        port = listener.local_endpoint().port();
        std::fflush(nullptr);
        pid = ::fork();
        ASSERT_GE(pid, 0) << "fork failed";
        if (pid == 0) { // the peer: accepts every connection and keeps it, until it is killed
            rlimit limit{};
            if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
                limit.rlim_cur = std::min<rlim_t>(limit.rlim_max, 4096);
                static_cast<void>(::setrlimit(RLIMIT_NOFILE, &limit));
            }
            const int listening = static_cast<int>(listener.native_handle());
            for (;;)
                if (::accept(listening, nullptr, nullptr) < 0)
                    ::usleep(1000);
        }
        // The child shares the listening socket: this side closes its copy, never shuts it down.
        static_cast<void>(::close(static_cast<int>(listener.release_handle())));
    }
    const Child peer_process(pid);

    std::size_t max_closing = 0;
    int         done = 0, failures = 0, first_failure = -1, first_errno = 0;
    {
        const DescriptorLimit limit(256);
        ASSERT_TRUE(limit.set()) << "could not set a descriptor limit of 256";
        for (int i = 0; i < closes; ++i) {
            auto client = std::make_unique<Client>();
            if (client->transport().connect_v4("127.0.0.1", port) != qb::io::SocketStatus::Done) {
                if (first_failure < 0) {
                    first_failure = i;
                    first_errno   = errno;
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
            max_closing = std::max(max_closing, closing());
        }
    }
    SCOPED_TRACE("first connect failure at #" + std::to_string(first_failure) + " (errno " + std::to_string(first_errno) + ")");
    EXPECT_EQ(failures, 0) << "the process ran out of descriptors: the closes it had asked for were holding them";
    EXPECT_EQ(done, closes);
    EXPECT_LE(max_closing, qb::io::async::detail::closing_sockets_cap()) << "the loop held more closing sockets than its cap";
    EXPECT_EQ(max_closing, qb::io::async::detail::closing_sockets_cap())
        << "the peer never closes: the closes must have reached the cap, or the case proves nothing about it";
}

// At the descriptor limit, with closes pending against silent peers (each bounded at 20 s), a connect needs a
// descriptor: the thread's closing sockets give theirs, and the connect succeeds.
TEST_F(CloseAfterPeerPosix, AConnectAtTheDescriptorLimitTakesTheDescriptorsOfTheClosingSockets) {
    constexpr std::size_t            pending = 4;
    qb::io::tcp::listener            listener;
    std::vector<qb::io::tcp::socket> peers;
    ASSERT_NO_FATAL_FAILURE(make_pending_closes(listener, peers, pending, 20s));
    EXPECT_EQ(closing(), pending) << "the closes did not go to the loop";
    const auto port = listener.local_endpoint().port();

    Client fresh;
    int    connected = -1, error = 0;
    {
        const DescriptorLimit limit(static_cast<rlim_t>(highest_descriptor() + 1 + 8));
        ASSERT_TRUE(limit.set()) << "could not lower the descriptor limit";
        const Hogs hogs;
        ASSERT_EQ(hogs.error(), EMFILE) << "the descriptor table was not filled";
        connected = fresh.transport().connect_v4("127.0.0.1", port);
        error     = errno;
    }
    EXPECT_EQ(connected, qb::io::SocketStatus::Done)
        << "a connect at the descriptor limit failed (errno " << error << "): the closing sockets kept their descriptors";
    EXPECT_EQ(closing(), 0u) << "the closing sockets were not all closed for the connect";
}

// At the descriptor limit, with closes pending against silent peers (each bounded at 20 s), an async acceptor has a
// connection waiting in its backlog. Without a descriptor to accept it into it would retry on every pass until the
// closes' bound gave one back; the closing sockets give theirs at once.
TEST_F(CloseAfterPeerPosix, AnAcceptorAtTheDescriptorLimitTakesTheDescriptorsOfTheClosingSockets) {
    constexpr std::size_t            pending = 4;
    qb::io::tcp::listener            listener;
    std::vector<qb::io::tcp::socket> peers;
    ASSERT_NO_FATAL_FAILURE(make_pending_closes(listener, peers, pending, 20s));
    EXPECT_EQ(closing(), pending) << "the closes did not go to the loop";

    AcceptServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    server.start();
    const auto port = server.transport().local_endpoint().port();
    // A first connection, accepted while descriptors are free: the acceptor is one that has served already. It also
    // keeps UndefinedBehaviorSanitizer quiet below: its first check of a polymorphic type opens a pipe, which a process
    // at its descriptor limit cannot, and it then reports a valid object as one with an invalid vptr.
    qb::io::tcp::socket first;
    ASSERT_EQ(first.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return server.session_count() == 1u; })) << "the first connection was never accepted";
    qb::io::tcp::socket waiting; // connected by the system, in the acceptor's backlog: no loop pass has run since
    ASSERT_EQ(waiting.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);

    int passes = 0;
    {
        const DescriptorLimit limit(static_cast<rlim_t>(highest_descriptor() + 1 + 8));
        ASSERT_TRUE(limit.set()) << "could not lower the descriptor limit";
        const Hogs hogs;
        ASSERT_EQ(hogs.error(), EMFILE) << "the descriptor table was not filled";
        const auto deadline = steady::now() + 2s;
        while (server.session_count() == 1u && steady::now() < deadline) {
            qb::io::async::run(EVRUN_NOWAIT);
            ++passes;
            std::this_thread::sleep_for(1ms);
        }
    }
    EXPECT_EQ(server.session_count(), 2u) << "the acceptor did not accept within 2 s at the descriptor limit (" << passes
                                          << " loop passes): it waits for the closes' bound";
    EXPECT_LE(passes, 10) << "the connection waiting in the backlog took " << passes << " loop passes";
    EXPECT_EQ(closing(), 0u) << "the closing sockets were not all closed for the accept";
}

// The same for the blocking form, accept() with no argument: a connection waiting in the backlog of a plain listener is
// accepted at the descriptor limit once the thread's closing sockets have given their descriptors.
TEST_F(CloseAfterPeerPosix, ABlockingAcceptAtTheDescriptorLimitTakesTheDescriptorsOfTheClosingSockets) {
    constexpr std::size_t            pending = 4;
    qb::io::tcp::listener            listener;
    std::vector<qb::io::tcp::socket> peers;
    ASSERT_NO_FATAL_FAILURE(make_pending_closes(listener, peers, pending, 20s));
    EXPECT_EQ(closing(), pending) << "the closes did not go to the loop";

    qb::io::tcp::socket waiting; // connected by the system, in the listener's backlog
    ASSERT_EQ(waiting.connect_v4("127.0.0.1", listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    qb::io::tcp::socket accepted;
    int                 error = 0;
    {
        const DescriptorLimit limit(static_cast<rlim_t>(highest_descriptor() + 1 + 8));
        ASSERT_TRUE(limit.set()) << "could not lower the descriptor limit";
        const Hogs hogs;
        ASSERT_EQ(hogs.error(), EMFILE) << "the descriptor table was not filled";
        accepted = listener.accept();
        error    = errno;
    }
    EXPECT_TRUE(accepted.is_open()) << "an accept at the descriptor limit failed (errno " << error
                                    << "): the closing sockets kept their descriptors";
    EXPECT_EQ(closing(), 0u) << "the closing sockets were not all closed for the accept";
}

// What "the system has no descriptor left" reads as: the process's limit and the system's, and nothing else.
static_assert(qb::io::async::detail::out_of_descriptors(EMFILE));
static_assert(qb::io::async::detail::out_of_descriptors(ENFILE));
static_assert(!qb::io::async::detail::out_of_descriptors(0));
static_assert(!qb::io::async::detail::out_of_descriptors(EAGAIN));
static_assert(!qb::io::async::detail::out_of_descriptors(ECONNABORTED));
static_assert(!qb::io::async::detail::out_of_descriptors(ENOMEM));

// The peer closes when it reads our EOF: the loop closes at once, not at the bound.
TEST_F(CloseAfterPeerPosix, AnAfUnixPeerThatClosesOnOurEofEndsTheClose) {
    const UnixPath        path("closes");
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   peer;
    Client                client;
    client.set_linger(10s);
    ASSERT_NO_FATAL_FAILURE(connect_unix(client, listener, peer, path));
    const auto handle = client.transport().native_handle();

    client.publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.out().size() == 0u; })) << "our last bytes never left";
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; })) << "the teardown never ran";
    const Read got = read_all(peer, 1000ms);
    EXPECT_EQ(got.received, LAST);
    EXPECT_EQ(got.end, "EOF") << "error " << got.error;

    peer.close();
    const auto closed_at = steady::now();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return !descriptor_open(handle); }, 5000ms))
        << "the peer closed: the descriptor must close at once, not at the bound or never";
    EXPECT_LT(ms_since(closed_at), 5000.);
    EXPECT_EQ(closing(), 0u);
}

// The peer's bytes are unread when we end, it writes again after our EOF, and only then reads: our last bytes, then EOF
// -- what it wrote is read and dropped, nothing is reset. The descriptor closes when the peer closes.
TEST_F(CloseAfterPeerPosix, AnAfUnixPeerWhoseBytesWeLeftUnreadReadsOurLastBytesThenEof) {
    const UnixPath        path("unread");
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   peer;
    Client                client;
    ASSERT_NO_FATAL_FAILURE(connect_unix(client, listener, peer, path));
    const auto handle = client.transport().native_handle();

    client.publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.out().size() == 0u; })) << "our last bytes never left";
    const std::vector<char> unread(4096, 'u');
    ASSERT_EQ(peer.write(unread.data(), unread.size()), 4096); // no loop pass reads it: the disconnect is handled first
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; })) << "the teardown never ran";
    pump(100ms);
    EXPECT_EQ(peer.write("again", 5), 5) << "the peer's write after our EOF failed";
    pump(100ms);
    const Read got = read_all(peer, 1000ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_EQ(got.end, "EOF") << "then a clean EOF, not a reset (error " << got.error << ")";

    peer.close();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return !descriptor_open(handle); })) << "nothing may be left once the peer closed";
    EXPECT_EQ(closing(), 0u);
}

// A peer that neither closes nor writes: on(event::dispose&&) runs at once, and the loop closes the descriptor at
// linger().
TEST_F(CloseAfterPeerPosix, AnAfUnixPeerThatStaysSilentIsClosedAtTheLinger) {
    const UnixPath        path("silent");
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   peer;
    Client                client;
    client.set_linger(300ms);
    ASSERT_NO_FATAL_FAILURE(connect_unix(client, listener, peer, path));
    const auto handle = client.transport().native_handle();

    client.disconnect();
    const auto start = steady::now();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; })) << "the teardown never ran";
    EXPECT_TRUE(descriptor_open(handle)) << "the descriptor closed before its peer did or its linger elapsed";
    double closed_ms = -1;
    while (closed_ms < 0 && ms_since(start) < 5000.) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (!descriptor_open(handle))
            closed_ms = ms_since(start);
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_GE(closed_ms, 250.) << "the descriptor was never closed, or closed before its linger of 300 ms";
    EXPECT_LT(closed_ms, 3000.) << "the descriptor outlived its linger of 300 ms";
    EXPECT_EQ(closing(), 0u);
}
