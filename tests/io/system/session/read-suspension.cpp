/**
 * @file qb/tests/io/system/session/read-suspension.cpp
 * @brief A session suspends and resumes its READING (`pause_reading()` / `resume_reading()`, Huly QB-1270), over TCP
 *        and over TLS.
 *
 * What is held here, each case once per transport:
 *  - the message whose handler suspends is the last of its read pass, and the resume hands the rest to the protocol
 *    with nothing more on the wire, in order -- from the loop, never from inside `resume_reading()`;
 *  - nothing re-arms readability under a suspension: a drain of the output, `ready_to_read()`, an empty write arm;
 *  - a readiness dispatched while suspended reads nothing for the application and does not end the session;
 *  - the peer's end of stream, sent during the suspension, is seen at the resume, after what was retained, once --
 *    over TLS also when the peer sends its `close_notify` and keeps the TCP connection open;
 *  - `close_after_deliver()` and its lingering close read through a suspension;
 *  - a suspension lasts until the resume or the end of the connection: a `stop()` and a `start()` leave it, a pause
 *    made in a session's constructor holds from the first `start()`, and what the suspension retained is delivered
 *    whatever the order of a resume around a `stop()` and a `start()` -- with nothing more on the wire;
 *  - a handler that suspends and resumes before it returns changes nothing, and a resume of a reading that is not
 *    suspended has no effect;
 *  - a message that suspends and ends its protocol ends the session; `close_after_deliver()` with nothing queued
 *    still ends at the peer's end; one request at a time, each answer flushed before the next request is handled;
 *  - over TLS a session suspended before its first handshake completed still completes it;
 *  - what a TLS stream holds decrypted stays there under a suspension, and is read at the resume;
 *  - `input<>`, the other base that reads, suspends and resumes the same way -- and so does a server, whose reading
 *    is its accepting: the connections wait in the listen backlog;
 *  - a transport taken from a suspended session (`extractSession()`) is read by its new owner, what waited in the
 *    socket included;
 *  - the call does not compile on a datagram component.
 *
 * Both ends live on the test thread: the server and its one session on this thread's loop, the client a raw socket
 * the case writes to -- one write is one TCP segment on the loopback, and one record over TLS -- so the case knows
 * which read brought what. Everything TLS is under `QB_HAS_SSL`: a build without OpenSSL keeps the TCP cases.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#ifdef QB_HAS_SSL
#include <openssl/ssl.h>

#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>
#endif

#include "../../shared/coroutine_test_support.h"
#ifdef QB_HAS_SSL
#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_pump.h"
#endif

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
#ifdef QB_HAS_SSL
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;
#endif

namespace read_suspension_test {

/// Line `index` of a case, its delimiter included.
std::string
line(std::size_t index) {
    return "line " + std::to_string(index) + '\n';
}

/// Lines `first` to `last - 1`, one after the other: what ONE write of the client carries.
std::string
lines(std::size_t first, std::size_t last) {
    std::string all;
    for (std::size_t i = first; i < last; ++i)
        all += line(i);
    return all;
}

/// A line of exactly a hundred octets, its delimiter included: thirty of them are one TLS record three times a read
/// cap of 1024 octets.
std::string
long_line(std::size_t index) {
    std::string text = std::to_string(index) + ':';
    text.resize(99u, static_cast<char>('a' + index % 26u));
    text += '\n';
    return text;
}

std::string
long_lines(std::size_t count) {
    std::string all;
    for (std::size_t i = 0; i < count; ++i)
        all += long_line(i);
    return all;
}

/// How many of the delivered lines are not the long line the case sent at that place.
std::size_t
misordered_long_lines(std::vector<std::string> const &got) {
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (long_line(i).compare(0u, 99u, got[i]) != 0)
            ++wrong;
    return wrong;
}

/// What the session is expected to have been handed once lines `0` to `count - 1` were delivered.
std::vector<std::string>
delivered(std::size_t count) {
    std::vector<std::string> all;
    for (std::size_t i = 0; i < count; ++i)
        all.emplace_back("line " + std::to_string(i));
    return all;
}

/// What the session of a case is told to do, and what it saw. Everything runs on the test thread.
struct Probe {
    std::size_t suspend_at            = 0;     ///< the handler of that line (counted from 1) suspends reading
    std::size_t suspend_and_resume_at = 0;     ///< the handler of that line suspends, and resumes before it returns
    std::size_t reply_at              = 0;     ///< the handler of that line answers "ack"
    std::size_t arm_at                = 0;     ///< the handler of that line asks for a write pass with nothing queued
    std::size_t reply_and_clear_at    = 0;     ///< the handler of that line answers "bye", then clears the protocols
    bool        suspend_at_pending    = false; ///< the first `pending_read` handler suspends the reading
    bool        suspend_when_accepted = false; ///< the server suspends the session as soon as it holds it
    bool        suspend_when_built    = false; ///< the session suspends its reading in its constructor, before any start()
    std::size_t suspend_and_end_at    = 0;     ///< the handler of that line suspends, then clears the protocols
    std::size_t switch_and_suspend_at = 0;     ///< the handler of that line switches a new protocol in, then suspends
    bool        one_at_a_time         = false; ///< every handler suspends and answers; the answer's eos resumes
    std::size_t accept_limit          = 0;     ///< the server suspends its ACCEPTING once it holds that many sessions
    std::size_t read_cap              = 0;     ///< the read cap the session sets when it is built (0: its default)

    std::vector<std::string> lines; ///< delivered, in order
    int                      eos           = 0;
    int                      pending_reads = 0; ///< `pending_read` events: a read pass that left octets
    int                      eofs          = 0; ///< `eof` events: a read pass that left none
    std::vector<int>         eos_at_message;    ///< how many `eos` had come when each message was handled
    int                      disconnections             = 0;
    int                      reason                     = 0;
    std::size_t              delivered_at_disconnection = 0;
};

Probe g_probe;

/// Whether @p Transport is TLS, asked OUTSIDE the session: MSVC refuses `Transport::is_secure()` written inside a class
/// that holds its transport as a private base (see tls-text-roundtrip.cpp).
template <typename Transport>
inline constexpr bool is_tls_transport = Transport::is_secure();

template <typename Transport, typename Accept>
class Server;

template <typename Transport, typename Accept>
class Session : public async::tcp::client<Session<Transport, Accept>, Transport, Server<Transport, Accept>> {
    using base_t = async::tcp::client<Session<Transport, Accept>, Transport, Server<Transport, Accept>>;

public:
    using Protocol = qb::protocol::text::command<Session>;

    explicit Session(Server<Transport, Accept> &server)
        : base_t(server) {
        if (g_probe.read_cap != 0u)
            this->set_max_read_buffer_size(g_probe.read_cap);
        if (g_probe.suspend_when_built)
            this->pause_reading(); // before the socket is installed and before any start()
    }

    void
    on(typename Protocol::message &&msg) {
        g_probe.lines.emplace_back(msg.text);
        g_probe.eos_at_message.push_back(g_probe.eos);
        const std::size_t count = g_probe.lines.size();
        if (count == g_probe.suspend_at)
            this->pause_reading();
        if (g_probe.one_at_a_time) {
            this->pause_reading();
            *this << "ack" << Protocol::end;
        }
        if (count == g_probe.switch_and_suspend_at) {
            static_cast<void>(this->template switch_protocol<Protocol>(*this)); // the old one is retired with its message
            this->pause_reading();
        }
        if (count == g_probe.suspend_and_end_at) {
            this->pause_reading();
            this->clear_protocols();
        }
        if (count == g_probe.suspend_and_resume_at) {
            this->pause_reading();
            this->resume_reading();
        }
        if (count == g_probe.reply_at)
            *this << "ack" << Protocol::end;
        if (count == g_probe.arm_at)
            this->ready_to_write();
        if (count == g_probe.reply_and_clear_at) {
            *this << "bye" << Protocol::end;
            this->clear_protocols(); // the answer is delivered, then the session closes -- with no protocol left
        }
    }

    void
    on(async::event::eos &&) {
        ++g_probe.eos;
        if (g_probe.one_at_a_time)
            this->resume_reading(); // the answer has left: the next request may come
    }

    void
    on(async::event::eof &&) {
        ++g_probe.eofs;
    }

    void
    on(async::event::pending_read &&) {
        ++g_probe.pending_reads;
        if (g_probe.suspend_at_pending && g_probe.pending_reads == 1)
            this->pause_reading(); // an incomplete message is in: nothing more is taken until the resume
    }

    void
    on(async::event::disconnected const &event) {
        ++g_probe.disconnections;
        g_probe.reason                     = event.reason;
        g_probe.delivered_at_disconnection = g_probe.lines.size();
    }

    /// A readiness for reading, queued by hand: what the loop holds when the socket became readable before the
    /// suspension and the dispatch comes after it.
    void
    feed_readiness() {
        this->_async_event.feed_event(EV_READ);
    }

    /// Whether this session's own TLS handshake is still under way; never over TCP.
    [[nodiscard]] bool
    handshake_under_way() {
        if constexpr (is_tls_transport<Transport>)
            return this->transport().handshaking();
        else
            return false;
    }
};

template <typename Transport, typename Accept>
class Server : public async::tcp::server<Server<Transport, Accept>, Session<Transport, Accept>, Accept> {
public:
    /// The session is started, its socket installed: the first moment anything can suspend it.
    void
    on(Session<Transport, Accept> &session) {
        if (g_probe.suspend_when_accepted)
            session.pause_reading();
        if (g_probe.accept_limit != 0u && this->session_count() >= g_probe.accept_limit)
            this->pause_reading(); // the server's own reading: its accepting
    }
};

/// A listening server, its one session, and the raw client end the case writes to and reads from.
template <typename Transport, typename Accept>
class Link {
    using Peer = typename Transport::transport_io_type;

    Server<Transport, Accept> _server;
    Peer                      _peer;

public:
    /// @param suspend_when_accepted the server suspends the session's reading as soon as it holds it.
    /// @param max_tls_version       the highest TLS version the server accepts (0: the library's).
    /// @param read_cap              the read cap the session sets when it is built (0: its default).
    /// @param suspend_when_built    the session suspends its reading in its constructor.
    [[nodiscard]] ::testing::AssertionResult
    open(bool suspend_when_accepted = false, [[maybe_unused]] int max_tls_version = 0, std::size_t read_cap = 0,
         bool suspend_when_built = false) {
        g_probe                       = Probe{};
        g_probe.suspend_when_accepted = suspend_when_accepted;
        g_probe.read_cap              = read_cap;
        g_probe.suspend_when_built    = suspend_when_built;
        async::init();
#ifdef QB_HAS_SSL
        if constexpr (Accept::is_secure()) {
            if (!require_ssl_files())
                return ::testing::AssertionFailure() << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");
            auto context = ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"));
            if (max_tls_version != 0)
                SSL_CTX_set_max_proto_version(context.native(), max_tls_version);
            _server.transport().init(std::move(context));
        }
#endif
        if (_server.transport().listen_v4(0, "127.0.0.1") != 0)
            return ::testing::AssertionFailure() << "listen_v4 failed";
        const auto port = _server.transport().local_endpoint().port();
        _server.start();

        if constexpr (Transport::is_secure()) {
            _peer.set_insecure(); // self-signed local cert
            const int ret = _peer.n_connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
            if (ret != 0 && !qb::io::socket_no_error(qb::io::socket::get_last_errno()))
                return ::testing::AssertionFailure() << "n_connect failed: " << ret;
        } else {
            if (_peer.connect_v4("127.0.0.1", port) != 0)
                return ::testing::AssertionFailure() << "connect_v4 failed";
            _peer.set_nonblocking(true);
        }
        // The session's side of the handshake runs on this thread's loop, the client's here.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            async::run_for(1ms);
            bool up = _server.session_count() == 1u;
            if constexpr (Transport::is_secure()) {
                if (!_peer.handshake_complete() && _peer.handshake_status() < 0)
                    return ::testing::AssertionFailure() << "client handshake failed: " << _peer.get_last_ssl_error_string();
                up = up && _peer.handshake_complete();
            }
            // The session's own handshake too: over TLS 1.3 the client is done before the server has read its Finished.
            up = up && !session()->handshake_under_way();
            if (up)
                return ::testing::AssertionSuccess();
            if (std::chrono::steady_clock::now() >= deadline)
                return ::testing::AssertionFailure() << "no session within the deadline";
        }
    }

    /// The session, while the server holds it.
    [[nodiscard]] auto
    session() {
        return _server.session_count() ? _server.sessions().begin()->second : nullptr;
    }

    [[nodiscard]] Peer &
    peer() noexcept {
        return _peer;
    }

    /// The session's transport, taken from it (`extractSession()`): the session is gone, the connection is not.
    [[nodiscard]] auto
    extract() {
        return _server.extractSession(_server.sessions().begin()->first);
    }

    /// The client writes `octets` in ONE write: one segment, one record.
    [[nodiscard]] ::testing::AssertionResult
    send(std::string_view octets) {
        const int ret = _peer.write(octets.data(), octets.size());
        if (ret != static_cast<int>(octets.size()))
            return ::testing::AssertionFailure() << "the client's write took " << ret << " of " << octets.size() << " octets";
        return ::testing::AssertionSuccess();
    }

    /// What the client has received so far, the loop pumped until it holds `wanted` octets or `budget` has passed.
    [[nodiscard]] std::string
    received(std::size_t wanted, std::chrono::milliseconds budget = 5s) {
        std::string all;
        const auto  deadline = std::chrono::steady_clock::now() + budget;
        while (all.size() < wanted && std::chrono::steady_clock::now() < deadline) {
            char      chunk[256];
            const int ret = _peer.read(chunk, sizeof chunk);
            if (ret > 0)
                all.append(chunk, static_cast<std::size_t>(ret));
            else
                async::run_for(1ms);
        }
        return all;
    }

    /// The client ends its stream and closes: a `close_notify` and a FIN over TLS, a FIN over TCP.
    void
    end_of_stream() {
        static_cast<void>(_peer.disconnect());
        _peer.close();
    }

    ~Link() {
        static_cast<void>(_peer.disconnect());
        for (int i = 0; i < 10; ++i)
            async::run_for(1ms);
        async::listener::current.clear();
    }
};

/// The loop runs for `wait`: time for something that must NOT happen to happen.
void
settle(std::chrono::milliseconds wait = 60ms) {
    async::run_for(wait);
}

// ---- the message that suspends is the last of its pass; the resume delivers the rest from the loop --------------

template <typename Transport, typename Accept>
void
expect_a_suspending_message_is_the_last_of_its_pass_and_the_resume_delivers_the_rest() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 3;

    ASSERT_TRUE(io.send(lines(0, 10))); // ten lines in one read
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 3u; }, 5s));
    settle();
    EXPECT_EQ(g_probe.lines, delivered(3)) << "the message that suspended was not the last of its read pass";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_TRUE(io.session()->reading_paused());
    EXPECT_FALSE(io.session()->is_reading()) << "readability is still watched under a suspension";
    EXPECT_EQ(g_probe.disconnections, 0);

    // Nothing more is on the wire: what the resume delivers was received before it.
    io.session()->resume_reading();
    EXPECT_FALSE(io.session()->reading_paused());
    EXPECT_EQ(g_probe.lines.size(), 3u) << "resume_reading() handed messages to the protocol from inside the call";
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 10u; }, 5s)) << "what the suspension retained was never delivered";
    EXPECT_EQ(g_probe.lines, delivered(10));
    ASSERT_NE(io.session(), nullptr) << "the session ended at its resume; reason " << g_probe.reason;
    EXPECT_TRUE(io.session()->is_reading());

    // And the session reads on.
    ASSERT_TRUE(io.send(line(10)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 11u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(11));
    EXPECT_EQ(g_probe.disconnections, 0);
}

TEST(ReadSuspension, ASuspendingMessageIsTheLastOfItsPassAndTheResumeDeliversTheRestOverTcp) {
    expect_a_suspending_message_is_the_last_of_its_pass_and_the_resume_delivers_the_rest<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASuspendingMessageIsTheLastOfItsPassAndTheResumeDeliversTheRestOverTls) {
    expect_a_suspending_message_is_the_last_of_its_pass_and_the_resume_delivers_the_rest<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- a handler that suspends and resumes before it returns changes nothing ---------------------------------------

template <typename Transport, typename Accept>
void
expect_a_handler_that_suspends_and_resumes_leaves_its_pass_going() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_and_resume_at = 2;
    const int passes              = g_probe.eofs; // the handshake's read passes, over TLS

    ASSERT_TRUE(io.send(lines(0, 5)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 5u; }, 5s)) << "a pass whose handler suspended and resumed stopped";
    settle();
    EXPECT_EQ(g_probe.lines, delivered(5));
    // Five short lines in one write are one segment (one record over TLS) and one read: one pass that left nothing.
    EXPECT_EQ(g_probe.eofs - passes, 1) << "the resume made inside the read pass asked the loop for a dispatch of its own";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_FALSE(io.session()->reading_paused());
    EXPECT_TRUE(io.session()->is_reading());
}

TEST(ReadSuspension, AHandlerThatSuspendsAndResumesLeavesItsPassGoingOverTcp) {
    expect_a_handler_that_suspends_and_resumes_leaves_its_pass_going<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AHandlerThatSuspendsAndResumesLeavesItsPassGoingOverTls) {
    expect_a_handler_that_suspends_and_resumes_leaves_its_pass_going<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- a resume of a reading that is not suspended has no effect --------------------------------------------------

template <typename Transport, typename Accept>
void
expect_a_resume_of_a_reading_that_is_not_suspended_has_no_effect() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());

    ASSERT_TRUE(io.send("line")); // no delimiter yet: an incomplete message stays in the buffer
    EXPECT_TRUE(pump_until([] { return g_probe.pending_reads >= 1; }, 5s)) << "the case proves nothing: nothing was read";
    settle();
    const int told = g_probe.pending_reads;
    ASSERT_NE(io.session(), nullptr);
    ASSERT_FALSE(io.session()->reading_paused());

    io.session()->resume_reading();
    settle();
    EXPECT_EQ(g_probe.pending_reads, told) << "a resume of a reading that was not suspended dispatched the component";
    EXPECT_TRUE(g_probe.lines.empty());
    ASSERT_NE(io.session(), nullptr);
    EXPECT_TRUE(io.session()->is_reading());

    ASSERT_TRUE(io.send(" 0\n"));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(1));
    EXPECT_EQ(g_probe.disconnections, 0);
}

TEST(ReadSuspension, AResumeOfAReadingThatIsNotSuspendedHasNoEffectOverTcp) {
    expect_a_resume_of_a_reading_that_is_not_suspended_has_no_effect<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AResumeOfAReadingThatIsNotSuspendedHasNoEffectOverTls) {
    expect_a_resume_of_a_reading_that_is_not_suspended_has_no_effect<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- nothing re-arms readability under a suspension ---------------------------------------------------------------

template <typename Transport, typename Accept>
void
expect_a_drain_of_the_output_does_not_read_again() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;
    g_probe.reply_at   = 1; // the handler that suspends answers: the write pass that follows drains the output

    ASSERT_TRUE(io.send(line(0)));
    EXPECT_EQ(io.received(4), "ack\n");
    EXPECT_TRUE(pump_until([] { return g_probe.eos >= 1; }, 5s));
    ASSERT_NE(io.session(), nullptr);
    EXPECT_FALSE(io.session()->is_reading()) << "the drain of the output watched readability again under a suspension";

    ASSERT_TRUE(io.send(lines(1, 4)));
    settle();
    EXPECT_EQ(g_probe.lines, delivered(1)) << "a suspended session was handed messages";
    EXPECT_EQ(g_probe.disconnections, 0);

    ASSERT_NE(io.session(), nullptr) << "the session ended under its suspension; reason " << g_probe.reason;
    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 4u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(4));
}

TEST(ReadSuspension, ADrainOfTheOutputDoesNotReadAgainOverTcp) {
    expect_a_drain_of_the_output_does_not_read_again<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ADrainOfTheOutputDoesNotReadAgainOverTls) {
    expect_a_drain_of_the_output_does_not_read_again<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

template <typename Transport, typename Accept>
void
expect_ready_to_read_has_no_effect_under_a_suspension() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    ASSERT_NE(io.session(), nullptr);

    io.session()->pause_reading(); // from outside any handler
    io.session()->ready_to_read();
    EXPECT_FALSE(io.session()->is_reading()) << "ready_to_read() read again under a suspension";
    EXPECT_TRUE(io.session()->reading_paused());

    ASSERT_TRUE(io.send(lines(0, 3)));
    settle();
    EXPECT_TRUE(g_probe.lines.empty()) << "a suspended session was handed messages";
    EXPECT_EQ(g_probe.disconnections, 0);

    ASSERT_NE(io.session(), nullptr) << "the session ended under its suspension; reason " << g_probe.reason;
    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 3u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(3));
}

TEST(ReadSuspension, ReadyToReadHasNoEffectUnderASuspensionOverTcp) {
    expect_ready_to_read_has_no_effect_under_a_suspension<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ReadyToReadHasNoEffectUnderASuspensionOverTls) {
    expect_ready_to_read_has_no_effect_under_a_suspension<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

template <typename Transport, typename Accept>
void
expect_an_empty_write_arm_under_a_suspension_earns_one_eos_and_reads_nothing() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;
    g_probe.arm_at     = 1; // the handler that suspends asks for a write pass with nothing queued

    ASSERT_TRUE(io.send(line(0)));
    EXPECT_TRUE(pump_until([] { return g_probe.eos >= 1 || g_probe.disconnections > 0; }, 5s)) << "the empty arm was never answered";
    settle();
    EXPECT_EQ(g_probe.eos, 1) << "an empty arm is answered by exactly one eos, suspended or not";
    EXPECT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);
    EXPECT_FALSE(io.session()->is_reading()) << "the answer to an empty arm watched readability again under a suspension";

    ASSERT_TRUE(io.send(lines(1, 3)));
    settle();
    EXPECT_EQ(g_probe.lines, delivered(1));

    ASSERT_NE(io.session(), nullptr) << "the session ended under its suspension; reason " << g_probe.reason;
    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 3u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(3));
}

TEST(ReadSuspension, AnEmptyWriteArmUnderASuspensionEarnsOneEosAndReadsNothingOverTcp) {
    expect_an_empty_write_arm_under_a_suspension_earns_one_eos_and_reads_nothing<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AnEmptyWriteArmUnderASuspensionEarnsOneEosAndReadsNothingOverTls) {
    expect_an_empty_write_arm_under_a_suspension_earns_one_eos_and_reads_nothing<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- a readiness dispatched while suspended ------------------------------------------------------------------------

template <typename Transport, typename Accept>
void
expect_a_readiness_dispatched_under_a_suspension_does_not_end_the_session() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    ASSERT_NE(io.session(), nullptr);

    ASSERT_TRUE(io.send(lines(0, 2)));
    io.session()->pause_reading();
    io.session()->feed_readiness(); // the dispatch of a readiness the loop already held
    settle();
    EXPECT_EQ(g_probe.disconnections, 0) << "a dispatch that read nothing for a suspended session ended it; reason " << g_probe.reason;
    EXPECT_TRUE(g_probe.lines.empty()) << "a suspended session was handed messages";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_FALSE(io.session()->is_reading());

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 2u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(2));
}

TEST(ReadSuspension, AReadinessDispatchedUnderASuspensionDoesNotEndTheSessionOverTcp) {
    expect_a_readiness_dispatched_under_a_suspension_does_not_end_the_session<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AReadinessDispatchedUnderASuspensionDoesNotEndTheSessionOverTls) {
    expect_a_readiness_dispatched_under_a_suspension_does_not_end_the_session<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- the peer's end, sent during the suspension --------------------------------------------------------------------

template <typename Transport, typename Accept>
void
expect_the_peers_end_is_seen_at_the_resume_after_what_was_retained_once() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;

    ASSERT_TRUE(io.send(lines(0, 5)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    io.end_of_stream();
    settle(100ms);
    EXPECT_EQ(g_probe.lines, delivered(1));
    EXPECT_EQ(g_probe.disconnections, 0) << "a suspended session with nothing to write learned of its peer's end";
    ASSERT_NE(io.session(), nullptr);

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 5s)) << "the peer's end was never seen after the resume";
    settle();
    EXPECT_EQ(g_probe.lines, delivered(5)) << "what the suspension retained was not delivered before the end";
    EXPECT_EQ(g_probe.delivered_at_disconnection, 5u) << "the end was said before what had been received";
    EXPECT_EQ(g_probe.disconnections, 1);
}

TEST(ReadSuspension, ThePeersEndIsSeenAtTheResumeAfterWhatWasRetainedOnceOverTcp) {
    expect_the_peers_end_is_seen_at_the_resume_after_what_was_retained_once<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ThePeersEndIsSeenAtTheResumeAfterWhatWasRetainedOnceOverTls) {
    expect_the_peers_end_is_seen_at_the_resume_after_what_was_retained_once<transport::stcp, transport::saccept>();
}

TEST(ReadSuspension, ACloseNotifyFromAPeerThatKeepsTcpOpenEndsTheSessionOnceAfterWhatWasRetained) {
    // The peer says its TLS stream is over and keeps the connection: no FIN follows the `close_notify`. Read by the
    // pass that resumes, with input retained, the `close_notify` must still end the session -- once.
    Link<transport::stcp, transport::saccept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;

    ASSERT_TRUE(io.send(lines(0, 4)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    static_cast<void>(SSL_shutdown(io.peer().ssl_handle())); // the close_notify alone
    settle(100ms);
    EXPECT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 5s)) << "the close_notify was never seen: the session waits for ever";
    settle();
    EXPECT_EQ(g_probe.lines, delivered(4));
    EXPECT_EQ(g_probe.delivered_at_disconnection, 4u);
    EXPECT_EQ(g_probe.disconnections, 1);
}
#endif // QB_HAS_SSL

// ---- close_after_deliver() and its lingering close read through a suspension ------------------------------------

template <typename Transport, typename Accept>
void
expect_close_after_deliver_reads_through_a_suspension() {
    using Protocol = typename Session<Transport, Accept>::Protocol;
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;

    ASSERT_TRUE(io.send(lines(0, 3)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    ASSERT_NE(io.session(), nullptr);
    *io.session() << "bye" << Protocol::end;
    io.session()->close_after_deliver();
    EXPECT_EQ(io.received(4), "bye\n") << "the output of a suspended session that closes after delivery never left";
    ASSERT_NE(io.session(), nullptr);
    io.session()->pause_reading(); // asked again while the close lingers: the linger's read is not the application's

    // Delivered: the session lingers for its peer's end. It reads that end although its reading is suspended -- it
    // would otherwise wait for the whole linger bound (two seconds by default).
    const auto asked = std::chrono::steady_clock::now();
    io.end_of_stream();
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 5s));
    EXPECT_LT(std::chrono::steady_clock::now() - asked, 1s) << "the lingering close did not read its peer's end through the suspension";
    EXPECT_EQ(g_probe.disconnections, 1);
    EXPECT_EQ(g_probe.lines, delivered(1)) << "a session that closes after delivery reads no more messages";
}

// A session that cleared its protocols and still had output closes after its delivery as well, and lingers for its
// peer with NO protocol: nothing but the linger itself says a close is under way. A suspension asked then must leave
// the linger's read, or the close waits for its whole bound.
template <typename Transport, typename Accept>
void
expect_a_suspension_asked_during_the_linger_of_a_session_with_no_protocol_leaves_its_read() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.reply_and_clear_at = 1;

    ASSERT_TRUE(io.send(line(0)));
    EXPECT_EQ(io.received(4), "bye\n") << "the answer of a session that cleared its protocols never left";
    ASSERT_TRUE(pump_until([&] { return io.session() != nullptr && io.session()->lingering(); }, 5s))
        << "the case proves nothing: the session does not linger";
    io.session()->pause_reading();

    const auto asked = std::chrono::steady_clock::now();
    io.end_of_stream();
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 5s));
    EXPECT_LT(std::chrono::steady_clock::now() - asked, 1s) << "the lingering close did not read its peer's end through the suspension";
    EXPECT_EQ(g_probe.disconnections, 1);
}

TEST(ReadSuspension, ASuspensionAskedDuringTheLingerOfASessionWithNoProtocolLeavesItsReadOverTcp) {
    expect_a_suspension_asked_during_the_linger_of_a_session_with_no_protocol_leaves_its_read<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASuspensionAskedDuringTheLingerOfASessionWithNoProtocolLeavesItsReadOverTls) {
    expect_a_suspension_asked_during_the_linger_of_a_session_with_no_protocol_leaves_its_read<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

TEST(ReadSuspension, CloseAfterDeliverReadsThroughASuspensionOverTcp) {
    expect_close_after_deliver_reads_through_a_suspension<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, CloseAfterDeliverReadsThroughASuspensionOverTls) {
    expect_close_after_deliver_reads_through_a_suspension<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- a suspension lasts until the resume, or the end of the connection --------------------------------------------
// `start()` neither makes nor ends one: whoever stops and starts a component does not lift a suspension its owner
// relies on, and a session that suspends its reading in its constructor -- before the server starts it -- starts
// suspended.

template <typename Transport, typename Accept>
void
expect_a_stop_and_a_start_leave_the_suspension() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    ASSERT_NE(io.session(), nullptr);

    io.session()->pause_reading();
    io.session()->stop();
    io.session()->start();
    EXPECT_TRUE(io.session()->reading_paused()) << "a start lifted a suspension it had not made";
    EXPECT_FALSE(io.session()->is_reading()) << "a start watched readability under a suspension";

    ASSERT_TRUE(io.send(lines(0, 2)));
    settle();
    EXPECT_TRUE(g_probe.lines.empty()) << "a suspended session was handed messages after a start";
    EXPECT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 2u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(2));
}

TEST(ReadSuspension, AStopAndAStartLeaveTheSuspensionOverTcp) {
    expect_a_stop_and_a_start_leave_the_suspension<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AStopAndAStartLeaveTheSuspensionOverTls) {
    expect_a_stop_and_a_start_leave_the_suspension<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

template <typename Transport, typename Accept>
void
expect_a_session_suspended_in_its_constructor_starts_suspended() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open(false, 0, 0, true)) << "the session suspended in its constructor never came up (over TLS: its handshake)";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_TRUE(io.session()->reading_paused()) << "the start that followed the constructor lifted its suspension";

    ASSERT_TRUE(io.send(lines(0, 3)));
    settle();
    EXPECT_TRUE(g_probe.lines.empty()) << "a session suspended in its constructor was handed messages";
    EXPECT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);
    EXPECT_FALSE(io.session()->is_reading());

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 3u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(3));
}

TEST(ReadSuspension, ASessionSuspendedInItsConstructorStartsSuspendedOverTcp) {
    expect_a_session_suspended_in_its_constructor_starts_suspended<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASessionSuspendedInItsConstructorStartsSuspendedOverTls) {
    expect_a_session_suspended_in_its_constructor_starts_suspended<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- what a suspension retained is never left to wait for the peer ------------------------------------------------
// `resume_reading()` asks the loop for the dispatch that slices what the suspension retained; a `stop()` drops what
// was asked of the loop, and a watcher that is stopped can be asked nothing. Whatever the order of the resume around a
// `stop()` and a `start()`, the messages already received are delivered once the component is started and its reading
// resumed -- with nothing more on the wire: a peer that waits for its answer sends nothing.

enum class restart { resumed_after_the_restart, resumed_while_stopped, stopped_before_the_resumes_dispatch };

template <typename Transport, typename Accept>
void
expect_what_a_suspension_retained_is_delivered_after_a_restart(restart order) {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;

    ASSERT_TRUE(io.send(lines(0, 4))); // four lines in one read: three are retained
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    settle();
    ASSERT_EQ(g_probe.lines, delivered(1));
    ASSERT_NE(io.session(), nullptr);
    switch (order) {
        case restart::resumed_after_the_restart: // the suspension holds across the restart
            io.session()->stop();
            io.session()->start();
            settle();
            ASSERT_EQ(g_probe.lines.size(), 1u) << "a start delivered what a suspension retained";
            ASSERT_NE(io.session(), nullptr);
            ASSERT_TRUE(io.session()->reading_paused());
            io.session()->resume_reading();
            break;
        case restart::resumed_while_stopped: // nothing can be asked of the loop for a watcher that is stopped
            io.session()->stop();
            io.session()->resume_reading();
            io.session()->start();
            break;
        case restart::stopped_before_the_resumes_dispatch: // the stop drops the dispatch the resume asked for
            io.session()->resume_reading();
            io.session()->stop();
            io.session()->start();
            break;
    }
    EXPECT_FALSE(io.session()->reading_paused());
    EXPECT_EQ(g_probe.lines.size(), 1u) << "messages were handed to the protocol from inside the call";
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 4u; }, 2s))
        << "what the suspension retained waits for the peer's next octets: " << g_probe.lines.size() << " of 4 delivered";
    EXPECT_EQ(g_probe.lines, delivered(4));
    EXPECT_EQ(g_probe.disconnections, 0);

    // And the session reads on.
    ASSERT_TRUE(io.send(line(4)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 5u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(5));
}

TEST(ReadSuspension, AResumeAfterARestartDeliversWhatWasRetainedOverTcp) {
    expect_what_a_suspension_retained_is_delivered_after_a_restart<transport::tcp, transport::accept>(restart::resumed_after_the_restart);
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AResumeAfterARestartDeliversWhatWasRetainedOverTls) {
    expect_what_a_suspension_retained_is_delivered_after_a_restart<transport::stcp, transport::saccept>(restart::resumed_after_the_restart);
}
#endif // QB_HAS_SSL

TEST(ReadSuspension, AResumeMadeWhileStoppedIsHonouredByTheStartOverTcp) {
    expect_what_a_suspension_retained_is_delivered_after_a_restart<transport::tcp, transport::accept>(restart::resumed_while_stopped);
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AResumeMadeWhileStoppedIsHonouredByTheStartOverTls) {
    expect_what_a_suspension_retained_is_delivered_after_a_restart<transport::stcp, transport::saccept>(restart::resumed_while_stopped);
}
#endif // QB_HAS_SSL

TEST(ReadSuspension, AStopBetweenAResumeAndItsDispatchLosesNothingOverTcp) {
    expect_what_a_suspension_retained_is_delivered_after_a_restart<transport::tcp, transport::accept>(
        restart::stopped_before_the_resumes_dispatch);
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AStopBetweenAResumeAndItsDispatchLosesNothingOverTls) {
    expect_what_a_suspension_retained_is_delivered_after_a_restart<transport::stcp, transport::saccept>(
        restart::stopped_before_the_resumes_dispatch);
}
#endif // QB_HAS_SSL

// ---- over TLS, a suspension does not stop the handshake ----------------------------------------------------------

// TLS 1.2: the client's handshake ends on the server's Finished, which the server sends once it has READ the client's.
// TLS 1.3: the server's own handshake ends on the client's Finished, which it must read. Either way a session suspended
// as soon as it is accepted must still make the reads OpenSSL needs -- or the handshake waits for ever. What follows
// the handshake is the application's: retained.
#ifdef QB_HAS_SSL
void
expect_a_session_suspended_before_its_first_handshake_completes_it(int max_tls_version) {
    Link<transport::stcp, transport::saccept> io;
    ASSERT_TRUE(io.open(true, max_tls_version)) << "the handshake of a session suspended at its acceptance did not complete";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_TRUE(io.session()->reading_paused());

    ASSERT_TRUE(io.send(lines(0, 3)));
    settle();
    EXPECT_TRUE(g_probe.lines.empty()) << "a suspended session was handed messages";
    EXPECT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);
    EXPECT_FALSE(io.session()->is_reading()) << "the handshake over, a suspended session still watched readability";

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 3u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(3));
}

TEST(ReadSuspension, ASessionSuspendedBeforeItsFirstHandshakeCompletesItUnderTls12) {
    expect_a_session_suspended_before_its_first_handshake_completes_it(TLS1_2_VERSION);
}

TEST(ReadSuspension, ASessionSuspendedBeforeItsFirstHandshakeCompletesItUnderTls13) {
    expect_a_session_suspended_before_its_first_handshake_completes_it(0); // the library's highest
}
#endif // QB_HAS_SSL

// ---- what a TLS stream holds decrypted, under a suspension -------------------------------------------------------
// A record is decrypted whole: a read that takes less than the record -- a read cap below it -- leaves the rest inside
// the TLS session, where no readiness of the socket speaks for it, and the io layer dispatches the component again for
// those octets (`held_input()`, Huly QB-86). Not while its reading is suspended: each such dispatch would read them
// into a buffer nobody empties, up to the cap, and the next would end the connection there as an overflow.

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASuspendedSessionLeavesWhatItsTlsStreamHoldsAndReadsItAtTheResume) {
    constexpr std::size_t                     kCap   = 1024u;
    constexpr std::size_t                     kLines = 30u; // one record of 3000 octets
    Link<transport::stcp, transport::saccept> io;
    ASSERT_TRUE(io.open(false, 0, kCap));
    g_probe.suspend_at = 5;

    ASSERT_TRUE(io.send(long_lines(kLines)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 5u; }, 5s));
    settle(100ms);
    EXPECT_EQ(g_probe.lines.size(), 5u) << "a suspended session was handed messages";
    EXPECT_EQ(g_probe.disconnections, 0) << "a suspended session read what its TLS stream held up to its cap; reason " << g_probe.reason;
    ASSERT_NE(io.session(), nullptr);
    // One read of `kCap` octets took the head of the record: the rest is held, all of it -- a suspended session
    // dispatched for it would have read more into the room the delivered lines left.
    EXPECT_EQ(static_cast<std::size_t>(SSL_pending(io.session()->transport().ssl_handle())), kLines * 100u - kCap)
        << "a suspended session read what its TLS stream held";

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= kLines || g_probe.disconnections > 0; }, 5s))
        << "what the TLS session held was never read after the resume: " << g_probe.lines.size() << " of " << kLines;
    EXPECT_EQ(g_probe.lines.size(), kLines);
    EXPECT_EQ(misordered_long_lines(g_probe.lines), 0u);
    EXPECT_EQ(g_probe.disconnections, 0) << "disconnected with reason " << g_probe.reason;
}
#endif // QB_HAS_SSL

// ---- `input<>`: the other base that reads ---------------------------------------------------------------------------

/// What a reader saw; one per reader, on the test thread.
struct ReaderProbe {
    std::size_t              suspend_at            = 0; ///< the handler of that line (counted from 1) suspends reading
    std::size_t              suspend_and_end_at    = 0; ///< the handler of that line suspends, then clears the protocols
    std::size_t              suspend_and_resume_at = 0; ///< the handler of that line suspends, and resumes before it returns
    std::vector<std::string> lines;
    int                      disconnections = 0;
    int                      reason         = 0;
    int                      eofs           = 0; ///< `eof` events: read passes that left nothing
};

/// A reader on `input<>` alone over a stream it is handed in the open: the composition a user writes by hand.
template <typename Transport>
class LineReader
    : public async::input<LineReader<Transport>>
    , public Transport {
public:
    using Protocol = qb::protocol::text::command<LineReader>;

    ReaderProbe seen;

    LineReader() {
        static_cast<void>(this->template switch_protocol<Protocol>(*this));
    }

    ~LineReader() {
        this->_async_event.stop(); // before the transport, a base listed after the watcher's, closes the descriptor
    }

    void
    on(typename Protocol::message &&msg) {
        seen.lines.emplace_back(msg.text);
        if (seen.lines.size() == seen.suspend_at)
            this->pause_reading();
        if (seen.lines.size() == seen.suspend_and_end_at) {
            this->pause_reading();
            this->clear_protocols();
        }
        if (seen.lines.size() == seen.suspend_and_resume_at) {
            this->pause_reading();
            this->resume_reading();
        }
    }

    void
    on(async::event::eof &&) {
        ++seen.eofs;
    }

    void
    on(async::event::disconnected &&event) {
        ++seen.disconnections;
        seen.reason = event.reason;
    }

    /// What an object reused for another connection calls first (the protected reset, made reachable).
    void
    reset_state() {
        this->reset_io_state();
    }
};

TEST(ReadSuspension, AnInputComponentSuspendsAndResumesItsReadingOverTcp) {
    async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    qb::io::tcp::socket accepted;
    ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
    {
        LineReader<transport::tcp> reader;
        reader.transport()     = std::move(accepted);
        reader.seen.suspend_at = 3;
        reader.start();

        const std::string ten = lines(0, 10);
        ASSERT_EQ(client.write(ten.data(), ten.size()), static_cast<int>(ten.size()));
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 3u; }, 5s));
        settle();
        EXPECT_EQ(reader.seen.lines, delivered(3)) << "the message that suspended was not the last of its read pass";
        EXPECT_TRUE(reader.reading_paused());
        EXPECT_FALSE(reader.is_reading());
        reader.ready_to_read();
        EXPECT_FALSE(reader.is_reading()) << "ready_to_read() read again under a suspension";

        reader.resume_reading();
        EXPECT_EQ(reader.seen.lines.size(), 3u) << "resume_reading() handed messages to the protocol from inside the call";
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 10u; }, 5s)) << "what the suspension retained was never delivered";
        EXPECT_EQ(reader.seen.lines, delivered(10));
        EXPECT_TRUE(reader.is_reading());

        // The peer's end during a second suspension: seen at the resume, after what was retained, once.
        reader.seen.suspend_at = 11;
        const std::string more = lines(10, 14);
        ASSERT_EQ(client.write(more.data(), more.size()), static_cast<int>(more.size()));
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 11u; }, 5s));
        client.close();
        settle(100ms);
        EXPECT_EQ(reader.seen.disconnections, 0) << "a suspended reader learned of its peer's end";
        reader.resume_reading();
        EXPECT_TRUE(pump_until([&] { return reader.seen.disconnections > 0; }, 5s)) << "the peer's end was never seen after the resume";
        EXPECT_EQ(reader.seen.lines, delivered(14)) << "what the suspension retained was not delivered before the end";
        EXPECT_EQ(reader.seen.disconnections, 1);
    }
    async::listener::current.clear();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASuspendedInputComponentLeavesWhatItsTlsStreamHoldsAndReadsItAtTheResume) {
    constexpr std::size_t kCap   = 1024u;
    constexpr std::size_t kLines = 30u; // one record of 3000 octets
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    async::init();
    qb::io::tcp::ssl::listener listener{ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"))};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    qb::io::test::tls_pair pair;
    pair.client.set_insecure(); // self-signed local cert
    ASSERT_TRUE(qb::io::test::open_tls_connection(listener, listener.local_endpoint().port(), pair));
    ASSERT_TRUE(qb::io::test::complete_tls_handshake(pair));
    {
        LineReader<transport::stcp> reader;
        reader.transport() = std::move(pair.server);
        reader.set_max_read_buffer_size(kCap);
        reader.seen.suspend_at = 5;
        reader.start();

        const std::string record = long_lines(kLines);
        ASSERT_EQ(pair.client.write(record.data(), record.size()), static_cast<int>(record.size())); // one record
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 5u; }, 5s));
        settle(100ms);
        EXPECT_EQ(reader.seen.lines.size(), 5u) << "a suspended reader was handed messages";
        EXPECT_EQ(reader.seen.disconnections, 0) << "a suspended reader read what its TLS stream held up to its cap; reason "
                                                 << reader.seen.reason;
        // One read of `kCap` octets took the head of the record: the rest is held, all of it -- a suspended reader
        // dispatched for it would have read more into the room the delivered lines left.
        EXPECT_EQ(reader.held_input(), record.size() - kCap) << "a suspended reader read what its TLS stream held";

        reader.resume_reading();
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= kLines || reader.seen.disconnections > 0; }, 5s))
            << "what the TLS session held was never read after the resume: " << reader.seen.lines.size() << " of " << kLines;
        EXPECT_EQ(reader.seen.lines.size(), kLines);
        EXPECT_EQ(misordered_long_lines(reader.seen.lines), 0u);
        EXPECT_EQ(reader.seen.disconnections, 0) << "disconnected with reason " << reader.seen.reason;
    }
    static_cast<void>(pair.client.disconnect());
    async::listener::current.clear();
}
#endif // QB_HAS_SSL

TEST(ReadSuspension, AnInputComponentsSuspensionOutlivesAStopAndAStart) {
    async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    qb::io::tcp::socket accepted;
    ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
    {
        LineReader<transport::tcp> reader;
        reader.transport() = std::move(accepted);
        reader.pause_reading(); // before the first start(): it holds from that start
        reader.start();
        EXPECT_TRUE(reader.reading_paused()) << "a start lifted a suspension made before it";
        EXPECT_FALSE(reader.is_reading()) << "a start watched readability under a suspension";
        reader.resume_reading();
        EXPECT_TRUE(reader.is_reading());

        // What a suspension retained is delivered by the resume that follows a restart, with nothing more on the wire.
        reader.seen.suspend_at  = 1;
        const std::string three = lines(0, 3);
        ASSERT_EQ(client.write(three.data(), three.size()), static_cast<int>(three.size()));
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 1u; }, 5s));
        settle();
        ASSERT_EQ(reader.seen.lines, delivered(1));
        reader.stop();
        reader.start();
        EXPECT_TRUE(reader.reading_paused()) << "a start lifted a suspension it had not made";
        settle();
        EXPECT_EQ(reader.seen.lines.size(), 1u) << "a start delivered what a suspension retained";
        reader.resume_reading();
        EXPECT_EQ(reader.seen.lines.size(), 1u) << "messages were handed to the protocol from inside the call";
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 3u; }, 2s))
            << "what the suspension retained waits for the peer's next octets: " << reader.seen.lines.size() << " of 3 delivered";
        EXPECT_EQ(reader.seen.lines, delivered(3));

        // ...and when the resume was made while the reader was stopped.
        reader.seen.suspend_at = 4;
        const std::string more = lines(3, 6);
        ASSERT_EQ(client.write(more.data(), more.size()), static_cast<int>(more.size()));
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 4u; }, 5s));
        settle();
        ASSERT_EQ(reader.seen.lines, delivered(4));
        reader.stop();
        reader.resume_reading();
        reader.start();
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 6u; }, 2s))
            << "a resume made while stopped was lost: " << reader.seen.lines.size() << " of 6 delivered";
        EXPECT_EQ(reader.seen.lines, delivered(6));
        EXPECT_EQ(reader.seen.disconnections, 0);
    }
    async::listener::current.clear();
}

// ---- a transport another component takes over ----------------------------------------------------------------------
// The suspension is the component's, not the connection's: a transport taken from a suspended session
// (`extractSession()`, what a protocol upgrade does) is read by its new owner from its first start -- what the peer
// sent during the suspension included, which waited in the socket.

template <typename Transport, typename Accept>
void
expect_a_transport_taken_from_a_suspended_session_is_read_by_its_new_owner() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    ASSERT_NE(io.session(), nullptr);

    io.session()->pause_reading();
    ASSERT_TRUE(io.send(lines(0, 3)));
    settle();
    EXPECT_TRUE(g_probe.lines.empty()) << "a suspended session was handed messages";
    ASSERT_NE(io.session(), nullptr);

    auto taken = io.extract();
    ASSERT_TRUE(taken.second) << "the session's transport could not be extracted";
    {
        LineReader<Transport> reader;
        reader.transport() = std::move(taken.first);
        reader.start();
        EXPECT_FALSE(reader.reading_paused());
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 3u; }, 5s))
            << "the new owner of a transport taken from a suspended session never read what waited in the socket";
        EXPECT_EQ(reader.seen.lines, delivered(3));

        ASSERT_TRUE(io.send(lines(3, 5)));
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 5u; }, 5s));
        EXPECT_EQ(reader.seen.lines, delivered(5));
        EXPECT_EQ(reader.seen.disconnections, 0);
    }
    EXPECT_TRUE(g_probe.lines.empty()) << "the session whose transport was taken was handed messages";
    EXPECT_EQ(g_probe.disconnections, 0) << "the session whose transport was taken was told it disconnected";
}

TEST(ReadSuspension, WhatASuspendedSessionRetainedDoesNotFollowItsTransport) {
    // Only the IO object leaves: complete messages the suspension retained are in the session's own buffer, and stay
    // there. Whoever takes a transport from a suspended session takes the stream from the socket's next octet.
    Link<transport::tcp, transport::accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;
    ASSERT_TRUE(io.send(lines(0, 3))); // one read: line 0 delivered, lines 1 and 2 retained
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    settle();
    ASSERT_NE(io.session(), nullptr);
    ASSERT_NE(io.session()->in().size(), 0u) << "the case proves nothing: nothing was retained";

    auto taken = io.extract();
    ASSERT_TRUE(taken.second);
    {
        LineReader<transport::tcp> reader;
        reader.transport() = std::move(taken.first);
        reader.start();
        ASSERT_TRUE(io.send(lines(3, 5)));
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 2u; }, 5s));
        settle();
        const std::vector<std::string> next{"line 3", "line 4"};
        EXPECT_EQ(reader.seen.lines, next) << "the new owner was handed what the session had retained, or missed what came after";
    }
    EXPECT_EQ(g_probe.lines, delivered(1));
    EXPECT_EQ(g_probe.disconnections, 0);
}

TEST(ReadSuspension, ATransportTakenFromASuspendedSessionIsReadByItsNewOwnerOverTcp) {
    expect_a_transport_taken_from_a_suspended_session_is_read_by_its_new_owner<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ATransportTakenFromASuspendedSessionIsReadByItsNewOwnerOverTls) {
    expect_a_transport_taken_from_a_suspended_session_is_read_by_its_new_owner<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

TEST(ReadSuspension, AnInputComponentWhoseHandlerSuspendsAndResumesLeavesItsPassGoing) {
    async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    qb::io::tcp::socket accepted;
    ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
    {
        LineReader<transport::tcp> reader;
        reader.transport()                = std::move(accepted);
        reader.seen.suspend_and_resume_at = 2;
        reader.start();
        const std::string five = lines(0, 5);
        ASSERT_EQ(client.write(five.data(), five.size()), static_cast<int>(five.size()));
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines.size() >= 5u; }, 5s)) << "a pass whose handler suspended and resumed stopped";
        settle();
        EXPECT_EQ(reader.seen.lines, delivered(5));
        EXPECT_EQ(reader.seen.eofs, 1) << "the resume made inside the read pass asked the loop for a dispatch of its own";
        EXPECT_FALSE(reader.reading_paused());
        EXPECT_TRUE(reader.is_reading());
    }
    async::listener::current.clear();
}

TEST(ReadSuspension, AnInputComponentWhoseHandlerSuspendsAndEndsItsProtocolEnds) {
    async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    qb::io::tcp::socket accepted;
    ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
    {
        LineReader<transport::tcp> reader;
        reader.transport()             = std::move(accepted);
        reader.seen.suspend_and_end_at = 2;
        reader.start();
        const std::string five = lines(0, 5);
        ASSERT_EQ(client.write(five.data(), five.size()), static_cast<int>(five.size()));
        EXPECT_TRUE(pump_until([&] { return reader.seen.disconnections > 0; }, 3s))
            << "a reader whose handler suspended and ended its protocol was left alive, suspended, with no protocol";
        EXPECT_EQ(reader.seen.lines, delivered(2));
        EXPECT_EQ(reader.seen.disconnections, 1);
        EXPECT_FALSE(reader.reading_paused()) << "the connection ended: so did the suspension";
    }
    async::listener::current.clear();
}

// ---- a message that suspends and ends its protocol ends the session ------------------------------------------------

template <typename Transport, typename Accept>
void
expect_a_message_that_suspends_and_ends_its_protocol_ends_the_session() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_and_end_at = 2;

    ASSERT_TRUE(io.send(lines(0, 5)));
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 3s))
        << "a session whose handler suspended and ended its protocol was left alive, suspended, with no protocol";
    EXPECT_EQ(g_probe.reason, -1);
    EXPECT_EQ(g_probe.lines, delivered(2));
    EXPECT_EQ(g_probe.disconnections, 1);
}

TEST(ReadSuspension, AMessageThatSuspendsAndEndsItsProtocolEndsTheSessionOverTcp) {
    expect_a_message_that_suspends_and_ends_its_protocol_ends_the_session<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AMessageThatSuspendsAndEndsItsProtocolEndsTheSessionOverTls) {
    expect_a_message_that_suspends_and_ends_its_protocol_ends_the_session<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- a protocol switched in by the handler that suspends -----------------------------------------------------------

template <typename Transport, typename Accept>
void
expect_a_protocol_switched_in_by_the_handler_that_suspends_slices_the_rest_at_the_resume() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.switch_and_suspend_at = 2;

    ASSERT_TRUE(io.send(lines(0, 5)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 2u; }, 5s));
    settle();
    EXPECT_EQ(g_probe.lines, delivered(2)) << "the message that switched and suspended was not the last of its pass";
    EXPECT_EQ(g_probe.disconnections, 0) << "disconnected with reason " << g_probe.reason;
    ASSERT_NE(io.session(), nullptr);
    EXPECT_TRUE(io.session()->reading_paused());

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 5u; }, 5s)) << "the protocol switched in never sliced what was retained";
    EXPECT_EQ(g_probe.lines, delivered(5));
    EXPECT_EQ(g_probe.disconnections, 0);
}

TEST(ReadSuspension, AProtocolSwitchedInByTheHandlerThatSuspendsSlicesTheRestAtTheResumeOverTcp) {
    expect_a_protocol_switched_in_by_the_handler_that_suspends_slices_the_rest_at_the_resume<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, AProtocolSwitchedInByTheHandlerThatSuspendsSlicesTheRestAtTheResumeOverTls) {
    expect_a_protocol_switched_in_by_the_handler_that_suspends_slices_the_rest_at_the_resume<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- a suspension made inside the slice a resume asked for ---------------------------------------------------------

template <typename Transport, typename Accept>
void
expect_a_suspension_inside_the_resumed_slice_ends_it() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 3;

    ASSERT_TRUE(io.send(lines(0, 10)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 3u; }, 5s));
    settle();
    ASSERT_EQ(g_probe.lines, delivered(3));
    ASSERT_NE(io.session(), nullptr);

    g_probe.suspend_at = 6; // the slice the resume asks for suspends again, at line 6
    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 6u; }, 5s));
    settle();
    EXPECT_EQ(g_probe.lines, delivered(6)) << "the message that suspended inside the resumed slice was not the last of it";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_TRUE(io.session()->reading_paused());
    EXPECT_FALSE(io.session()->is_reading());

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 10u; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(10));
    EXPECT_EQ(g_probe.disconnections, 0);
}

TEST(ReadSuspension, ASuspensionInsideTheResumedSliceEndsItOverTcp) {
    expect_a_suspension_inside_the_resumed_slice_ends_it<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASuspensionInsideTheResumedSliceEndsItOverTls) {
    expect_a_suspension_inside_the_resumed_slice_ends_it<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- one request at a time -------------------------------------------------------------------------------------------
// The shape the two calls are for: every handler suspends the reading and answers, and the `eos` of that answer --
// it has left -- resumes. Five requests in one read are handled one by one, each after the answer before it was flushed.

template <typename Transport, typename Accept>
void
expect_one_request_at_a_time_each_answer_is_flushed_before_the_next_request() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.one_at_a_time = true;

    ASSERT_TRUE(io.send(lines(0, 5)));
    EXPECT_EQ(io.received(20), "ack\nack\nack\nack\nack\n") << "five requests were not each answered";
    EXPECT_TRUE(pump_until([] { return g_probe.eos >= 5; }, 5s));
    EXPECT_EQ(g_probe.lines, delivered(5));
    ASSERT_EQ(g_probe.eos_at_message.size(), 5u);
    for (std::size_t i = 0; i < g_probe.eos_at_message.size(); ++i)
        EXPECT_EQ(g_probe.eos_at_message[i] - g_probe.eos_at_message[0], static_cast<int>(i))
            << "request " << i << " was handled before the answer to the one before it had left";
    EXPECT_EQ(g_probe.disconnections, 0);
}

TEST(ReadSuspension, OneRequestAtATimeEachAnswerIsFlushedBeforeTheNextRequestOverTcp) {
    expect_one_request_at_a_time_each_answer_is_flushed_before_the_next_request<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, OneRequestAtATimeEachAnswerIsFlushedBeforeTheNextRequestOverTls) {
    expect_one_request_at_a_time_each_answer_is_flushed_before_the_next_request<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- close_after_deliver() with nothing queued ----------------------------------------------------------------------
// A close with nothing to deliver ends at the connection's next event -- the peer's end, here. A component whose
// reading is suspended watches nothing: the close must watch for that end itself, whichever of the two calls came first.

template <typename Transport, typename Accept>
void
expect_a_close_with_nothing_queued_under_a_suspension_ends_at_the_peers_end(bool close_first) {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    ASSERT_NE(io.session(), nullptr);

    if (close_first) {
        io.session()->close_after_deliver(); // nothing queued, outside any handler
        io.session()->pause_reading();
    } else {
        io.session()->pause_reading();
        io.session()->close_after_deliver();
    }
    settle();
    ASSERT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);

    io.end_of_stream(); // the peer gives up and closes
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 3s))
        << "a close with nothing queued never ended a session whose reading is suspended: nothing watched for its peer's end";
    EXPECT_EQ(g_probe.disconnections, 1);
    EXPECT_TRUE(g_probe.lines.empty());
}

TEST(ReadSuspension, ACloseWithNothingQueuedUnderASuspensionEndsAtThePeersEndOverTcp) {
    expect_a_close_with_nothing_queued_under_a_suspension_ends_at_the_peers_end<transport::tcp, transport::accept>(false);
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ACloseWithNothingQueuedUnderASuspensionEndsAtThePeersEndOverTls) {
    expect_a_close_with_nothing_queued_under_a_suspension_ends_at_the_peers_end<transport::stcp, transport::saccept>(false);
}
#endif // QB_HAS_SSL

TEST(ReadSuspension, ASuspensionAskedAfterACloseWithNothingQueuedLeavesItsWatchOverTcp) {
    expect_a_close_with_nothing_queued_under_a_suspension_ends_at_the_peers_end<transport::tcp, transport::accept>(true);
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASuspensionAskedAfterACloseWithNothingQueuedLeavesItsWatchOverTls) {
    expect_a_close_with_nothing_queued_under_a_suspension_ends_at_the_peers_end<transport::stcp, transport::saccept>(true);
}
#endif // QB_HAS_SSL

// ...and across a stop() and a start(), which leave a suspension: they leave the close what it watched as well -- the
// close made before the stop, or while the session was stopped.
enum class closed { before_the_stop, while_stopped };

template <typename Transport, typename Accept>
void
expect_a_close_under_a_suspension_keeps_its_watch_across_a_restart(closed when) {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    ASSERT_NE(io.session(), nullptr);

    io.session()->pause_reading();
    if (when == closed::before_the_stop) {
        io.session()->close_after_deliver();
        io.session()->stop();
        io.session()->start();
    } else {
        io.session()->stop();
        io.session()->close_after_deliver();
        io.session()->start();
    }
    settle();
    ASSERT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);

    io.end_of_stream();
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 3s))
        << "a close with nothing queued, restarted under a suspension, never saw its peer's end";
    EXPECT_EQ(g_probe.disconnections, 1);
}

TEST(ReadSuspension, ACloseUnderASuspensionKeepsItsWatchAcrossAStopAndAStartOverTcp) {
    expect_a_close_under_a_suspension_keeps_its_watch_across_a_restart<transport::tcp, transport::accept>(closed::before_the_stop);
}

TEST(ReadSuspension, ACloseMadeWhileStoppedUnderASuspensionWatchesForItsPeersEndOverTcp) {
    expect_a_close_under_a_suspension_keeps_its_watch_across_a_restart<transport::tcp, transport::accept>(closed::while_stopped);
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ACloseUnderASuspensionKeepsItsWatchAcrossAStopAndAStartOverTls) {
    expect_a_close_under_a_suspension_keeps_its_watch_across_a_restart<transport::stcp, transport::saccept>(closed::before_the_stop);
}

TEST(ReadSuspension, ACloseMadeWhileStoppedUnderASuspensionWatchesForItsPeersEndOverTls) {
    expect_a_close_under_a_suspension_keeps_its_watch_across_a_restart<transport::stcp, transport::saccept>(closed::while_stopped);
}
#endif // QB_HAS_SSL

template <typename Transport, typename Accept>
void
expect_a_lingering_close_under_a_suspension_keeps_its_read_across_a_restart() {
    using Protocol = typename Session<Transport, Accept>::Protocol;
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;

    ASSERT_TRUE(io.send(lines(0, 3)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    ASSERT_NE(io.session(), nullptr);
    *io.session() << "bye" << Protocol::end;
    io.session()->close_after_deliver();
    EXPECT_EQ(io.received(4), "bye\n");
    ASSERT_TRUE(pump_until([&] { return io.session() != nullptr && io.session()->lingering(); }, 5s))
        << "the case proves nothing: the session does not linger";
    io.session()->stop();
    io.session()->start();

    const auto asked = std::chrono::steady_clock::now();
    io.end_of_stream();
    EXPECT_TRUE(pump_until([] { return g_probe.disconnections > 0; }, 5s));
    EXPECT_LT(std::chrono::steady_clock::now() - asked, 1s)
        << "a lingering close restarted under a suspension stopped reading its peer: it waited for its bound";
    EXPECT_EQ(g_probe.disconnections, 1);
}

TEST(ReadSuspension, ALingeringCloseUnderASuspensionKeepsItsReadAcrossAStopAndAStartOverTcp) {
    expect_a_lingering_close_under_a_suspension_keeps_its_read_across_a_restart<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ALingeringCloseUnderASuspensionKeepsItsReadAcrossAStopAndAStartOverTls) {
    expect_a_lingering_close_under_a_suspension_keeps_its_read_across_a_restart<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- a suspension asked from the handler of an incomplete message ---------------------------------------------------

template <typename Transport, typename Accept>
void
expect_a_suspension_asked_from_pending_read_holds_what_follows() {
    Link<Transport, Accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at_pending = true;

    ASSERT_TRUE(io.send("line")); // incomplete: the read pass ends on `pending_read`, whose handler suspends
    EXPECT_TRUE(pump_until([] { return g_probe.pending_reads >= 1; }, 5s));
    ASSERT_TRUE(io.send(" 0\n" + lines(1, 3)));
    settle();
    EXPECT_TRUE(g_probe.lines.empty()) << "a session suspended from its `pending_read` handler was handed messages";
    EXPECT_EQ(g_probe.disconnections, 0);
    ASSERT_NE(io.session(), nullptr);
    EXPECT_TRUE(io.session()->reading_paused());

    io.session()->resume_reading();
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 3u; }, 5s)) << "what came after the suspension was never read";
    EXPECT_EQ(g_probe.lines, delivered(3));
}

TEST(ReadSuspension, ASuspensionAskedFromPendingReadHoldsWhatFollowsOverTcp) {
    expect_a_suspension_asked_from_pending_read_holds_what_follows<transport::tcp, transport::accept>();
}

#ifdef QB_HAS_SSL
TEST(ReadSuspension, ASuspensionAskedFromPendingReadHoldsWhatFollowsOverTls) {
    expect_a_suspension_asked_from_pending_read_holds_what_follows<transport::stcp, transport::saccept>();
}
#endif // QB_HAS_SSL

// ---- what the suspension is for ---------------------------------------------------------------------------------------
// A peer that keeps sending to a session whose reading is suspended is stopped by its own transport, and the session's
// buffer does not grow: the octets wait in the two sockets.

TEST(ReadSuspension, APeerThatKeepsSendingToASuspendedSessionIsStoppedByItsOwnTransport) {
    Link<transport::tcp, transport::accept> io;
    ASSERT_TRUE(io.open());
    g_probe.suspend_at = 1;

    ASSERT_TRUE(io.send(lines(0, 2)));
    EXPECT_TRUE(pump_until([] { return g_probe.lines.size() >= 1u; }, 5s));
    settle();
    ASSERT_NE(io.session(), nullptr);
    const std::size_t retained = io.session()->in().size();
    const std::string chunk(64u * 1024u, 'x');
    std::size_t       sent    = 0;
    bool              refused = false;
    for (int i = 0; i < 4096 && !refused; ++i) { // 256 MiB at most: far past what two socket buffers hold
        const int ret = io.peer().write(chunk.data(), chunk.size());
        if (ret > 0)
            sent += static_cast<std::size_t>(ret);
        else
            refused = true;
        async::run_for(1ms);
    }
    EXPECT_TRUE(refused) << "the peer of a suspended session was never stopped: it sent " << sent << " octets";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_EQ(io.session()->in().size(), retained) << "the buffer of a suspended session grew while its peer sent " << sent << " octets";
    EXPECT_EQ(g_probe.lines, delivered(1));
    EXPECT_EQ(g_probe.disconnections, 0);
}

// ---- a suspension ends with its connection -------------------------------------------------------------------------
// It lasts until the resume or the end of the connection it was made on: the dispose of a `disconnect()`,
// `reset_io_state()` and `reset_for_reconnect()` each end it, and the next connection of the same object reads.

/// A standalone client: the object a program uses for one connection after another. Its two resets are the
/// protected ones of `io<>`, made reachable.
class Dialer : public async::tcp::client<Dialer, transport::tcp> {
public:
    using Protocol = qb::protocol::text::command<Dialer>;

    std::vector<std::string> received;
    int                      disconnections          = 0;
    bool                     suspend_when_told       = false; ///< `on(disconnected)` asks for a suspension
    bool                     suspended_when_it_ended = false; ///< `reading_paused()` as that hook read it

    void
    on(Protocol::message &&msg) {
        received.emplace_back(msg.text);
    }

    void
    on(async::event::disconnected &&) {
        ++disconnections;
        suspended_when_it_ended = this->reading_paused();
        if (suspend_when_told)
            this->pause_reading(); // asked of a connection that has ended: it ends with it
    }

    void
    reset_state() {
        this->reset_io_state();
    }

    void
    reset_connection() {
        this->reset_for_reconnect();
    }
};

enum class ended { by_its_disconnect, by_its_disconnect_whose_hook_suspends, by_reset_io_state, by_reset_for_reconnect };

void
expect_a_suspension_does_not_outlive_its_connection(ended how) {
    async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    {
        Dialer dialer;
        ASSERT_EQ(dialer.transport().connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        qb::io::tcp::socket first;
        ASSERT_EQ(listener.accept(first), qb::io::SocketStatus::Done);
        dialer.start();
        dialer.pause_reading();
        ASSERT_TRUE(dialer.reading_paused());
        switch (how) {
            case ended::by_its_disconnect:
                dialer.disconnect();
                ASSERT_TRUE(pump_until([&] { return dialer.disconnections > 0; }, 5s));
                EXPECT_TRUE(dialer.suspended_when_it_ended) << "the hook that is told of the end no longer read what the reading was";
                break;
            case ended::by_its_disconnect_whose_hook_suspends:
                dialer.resume_reading(); // the connection ends with its reading NOT suspended...
                dialer.suspend_when_told = true;
                dialer.disconnect();
                ASSERT_TRUE(pump_until([&] { return dialer.disconnections > 0; }, 5s));
                EXPECT_FALSE(dialer.suspended_when_it_ended);
                dialer.suspend_when_told = false; // ...and its hook asked for one
                break;
            case ended::by_reset_io_state:
                dialer.stop();
                dialer.transport().close();
                dialer.reset_state();
                break;
            case ended::by_reset_for_reconnect:
                dialer.stop();
                dialer.transport().close();
                dialer.reset_connection();
                static_cast<void>(dialer.switch_protocol<Dialer::Protocol>(dialer));
                break;
        }
        EXPECT_FALSE(dialer.reading_paused()) << "a suspension outlived the connection it was made on";
        first.close();

        // The next connection of the same object reads.
        ASSERT_EQ(dialer.transport().connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        qb::io::tcp::socket second;
        ASSERT_EQ(listener.accept(second), qb::io::SocketStatus::Done);
        dialer.start();
        const std::string two = lines(0, 2);
        ASSERT_EQ(second.write(two.data(), two.size()), static_cast<int>(two.size()));
        EXPECT_TRUE(pump_until([&] { return dialer.received.size() >= 2u; }, 5s))
            << "the next connection of a client that had suspended its reading is deaf";
        EXPECT_EQ(dialer.received, delivered(2));
        second.close();
    }
    async::listener::current.clear();
}

TEST(ReadSuspension, ASuspensionEndsWithTheDisposeOfItsConnection) {
    expect_a_suspension_does_not_outlive_its_connection(ended::by_its_disconnect);
}

TEST(ReadSuspension, ASuspensionAskedFromTheDisconnectedHookEndsWithThatConnection) {
    expect_a_suspension_does_not_outlive_its_connection(ended::by_its_disconnect_whose_hook_suspends);
}

TEST(ReadSuspension, AResetOfTheIoStateEndsASuspension) {
    expect_a_suspension_does_not_outlive_its_connection(ended::by_reset_io_state);
}

TEST(ReadSuspension, AResetForReconnectEndsASuspension) {
    expect_a_suspension_does_not_outlive_its_connection(ended::by_reset_for_reconnect);
}

TEST(ReadSuspension, AResetOfAnInputComponentEndsItsSuspension) {
    async::init();
    {
        LineReader<transport::tcp> reader;
        reader.pause_reading(); // before any start(): held
        ASSERT_TRUE(reader.reading_paused());
        reader.reset_state();
        EXPECT_FALSE(reader.reading_paused()) << "a suspension outlived the reset of its component";
    }
    async::listener::current.clear();
}

// ---- a server's reading is its accepting ---------------------------------------------------------------------------
// An acceptor is an `input<>` whose read accepts one connection and whose message is that connection. Suspended, it
// accepts nothing: the connections complete in the listen backlog and wait there, and the resume accepts them.

TEST(ReadSuspension, AServerWhoseReadingIsSuspendedAcceptsNoConnectionUntilItResumes) {
    g_probe = Probe{};
    async::init();
    {
        Server<transport::tcp, transport::accept> server;
        ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        const auto port = server.transport().local_endpoint().port();
        server.start();
        server.pause_reading();
        EXPECT_TRUE(server.reading_paused());

        qb::io::tcp::socket first, second;
        ASSERT_EQ(first.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done); // completes in the backlog
        ASSERT_EQ(second.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        settle(100ms);
        EXPECT_EQ(server.session_count(), 0u) << "a server whose reading is suspended accepted a connection";

        server.resume_reading();
        EXPECT_TRUE(pump_until([&] { return server.session_count() == 2u; }, 5s))
            << "the connections that waited in the backlog were never accepted: " << server.session_count() << " of 2";
        first.close();
        second.close();
        EXPECT_TRUE(pump_until([&] { return server.session_count() == 0u; }, 5s));
    }
    async::listener::current.clear();
}

TEST(ReadSuspension, AServerThatSuspendsFromTheHookOfAnAcceptedConnectionTakesOneAtATime) {
    g_probe              = Probe{};
    g_probe.accept_limit = 1;
    async::init();
    {
        Server<transport::tcp, transport::accept> server;
        ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        const auto port = server.transport().local_endpoint().port();
        server.start();

        qb::io::tcp::socket first, second;
        ASSERT_EQ(first.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        ASSERT_EQ(second.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        EXPECT_TRUE(pump_until([&] { return server.session_count() >= 1u; }, 5s));
        settle(100ms);
        EXPECT_EQ(server.session_count(), 1u) << "a server that suspended its accepting at its first connection took the second";
        EXPECT_TRUE(server.reading_paused());

        // The first connection ends; the server takes the next one.
        first.close();
        EXPECT_TRUE(pump_until([&] { return server.session_count() == 0u; }, 5s));
        server.resume_reading();
        EXPECT_TRUE(pump_until([&] { return server.session_count() == 1u; }, 5s)) << "the connection that waited was never accepted";
        second.close();
        EXPECT_TRUE(pump_until([&] { return server.session_count() == 0u; }, 5s));
    }
    async::listener::current.clear();
}

// ---- not on a datagram component -----------------------------------------------------------------------------------

struct DatagramProbe : async::udp::client<DatagramProbe> {};

template <typename Component>
concept suspends_its_reading = requires(Component &component) {
    component.pause_reading();
    component.resume_reading();
};

static_assert(suspends_its_reading<Session<transport::tcp, transport::accept>>, "a stream session suspends and resumes its reading");
#ifdef QB_HAS_SSL
static_assert(suspends_its_reading<Session<transport::stcp, transport::saccept>>, "a TLS session suspends and resumes its reading");
#endif
static_assert(suspends_its_reading<LineReader<transport::tcp>>, "a reader on input<> suspends and resumes its reading");
static_assert(!suspends_its_reading<DatagramProbe>, "a datagram component must refuse pause_reading() at compile time");

} // namespace read_suspension_test
