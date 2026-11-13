/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/static_exit_support.h
 * @brief What the witnesses of an object with STATIC STORAGE that ends at process exit share (Huly QB-1246): a
 *        loopback relay the test steps by hand, the record of a case and the judge that reads it, the places a
 *        program ends from, and the matcher of a child that printed its verdict and nothing else.
 *
 * `exit()` destroys the main thread's thread-locals -- `listener::current` among them -- before the objects with
 * static storage. A client a program keeps in static storage is therefore destroyed after its thread's loop, and
 * everything its destructor does runs where nothing can be registered, armed or resumed any more. The modules'
 * suites (qbm-http, qbm-redis, qbm-pgsql) each run their clients through that end, one CHILD process per case:
 *
 *   - a JUDGE, an `atexit` handler registered BEFORE the object with static storage is built, runs after its
 *     destructor; it reads what the case recorded and how the peer's stream ended, prints ONE line on stderr and
 *     ends the child with `std::_Exit` (never the destructor of a static: one that does not return is MSVC's C4722);
 *   - the peer is a `relay`: a listening socket of the test, the connection it accepted and, when the case has a
 *     real server, a connection to it. Nothing runs it but `step()`, called between two passes of the loop while the
 *     case is set up. A case that stops stepping has a peer that is open and silent -- a request then stays in
 *     flight for as long as the case likes, with no server-side cooperation -- and at exit the judge reads the
 *     accepted end itself: a clean EOF, a reset, or a connection still open.
 *
 * Everything here runs on the test's thread; the relay uses no loop and no thread.
 */

#ifndef QB_IO_TEST_STATIC_EXIT_SUPPORT_H
#define QB_IO_TEST_STATIC_EXIT_SUPPORT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

// LeakSanitizer comes with AddressSanitizer where it exists (not on Windows): the judge asks it, because a child
// that ends with `_Exit` never reaches the check a process makes at its normal end.
#if !defined(_WIN32)
#if defined(__SANITIZE_ADDRESS__)
#define QB_STATIC_EXIT_LSAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QB_STATIC_EXIT_LSAN 1
#endif
#endif
#endif
#ifdef QB_STATIC_EXIT_LSAN
#include <sanitizer/lsan_interface.h>
#endif

namespace qb::io::test::static_exit {

using steady = std::chrono::steady_clock;

/// The thread's `listener::current` is destroyed (read on the thread's record, never on the listener).
[[nodiscard]] inline bool
loop_gone() noexcept {
    return qb::io::async::detail::thread_loop_gone();
}

/// The end of a child: its verdict is its exit code.
[[noreturn]] inline void
end_child(bool const ok) noexcept {
    std::fflush(stderr);
    std::_Exit(ok ? 0 : 1);
}

/// A precondition of the case did not hold: its verdict would mean nothing.
[[noreturn]] inline void
setup_failed(char const *const what) noexcept {
    std::fprintf(stderr, "setup failed: %s\n", what);
    std::fflush(stderr);
    std::_Exit(2);
}

/// 1 when LeakSanitizer reports a leak now (it prints its report on stderr), else 0 -- also where there is none.
[[nodiscard]] inline int
leaks_now() noexcept {
#ifdef QB_STATIC_EXIT_LSAN
    return __lsan_do_recoverable_leak_check() != 0 ? 1 : 0;
#else
    return 0;
#endif
}

/// Sets its flag when it is destroyed. Declared BEFORE a member, it is destroyed after it: the member's destructor
/// returned.
struct destroyed_flag {
    bool &flag;
    explicit destroyed_flag(bool &f) noexcept
        : flag(f) {}
    destroyed_flag(destroyed_flag const &)            = delete;
    destroyed_flag &operator=(destroyed_flag const &) = delete;
    ~destroyed_flag() {
        flag = true;
    }
};

/// How the stream a raw socket reads ends within `budget`: 0 a clean EOF, 1 a reset, 2 still open.
[[nodiscard]] inline int
stream_end(::socket_type const peer, std::chrono::milliseconds const budget = std::chrono::milliseconds(2000)) {
    static_cast<void>(qb::io::socket::set_nonblocking(peer, true));
    char       chunk[4096];
    const auto deadline = steady::now() + budget;
    while (steady::now() < deadline) {
        const int n = qb::io::socket::recv(peer, chunk, static_cast<int>(sizeof(chunk)));
        if (n > 0)
            continue;
        if (n == 0)
            return 0;
        if (!qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 2;
}

/**
 * @brief The peer of a case: what a client connects to, stepped by hand.
 * @details `listen()` binds a loopback port. `step()` accepts the client when one is waiting and none is held, then
 *          moves what is readable: with `through()`, between the client and a real server (a daemon, or a server of
 *          the test on a thread of its own); with `answer()`, into a script that says what to write back; with
 *          neither, nowhere -- the bytes are read and counted. It never waits for a peer: its sockets are read and
 *          written without blocking, and the one blocking call is the connection to the server of `through()`, made
 *          on the loopback when a client is accepted. The relay of a child (`new_peer()`) is built on the heap and
 *          never destroyed: `exit()` runs no destructor of it, so the judge reads the accepted end after the
 *          statics. A case that runs in the test's own process keeps its relay on its stack.
 */
class relay {
public:
    /// What a scripted peer writes back: called with everything read from the client and not yet consumed.
    using script = std::function<std::string(std::string &unread)>;

    relay()                         = default;
    relay(relay const &)            = delete;
    relay &operator=(relay const &) = delete;

    [[nodiscard]] bool
    listen() {
        if (_listener.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
            return false;
        _port = _listener.local_endpoint().port();
        static_cast<void>(qb::io::socket::set_nonblocking(_listener.native_handle(), true));
        return _port != 0;
    }

    [[nodiscard]] std::uint16_t
    port() const noexcept {
        return _port;
    }

    /// Everything the client sends goes to `host:port`, and back.
    void
    through(std::string host, std::uint16_t const port) {
        _upstream_host = std::move(host);
        _upstream_port = port;
    }

    /// Everything the client sends goes to `answer`, whose return is written back.
    void
    answer(script answer) {
        _script = std::move(answer);
    }

    /// One turn: accept, then move what is readable. Does nothing once the relay is silent.
    void
    step() {
        if (_silent)
            return;
        if (!_client.is_open() && _listener.is_open() && !_holding) {
            qb::io::tcp::socket accepted;
            if (_listener.accept(accepted) == qb::io::SocketStatus::Done && accepted.is_open()) {
                _client = std::move(accepted);
                static_cast<void>(_client.set_nonblocking(true));
                ++_accepted;
                _client_end = -1;
                _to_client.clear();
                _to_server.clear();
                _unread.clear();
                if (_upstream_port != 0) {
                    _server.close();
                    if (_server.connect_v4(_upstream_host, _upstream_port) != qb::io::SocketStatus::Done)
                        _server.close();
                    else
                        static_cast<void>(_server.set_nonblocking(true));
                }
            }
        }
        if (!_client.is_open())
            return;
        char chunk[16384];
        for (;;) { // from the client
            const int n = qb::io::socket::recv(_client.native_handle(), chunk, static_cast<int>(sizeof(chunk)));
            if (n > 0) {
                _from_client += static_cast<std::size_t>(n);
                if (_upstream_port != 0)
                    _to_server.append(chunk, static_cast<std::size_t>(n));
                else
                    _unread.append(chunk, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0)
                _client_end = 0;
            else if (!qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
                _client_end = 1;
            break;
        }
        if (_script && !_unread.empty())
            _to_client += _script(_unread);
        if (_server.is_open()) {
            for (;;) { // from the server
                const int n = qb::io::socket::recv(_server.native_handle(), chunk, static_cast<int>(sizeof(chunk)));
                if (n > 0) {
                    _to_client.append(chunk, static_cast<std::size_t>(n));
                    continue;
                }
                break; // its end is not the client's: the accepted connection stays as it is
            }
            flush(_server, _to_server);
        }
        flush(_client, _to_client);
    }

    /// From now on the peer is open and silent: `step()` does nothing, whatever the client sends stays unread.
    void
    go_silent() noexcept {
        _silent = true;
    }

    /// The peer goes away: the accepted connection is closed and the port refuses every new one.
    void
    go_away() {
        _client.close();
        _server.close();
        _listener.close();
    }

    /// The peer drops the connection it holds and accepts the next (a server that restarts).
    void
    drop_client() {
        _client.close();
        _server.close();
    }

    /// The port completes the TCP handshake of every new connection and nothing more: no accept from now on.
    void
    hold_new_connections() noexcept {
        _holding = true;
    }

    [[nodiscard]] bool
    has_client() const noexcept {
        return _client.is_open();
    }

    [[nodiscard]] std::size_t
    accepted() const noexcept {
        return _accepted;
    }

    /// Octets read from the client since the relay began.
    [[nodiscard]] std::size_t
    from_client() const noexcept {
        return _from_client;
    }

    /// The client's stream had ended (EOF or an error) when the relay last read it.
    [[nodiscard]] bool
    client_ended() const noexcept {
        return _client_end >= 0;
    }

    /// How the client's stream ends, within `budget`: 0 a clean EOF, 1 a reset, 2 still open -- and -1 when the relay
    /// holds no connection to read (it accepted none, or closed its end itself). What the relay read itself is
    /// remembered: a reset is reported once by the system, and reads as an EOF from then on.
    [[nodiscard]] int
    client_end(std::chrono::milliseconds const budget = std::chrono::milliseconds(2000)) {
        if (_client_end >= 0)
            return _client_end;
        if (!_client.is_open())
            return -1;
        const int end = stream_end(_client.native_handle(), budget);
        if (end != 2)
            _client_end = end;
        return end;
    }

private:
    static void
    flush(qb::io::tcp::socket &to, std::string &pending) {
        while (!pending.empty()) {
            const int n = qb::io::socket::send(to.native_handle(), pending.data(), static_cast<int>(pending.size()));
            if (n <= 0)
                return; // not now, or never: the next step tries again
            pending.erase(0, static_cast<std::size_t>(n));
        }
    }

    qb::io::tcp::listener _listener;
    qb::io::tcp::socket   _client;
    qb::io::tcp::socket   _server;
    std::string           _upstream_host;
    std::uint16_t         _upstream_port = 0;
    script                _script;
    std::string           _to_client;
    std::string           _to_server;
    std::string           _unread;
    std::uint16_t         _port        = 0;
    std::size_t           _accepted    = 0;
    std::size_t           _from_client = 0;
    int                   _client_end  = -1; ///< how the client's stream ended: -1 not yet, 0 EOF, 1 reset
    bool                  _silent      = false;
    bool                  _holding     = false;
};

/// Runs the loop without blocking, stepping `peer` between two passes, until `pred()` or the budget.
template <typename Pred>
[[nodiscard]] inline bool
pump_until(relay *const peer, Pred &&pred, std::chrono::milliseconds const budget = std::chrono::milliseconds(10000)) {
    const auto deadline = steady::now() + budget;
    for (;;) {
        if (peer)
            peer->step();
        qb::io::async::run(EVRUN_NOWAIT);
        if (pred())
            return true;
        if (steady::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// Runs the loop for `how_long`, stepping `peer` between two passes.
inline void
pump_for(relay *const peer, std::chrono::milliseconds const how_long) {
    static_cast<void>(pump_until(peer, [] { return false; }, how_long));
}

/// The answer to a question the process that runs the cases asks ONCE -- can the daemon of this tier serve them? --
/// and that the child of a case inherits through its environment. A child re-executes the binary, `SetUp()`
/// included, and must not ask again: asking uses the loop, and a case runs on a loop nothing used before it. `name`
/// is the environment variable that carries a yes; a no is remembered by the process that heard it, per type of
/// `ask` -- so a question is asked from ONE place, a function of its own, never from a member of a class template.
template <typename Ask>
[[nodiscard]] inline bool
asked_once(char const *const name, Ask &&ask) {
    if (std::getenv(name) != nullptr)
        return true;             // heard yes: this process at an earlier case, or the one that runs the cases
    static bool refused = false; // one per type of `ask`
    if (refused || !ask()) {
        refused = true;
        return false;
    }
#ifdef _WIN32
    static_cast<void>(::_putenv_s(name, "1"));
#else
    static_cast<void>(::setenv(name, "1", 1));
#endif
    return true;
}

/// A child's stderr is exactly `expected`: the judge's line and nothing else -- no warning, no sanitizer report, no
/// named stop. With `first_line`, one line that contains it comes before, and nothing else: the one line qb-io
/// prints by rule (a scheduler that abandons its coroutines says so). Windows writes a text stream's line ends as
/// CR LF: they are read as LF.
class stderr_is_matcher : public ::testing::MatcherInterface<std::string const &> {
public:
    stderr_is_matcher(std::string expected, std::string first_line)
        : _expected(std::move(expected))
        , _first_line(std::move(first_line)) {}

    bool
    MatchAndExplain(std::string const &actual, ::testing::MatchResultListener *) const override {
        std::string read;
        read.reserve(actual.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] == '\r' && i + 1 < actual.size() && actual[i + 1] == '\n')
                continue;
            read.push_back(actual[i]);
        }
        if (_first_line.empty())
            return read == _expected;
        const auto end = read.find('\n');
        if (end == std::string::npos)
            return false;
        return read.substr(0, end).find(_first_line) != std::string::npos && read.substr(end + 1) == _expected;
    }

    void
    DescribeTo(std::ostream *os) const override {
        if (!_first_line.empty())
            *os << "is one line that contains \"" << _first_line << "\", then ";
        *os << "is exactly \"" << _expected << "\"";
    }

    void
    DescribeNegationTo(std::ostream *os) const override {
        *os << "is not what a clean end prints: \"" << _expected << "\"";
    }

private:
    std::string _expected;
    std::string _first_line;
};

[[nodiscard]] inline ::testing::Matcher<std::string const &>
stderr_is(std::string expected, std::string first_line = {}) {
    return ::testing::MakeMatcher(new stderr_is_matcher(std::move(expected), std::move(first_line)));
}

// ---- What a child records, and its judge -----------------------------------------------------------------------------

/// What a case records for its judge. One child runs one case: a single record.
struct record {
    relay *peer        = nullptr;
    int    want_peer   = -1;   ///< how the peer's stream must end: 0 a clean EOF, 2 still open; -1 the case has no connection
    bool   check_leaks = true; ///< false where the frames are abandoned by rule (`exit()` in a coroutine)
    bool   loop_gone   = false;
    bool   destroyed   = false; ///< the client's destructor returned
    bool   want_alive  = false; ///< true where the client outlives its static owner by rule: `exit()` inside its own dispatch, which holds it
    int    calls       = 0;     ///< the callback of what was in flight ran
    int    late_calls  = 0;     ///< ... after the thread's loop
    int    outcome     = 0;     ///< what it received last: the module's own code (an HTTP status, 1 a reply, 2 a failure)
    int    resumed     = 0;     ///< the coroutine parked on the client went past its co_await
    int    frames      = 0;     ///< its frame was destroyed
    int    late_frames = 0;     ///< ... after the thread's loop
    bool   parked      = false;
};

inline record g;

/// The judge: an `atexit` handler registered before the object with static storage is built. One line, then the end.
inline void
judge() {
    const int peer  = g.want_peer >= 0 && g.peer ? g.peer->client_end() : -1;
    const int leaks = g.check_leaks ? leaks_now() : 0;
    std::fprintf(stderr,
                 "judge: after the loop %d, destroyed %d, peer %d, callback %d (after the loop %d, outcome %d), "
                 "coroutine resumed %d, frame destroyed %d (after the loop %d), leaks %d\n",
                 g.loop_gone ? 1 : 0, g.destroyed ? 1 : 0, peer, g.calls, g.late_calls, g.outcome, g.resumed, g.frames, g.late_frames, leaks);
    end_child(g.loop_gone && g.destroyed != g.want_alive && peer == g.want_peer && leaks == 0);
}

[[nodiscard]] inline std::string
judged(bool const destroyed, int const peer, int const calls, int const late_calls, int const outcome, int const resumed, int const frames,
       int const late_frames) {
    char line[320];
    std::snprintf(line, sizeof line,
                  "judge: after the loop 1, destroyed %d, peer %d, callback %d (after the loop %d, outcome %d), "
                  "coroutine resumed %d, frame destroyed %d (after the loop %d), leaks 0\n",
                  destroyed ? 1 : 0, peer, calls, late_calls, outcome, resumed, frames, late_frames);
    return line;
}

/// The judge's line of a case that ended as it must: the client destroyed after the loop, no leak, and the rest as given.
[[nodiscard]] inline std::string
verdict(int const peer, int const calls, int const late_calls, int const outcome, int const resumed, int const frames, int const late_frames) {
    return judged(true, peer, calls, late_calls, outcome, resumed, frames, late_frames);
}

/// ... of a case whose client must be left ALIVE (`record::want_alive`): its static owner destroyed after the loop,
/// the client not, no leak, and the rest as given.
[[nodiscard]] inline std::string
left_alive(int const peer, int const calls, int const late_calls, int const outcome, int const resumed, int const frames,
           int const late_frames) {
    return judged(false, peer, calls, late_calls, outcome, resumed, frames, late_frames);
}

/// An object with static storage: `client`, destroyed after the main thread's loop.
template <typename Client>
struct at_exit {
    destroyed_flag done{g.destroyed}; ///< destroyed after `client`: its destructor returned
    Client         client;

    template <typename... Args>
    explicit at_exit(Args &&...args)
        : client(std::forward<Args>(args)...) {}
    at_exit(at_exit const &)            = delete;
    at_exit &operator=(at_exit const &) = delete;
    ~at_exit() {
        g.loop_gone = loop_gone(); // before `client`'s destructor
    }
};

/// The same for a client its program shares (`make_client()`): what has static storage is a pointer, and the client
/// is destroyed with it only when it is the last owner. `destroyed` is said of the CLIENT here too: the pointer's
/// destructor returned and nothing holds the client any more.
template <typename Shared>
struct at_exit<std::shared_ptr<Shared>> {
    struct expired_flag {
        std::weak_ptr<Shared> watched;
        expired_flag()                                = default;
        expired_flag(expired_flag const &)            = delete;
        expired_flag &operator=(expired_flag const &) = delete;
        ~expired_flag() {
            g.destroyed = watched.expired();
        }
    } done; ///< destroyed after `client`
    std::shared_ptr<Shared> client;

    explicit at_exit(std::shared_ptr<Shared> owner)
        : client(std::move(owner)) {
        done.watched = client;
    }
    at_exit(at_exit const &)            = delete;
    at_exit &operator=(at_exit const &) = delete;
    ~at_exit() {
        g.loop_gone = loop_gone(); // before the pointer's destructor
    }
};

/// A local of a coroutine parked on a client.
struct frame_local {
    frame_local()                               = default;
    frame_local(frame_local const &)            = delete;
    frame_local &operator=(frame_local const &) = delete;
    ~frame_local() {
        ++g.frames;
        if (loop_gone())
            ++g.late_frames;
    }
};

/// The callback of what a case left in flight ran, with `outcome`.
inline void
called_with(int const outcome) noexcept {
    ++g.calls;
    if (loop_gone())
        ++g.late_calls;
    g.outcome = outcome;
}

/// The peer of the case: on the heap and never destroyed, so the judge reads its accepted end. The thread's
/// `listener::current` exists from here on -- a thread-local is built on its first use, and a case whose object never
/// reaches for the loop must still be destroyed AFTER it, not on a thread that never had one.
[[nodiscard]] inline relay &
new_peer() {
    static_cast<void>(qb::io::async::listener::current.size());
    auto *peer = new relay;
    if (!peer->listen())
        setup_failed("the relay could not listen");
    g.peer = peer;
    return *peer;
}

// ---- Where a case ends the process -----------------------------------------------------------------------------------

enum class site {
    main,      ///< `exit()` with no dispatch on the stack: what the return from `main` does
    callback,  ///< `exit()` from a callback of the loop, after one whole pass of it at least
    coroutine, ///< `exit()` from a coroutine: the thread's scheduler abandons what it owns, and says so
};

inline qb::io::async::task<void>
exit_from_a_coroutine() {
    co_await qb::io::async::sleep(qb::duration::zero()); // a cooperative yield: resumed by the scheduler's drain
    std::exit(3);
}

/// Ends the process from `where`. The relay is not stepped any more: the peer is open and silent from here on.
[[noreturn]] inline void
leave(site const where) {
    switch (where) {
        case site::main:
            std::exit(3); // the thread-locals (the listener), then the statics, then the judge
        case site::callback:
            // Armed at the tail of the next pass, never beside it: what the case left to the loop -- an event fed, a
            // callback deferred, a coroutine woken -- runs in a pass that ends before this timer exists. Armed here,
            // a millisecond lost between this line and that pass let the exit overtake them.
            qb::io::async::defer([] { qb::io::async::callback([] { std::exit(3); }, std::chrono::milliseconds(1)); });
            break;
        case site::coroutine:
            g.check_leaks = false; // the frames its scheduler abandons are a leak by rule
            qb::io::async::coro_scheduler().spawn(exit_from_a_coroutine());
            break;
    }
    pump_for(nullptr, std::chrono::seconds(30));
    setup_failed("exit() was never called");
}

/// The line a scheduler prints when `exit()` is called from one of its coroutines, for `stderr_is`'s first line.
[[nodiscard]] inline std::string
abandoned(int const coroutines) {
    return "destroyed FROM INSIDE one of its own coroutines with " + std::to_string(coroutines) + " coroutine(s)";
}

[[nodiscard]] inline std::string
site_name(::testing::TestParamInfo<site> const &info) {
    switch (info.param) {
        case site::main:
            return "FromMain";
        case site::callback:
            return "FromACallbackOfTheLoop";
        case site::coroutine:
            return "FromACoroutine";
    }
    return "Unknown";
}

/// The fixture of a case that runs once: every case is a child process, re-executed.
class in_a_child : public ::testing::Test {
protected:
    void
    SetUp() override {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

/// The fixture of a case that runs once per place a program ends from.
class in_a_child_from : public ::testing::TestWithParam<site> {
protected:
    void
    SetUp() override {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

} // namespace qb::io::test::static_exit

#endif // QB_IO_TEST_STATIC_EXIT_SUPPORT_H
