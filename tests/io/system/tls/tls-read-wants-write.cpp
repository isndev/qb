/**
 * @file qb/tests/io/system/tls/tls-read-wants-write.cpp
 * @brief A TLS read that must WRITE before it can read goes on when the socket can take that write (Huly QB-1270).
 *
 * `SSL_read()` writes when a handshake message must answer what it read: a TLS 1.2 client answers its server's
 * HelloRequest with a ClientHello, a server that allows a client's renegotiation answers its ClientHello, a TLS 1.3
 * client answers a post-handshake CertificateRequest with its Certificate and Finished. When the socket cannot take
 * that write now, OpenSSL says so: `SSL_ERROR_WANT_WRITE` from a read, or `SSL_ERROR_SYSCALL` with an error the io
 * layer makes the call again after (`ENOBUFS`, `EINTR`). The peer usually waits for that message before it sends
 * anything more, so a read that waits for READABILITY waits for ever -- the connection stalls until a timeout --, and
 * a read that takes `ENOBUFS` for a reset ends a connection OpenSSL lets go on.
 *
 * The cases drive qb's io objects -- a standalone `io<>` client, a server session, an `input<>` and an `output<>`
 * object -- against a raw OpenSSL peer on the same thread. The socket refuses the read's write through a BIO that wraps
 * the socket BIO (`shared/tls_wrapping_bio.h`): once -- exactly what OpenSSL sees of a full send buffer, a write
 * answered "try again" with the retry flag set, or of a socket that fails one write, no retry flag --, the socket
 * itself writable; or for as long as a REAL full socket reports no room (`fill_send_buffer()`: a peer that reads none of
 * a fill larger than its window, the socket then held full -- `hold_full()`), where writability comes only when the
 * peer reads. What is measured is whether the io layer watches writability for such a read and reads again
 * when it comes, and readability for a write that waits for the peer.
 *
 * - **Answered**: the renegotiation completes and the data the peer sends after it is read and answered, within a
 *   bound sized for a loaded host (the mechanism takes a few loop passes).
 * - **Not a write the read makes**: a TLS 1.3 KeyUpdate the peer requests is answered by this side's next WRITE
 *   (OpenSSL's choice, RFC 8446 4.6.3: before its next application data), never by the read: measured, nothing to wait
 *   for there.
 * - **OpenSSL's state is the judge**: a socket error the read's write met is retried where OpenSSL keeps the session
 *   (every OpenSSL 3 measured, and OpenSSL 4.1.0 for a handshake write), and reported aborted where it ended it.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <poll.h>
#endif

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>
#include <qb/io/transport/stcp.h>
#include <qb/system/allocator/pipe.h>

#include "../../shared/close_after_peer_support.h"
#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_wrapping_bio.h"

using namespace std::chrono_literals;

namespace tls_read_wants_write_test {

using namespace qb::io::test::close_after_peer;

/// How long a case waits for what must happen: the mechanism takes a few loop passes, the bound a loaded host.
constexpr auto kBound = 5000ms;

/// An OpenSSL context of the test's own, freed with it.
struct RawContext {
    SSL_CTX *ctx = nullptr;

    enum class role { server, client };

    explicit RawContext(role r, int max_version = 0) {
        ctx = SSL_CTX_new(r == role::server ? TLS_server_method() : TLS_client_method());
        if (!ctx)
            return;
        if (max_version != 0)
            SSL_CTX_set_max_proto_version(ctx, max_version);
        if (r == role::server) {
            const bool ok = SSL_CTX_use_certificate_file(ctx, qb::io::test::ssl_resource_path("cert.pem").c_str(), SSL_FILETYPE_PEM) == 1
                            && SSL_CTX_use_PrivateKey_file(ctx, qb::io::test::ssl_resource_path("key.pem").c_str(), SSL_FILETYPE_PEM) == 1;
            if (!ok) {
                SSL_CTX_free(ctx);
                ctx = nullptr;
            }
        }
    }

    RawContext(RawContext const &)            = delete;
    RawContext &operator=(RawContext const &) = delete;

    ~RawContext() {
        SSL_CTX_free(ctx);
    }
};

/**
 * @brief The peer: OpenSSL over a plain TCP socket, non-blocking, pumped by the test's thread -- which is the loop's.
 * @details Every pump runs one loop pass, then one `SSL_read()`: what any OpenSSL program does with the connection, a
 *          handshake in progress included (OpenSSL drives it from the read).
 */
class RawPeer {
    RawContext::role _role;

public:
    qb::io::tcp::socket tcp;
    SSL                *ssl = nullptr;
    std::string         received;                ///< The application octets read so far.
    int                 failed = SSL_ERROR_NONE; ///< The first error a read met that was not "try again".

    explicit RawPeer(RawContext const &context, RawContext::role r)
        : _role(r)
        , ssl(context.ctx ? SSL_new(context.ctx) : nullptr) {}

    RawPeer(RawPeer const &)            = delete;
    RawPeer &operator=(RawPeer const &) = delete;

    ~RawPeer() {
        SSL_free(ssl);
        tcp.close();
    }

    /// Once `tcp` is connected: non-blocking, and the `SSL` object reads and writes it.
    void
    attach() {
        ASSERT_NE(ssl, nullptr);
        ASSERT_EQ(tcp.set_nonblocking(true), 0);
        ASSERT_EQ(SSL_set_fd(ssl, static_cast<int>(tcp.native_handle())), 1);
        if (_role == RawContext::role::server)
            SSL_set_accept_state(ssl);
        else
            SSL_set_connect_state(ssl);
    }

    /// One step of the handshake: 1 complete, 0 waiting for the other end, -1 failed.
    [[nodiscard]] int
    handshake_step() {
        ERR_clear_error();
        const int ret = SSL_do_handshake(ssl);
        if (ret == 1)
            return 1;
        const int error = SSL_get_error(ssl, ret);
        return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
    }

    /// One loop pass, then one read: the application octets kept, a handshake in progress driven.
    void
    pump_once() {
        qb::io::async::run(EVRUN_NOWAIT);
        char buffer[4096];
        ERR_clear_error();
        const int n = SSL_read(ssl, buffer, static_cast<int>(sizeof(buffer)));
        if (n > 0) {
            received.append(buffer, static_cast<std::size_t>(n));
            return;
        }
        const int error = SSL_get_error(ssl, n);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE && failed == SSL_ERROR_NONE)
            failed = error;
    }

    /// Pumps until `done()` or the bound: whether `done()` held.
    template <typename Pred>
    [[nodiscard]] bool
    pump_until(Pred &&done, std::chrono::milliseconds budget = kBound) {
        const auto deadline = steady::now() + budget;
        while (!done()) {
            if (steady::now() >= deadline)
                return false;
            pump_once();
            if (!done())
                std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /// Writes `what` whole, the loop running meanwhile.
    [[nodiscard]] bool
    write(std::string_view what, std::chrono::milliseconds budget = kBound) {
        const auto  deadline = steady::now() + budget;
        std::size_t sent     = 0;
        while (sent < what.size() && steady::now() < deadline) {
            ERR_clear_error();
            const int n = SSL_write(ssl, what.data() + sent, static_cast<int>(what.size() - sent));
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            const int error = SSL_get_error(ssl, n);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                return false;
            qb::io::async::run(EVRUN_NOWAIT);
            std::this_thread::sleep_for(1ms);
        }
        return sent == what.size();
    }
};

/// Whether the system reports `fd` writable now: what a loop that watches writability would be told.
bool
writable_now(::socket_type fd) {
#if defined(_WIN32)
    WSAPOLLFD p{};
    p.fd     = fd;
    p.events = POLLWRNORM;
    return ::WSAPoll(&p, 1, 0) == 1 && (p.revents & POLLWRNORM) != 0;
#else
    pollfd p{};
    p.fd     = fd;
    p.events = POLLOUT;
    return ::poll(&p, 1, 0) == 1 && (p.revents & POLLOUT) != 0;
#endif
}

/**
 * @brief Holds a socket whose send queue holds octets its peer has not taken full, in what the system reports: no room
 *        until its peer reads. Linux and Windows take a send buffer shrunk under what the socket holds (measured: no
 *        room reported). XNU takes the send low-water mark, capped at the buffer: room only once the send queue is
 *        empty (its socket code; asserted below on the macOS runner). Linux refuses that option.
 */
void
hold_full(qb::io::tcp::ssl::socket &socket) {
#if defined(__APPLE__)
    ASSERT_EQ(socket.set_optval(SOL_SOCKET, SO_SNDLOWAT, (std::numeric_limits<int>::max)()), 0);
#else
    ASSERT_EQ(socket.set_optval(SOL_SOCKET, SO_SNDBUF, 1), 0); // the system's least: under what the socket holds
#endif
    ASSERT_FALSE(writable_now(socket.native_handle())) << "the case proves nothing: the socket reports room";
}

/**
 * @brief Whether `socket`'s send queue holds octets its peer has not taken: a fill larger than the peer's receive buffer.
 *        XNU gives an accepted socket a receive buffer of its own (measured on the macOS runner: 324 984 octets with
 *        `kPeerWindow` set on the listener), and its peer took a `kFill` whole -- `SO_NWRITE` 0, the socket writable
 *        whatever its low-water mark. Linux and Windows keep the listener's window, which one `kFill` outgrows
 *        (measured): nothing is asked of them.
 */
bool
holds_unsent(qb::io::tcp::ssl::socket &socket) {
#if defined(__APPLE__)
    int       queued = 0;
    socklen_t length = sizeof(queued);
    return ::getsockopt(socket.native_handle(), SOL_SOCKET, SO_NWRITE, &queued, &length) == 0 && queued > 0;
#else
    static_cast<void>(socket);
    return true;
#endif
}

/**
 * @brief A BIO in front of the socket BIO an `SSL` object writes through: it refuses the next write once -- "try again",
 *        the retry flag set, which is what a socket BIO answers for a send buffer that is full (`EWOULDBLOCK`) --, or
 *        every write while the system reports the socket full (the loop's own view of it: a system may take a small
 *        write it reports no room for, Linux appends one to a segment not yet sent), or fails the next write once with
 *        a system error and no retry flag (what a socket BIO answers for `ENOBUFS`); and counts every write the `SSL`
 *        object attempts. It wraps the socket BIO, never chained onto it: OpenSSL before 3.5.6
 *        leaks the socket BIO of a chain (`shared/tls_wrapping_bio.h`).
 */
namespace refuse {
int         would_block = 0;     ///< How many next writes are refused "try again".
bool        while_full  = false; ///< Every write is refused "try again" while the system reports the socket full.
int         fail_with   = 0;     ///< The system error the next write fails with, once; 0: none.
std::size_t attempts    = 0;     ///< Writes attempted through the wrapper, refused or not.
int         half_once   = 0;     ///< How many next writes are taken in half: a record OpenSSL then holds unwritten.
bool        hold        = false; ///< Every write is refused "try again" -- set by a half-taken write, cleared by the case.

int
write(BIO *self, BIO *inner, const char *in, int size) {
    ++attempts;
    if (half_once > 0 && size > 1) {
        --half_once;
        hold = true;
        return qb::io::test::tls_wrapping_bio::forward_write(self, inner, in, size / 2);
    }
    if (hold) {
        BIO_set_retry_write(self);
        return -1;
    }
    if (while_full) {
        int fd = -1;
        if (BIO_get_fd(inner, &fd) >= 0 && !writable_now(static_cast<::socket_type>(fd))) {
            BIO_set_retry_write(self);
            return -1;
        }
    }
    if (would_block > 0) {
        --would_block;
        BIO_set_retry_write(self);
        return -1;
    }
    if (const int error = std::exchange(fail_with, 0)) {
        qb::io::socket::set_last_errno(error);
        return -1;
    }
    return qb::io::test::tls_wrapping_bio::forward_write(self, inner, in, size);
}

/// The wrapper in front of the socket BIO `ssl` writes through, from here on: the descriptor stays the socket's.
void
install(SSL *ssl) {
    qb::io::test::tls_wrapping_bio::wrapper<&refuse::write, &qb::io::test::tls_wrapping_bio::forward_read>::install(ssl, true);
    attempts = 0;
}
} // namespace refuse

/// Finished messages a raw peer read, counted by its message callback.
int g_finished_read = 0;

/// Whether OpenSSL ended `ssl`'s session: its error state, which `SSL_state_string()` names "SSLERR".
bool
in_error_state(SSL *ssl) {
    const char *state = SSL_state_string(ssl);
    return state && std::string_view(state) == "SSLERR";
}

/// The events an io object's watcher watches, spelled.
std::string
spell(int events) {
    std::string out;
    if (events & EV_READ)
        out += "EV_READ";
    if (events & EV_WRITE)
        out += out.empty() ? "EV_WRITE" : "|EV_WRITE";
    return out.empty() ? "nothing" : out;
}

/// A standalone TLS client: answers "pong" to every "ping".
/// `read_wants_write()`: the mark a read that must write first leaves. A qb without it (3.3, the witnesses' red run) has
/// no premise to hold there.
template <typename Socket>
bool
read_mark(Socket &socket) {
    if constexpr (requires { socket.read_wants_write(); })
        return socket.read_wants_write();
    else
        return true;
}

class PingClient : public qb::io::use<PingClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::text::command<PingClient>;

    std::size_t pings                 = 0;
    std::size_t disconnected          = 0;
    int         reason                = -99;
    int         system_error          = -99;
    bool        answer                = true;  ///< Whether a "ping" is answered.
    std::size_t answered_in_handshake = 0;     ///< Answers queued while a handshake after the first was under way.
    bool        arm_on_ping           = false; ///< Whether a "ping" makes an empty arm (`publish()` with no operand).
    bool        marked_at_arm         = false; ///< `read_wants_write()` when that arm was made: the read's mark.

    void
    on(Protocol::message &&message) {
        if (message.text == "ping") {
            ++pings;
            if (answer) {
                if (SSL_in_init(this->transport().ssl_handle()) == 1) // OpenSSL's own: the witness builds on qb 3.3 too
                    ++answered_in_handshake;
                *this << "pong\n";
            }
            if (arm_on_ping) {
                marked_at_arm = read_mark(this->transport());
                this->publish();
            }
        }
    }

    void
    on(qb::io::async::event::disconnected &&event) {
        ++disconnected;
        reason       = event.reason;
        system_error = event.error_code.value();
    }

    std::size_t pending_writes = 0; ///< `pending_write` events: a write pass that left output queued.
    void
    on(qb::io::async::event::pending_write &&) {
        ++pending_writes;
    }

    std::size_t eos = 0; ///< `eos` events: what was queued, or an empty arm, written.
    void
    on(qb::io::async::event::eos &&) {
        ++eos;
    }

    /// What the object's watcher watches now.
    [[nodiscard]] int
    watching() const noexcept {
        return this->_async_event.events;
    }
};

/// Loop passes a waiting case counts events over, and how many a connection with nothing to do may be dispatched in
/// them: none of its own -- a writable socket watched for a write that waits for the peer is dispatched on every pass.
constexpr int         kPasses     = 200;
constexpr std::size_t kIdleEvents = 2;

/// More than four TLS records (16 KiB each): what a write the read pass makes again cannot take in one call.
constexpr std::size_t kBulk = std::size_t{80} << 10;

/// The fill of a real send buffer, and the receive buffer of the peer that reads none of it: the fill outgrows it.
constexpr std::size_t kFill       = std::size_t{96} << 10; // under what a send buffer may hold (Linux: wmem_max)
constexpr std::size_t kFillBound  = std::size_t{8} << 20;  // what a fill may grow to on XNU (holds_unsent())
constexpr int         kPeerWindow = 8192;

/// A standalone object on `output<>` alone over a TLS socket: it writes what it is given.
class PushOutput : public qb::io::async::output<PushOutput> {
    qb::io::tcp::ssl::socket  _transport;
    qb::allocator::pipe<char> _out;

public:
    std::size_t disconnected = 0;

    qb::io::tcp::ssl::socket &
    transport() noexcept {
        return _transport;
    }
    qb::allocator::pipe<char> &
    out() noexcept {
        return _out;
    }
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out.size();
    }
    [[nodiscard]] std::size_t
    max_write_buffer_size() const noexcept {
        return static_cast<std::size_t>(-1);
    }
    int
    write() noexcept {
        if (_out.empty())
            return 0;
        const int ret = _transport.write(_out.begin(), _out.size());
        if (ret > 0)
            _out.free_front(static_cast<std::size_t>(ret));
        return ret;
    }
    void
    on(qb::io::async::event::disconnected &&) noexcept {
        ++disconnected;
    }
    [[nodiscard]] int
    watching() const noexcept {
        return this->_async_event.events;
    }
};

/// A standalone object on `input<>` alone over a TLS socket: it reads, counts the "ping" lines, and answers nothing.
class PingInput : public qb::io::async::input<PingInput> {
    qb::io::tcp::ssl::socket  _transport;
    qb::allocator::pipe<char> _in;

public:
    using Protocol = qb::protocol::text::command<PingInput>;

    std::size_t pings        = 0;
    std::size_t disconnected = 0;
    int         system_error = -99;

    qb::io::tcp::ssl::socket &
    transport() noexcept {
        return _transport;
    }
    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }
    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _in.size();
    }
    int
    read() noexcept {
        constexpr std::size_t chunk = 4096;
        auto *const           into  = _in.allocate_back(chunk);
        const int             ret   = _transport.read(into, chunk);
        _in.free_back(ret > 0 ? chunk - static_cast<std::size_t>(ret) : chunk);
        return ret;
    }
    void
    flush(std::size_t size) noexcept {
        _in.free_front(size);
    }
    void
    eof() noexcept {
        if (_in.empty())
            _in.reset();
        else
            _in.reorder();
    }

    void
    on(Protocol::message &&message) {
        if (message.text == "ping")
            ++pings;
    }
    void
    on(qb::io::async::event::disconnected &&event) noexcept {
        ++disconnected;
        system_error = event.error_code.value();
    }

    [[nodiscard]] int
    watching() const noexcept {
        return this->_async_event.events;
    }
};

// One server session per case: what it saw.
struct State {
    std::size_t pings          = 0;
    std::size_t disconnected   = 0;
    int         system_error   = -99;
    SSL        *ssl            = nullptr;
    int         refuse_first   = 0;     ///< The session's first writes refused "try again": its handshake flight.
    int         fail_first     = 0;     ///< ...or the first one failed once with this system error.
    bool        greet          = false; ///< The session queues a greeting before its handshake completed.
    std::size_t pending_writes = 0;     ///< `pending_write` events: a write pass that left output queued.
};
State *g_state = nullptr;

class PingServer;

class PingSession : public qb::io::use<PingSession>::tcp::ssl::client<PingServer> {
public:
    using Protocol = qb::protocol::text::command<PingSession>;

    explicit PingSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&message) {
        if (message.text == "ping") {
            ++g_state->pings;
            *this << "pong\n";
        }
    }
    void
    on(qb::io::async::event::disconnected &&event) {
        ++g_state->disconnected;
        g_state->system_error = event.error_code.value();
    }
    void
    on(qb::io::async::event::pending_write &&) {
        ++g_state->pending_writes;
    }
};

class PingServer : public qb::io::use<PingServer>::tcp::ssl::server<PingSession> {
public:
    void
    on(IOSession &session) {
        g_state->ssl = session.transport().ssl_handle();
        if (g_state->refuse_first != 0 || g_state->fail_first != 0) {
            refuse::install(g_state->ssl);
            refuse::would_block = g_state->refuse_first;
            refuse::fail_with   = g_state->fail_first;
        }
        if (g_state->greet)
            session << "hello\n";
    }
};

class TlsReadWantsWrite : public CleanLoop {
protected:
    State                       state;
    qb::io::tcp::listener       listener; ///< Plain TCP: the raw server peer accepts on it.
    std::uint16_t               port = 0;
    std::unique_ptr<PingServer> server;
    std::uint16_t               server_port = 0;
    std::size_t                 filled      = 0; ///< What the last fill wrote: `kFill`, or more on XNU (holds_unsent()).

    void
    SetUp() override {
        CleanLoop::SetUp();
        g_state             = &state;
        refuse::would_block = 0;
        refuse::while_full  = false;
        refuse::fail_with   = 0;
        refuse::attempts    = 0;
        refuse::half_once   = 0;
        refuse::hold        = false;
        ASSERT_TRUE(qb::io::test::require_ssl_files());
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        port = listener.local_endpoint().port();
        ASSERT_NE(port, 0u);
    }

    void
    TearDown() override {
        server.reset();
        listener.close();
        CleanLoop::TearDown();
        g_state = nullptr;
    }

    /// `client` connected to `peer`, a raw OpenSSL server, both handshakes complete; `before` runs on the client's
    /// `SSL` object before its first handshake message.
    template <typename Before>
    void
    connect_to(RawPeer &peer, qb::io::tcp::ssl::socket &client, Before &&before) {
        client.set_insecure();
        const int ret = client.n_connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
        const int err = qb::io::socket::get_last_errno();
        ASSERT_TRUE(ret == 0 || qb::io::socket_no_error(err)) << "n_connect: " << ret << ", error " << err;
        ASSERT_NE(client.ssl_handle(), nullptr);
        before(client.ssl_handle());
        ASSERT_EQ(listener.accept(peer.tcp), qb::io::SocketStatus::Done);
        ASSERT_NO_FATAL_FAILURE(peer.attach());
        const auto deadline = steady::now() + kBound;
        for (;;) {
            const int ours   = client.handshake_status();
            const int theirs = peer.handshake_step();
            ASSERT_GE(ours, 0) << "the client's handshake failed: " << client.get_last_ssl_error_string();
            ASSERT_GE(theirs, 0) << "the raw server's handshake failed";
            if (ours == 1 && theirs == 1)
                break;
            ASSERT_LT(steady::now(), deadline) << "the handshake did not complete";
            std::this_thread::sleep_for(1ms);
        }
    }

    void
    connect_to(RawPeer &peer, qb::io::tcp::ssl::socket &client) {
        connect_to(peer, client, [](SSL *) {});
    }

    /// qb's TLS server listens; `options` are set on its context first.
    void
    start_server(std::uint64_t options) {
        server = std::make_unique<PingServer>();
        server->transport().init(
            qb::io::ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem")));
        if (options != 0)
            server->transport().set_options(options);
        ASSERT_EQ(server->transport().listen_v4(0, "127.0.0.1"), 0);
        server_port = server->transport().local_endpoint().port();
        server->start();
    }

    /// `peer`, a raw OpenSSL client, connects to qb's TLS server and completes its handshake with the session.
    void
    connect_to_server(RawPeer &peer, std::uint64_t options) {
        ASSERT_NO_FATAL_FAILURE(start_server(options));
        ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", server_port), qb::io::SocketStatus::Done);
        ASSERT_NO_FATAL_FAILURE(peer.attach());
        const auto deadline = steady::now() + kBound;
        for (;;) {
            qb::io::async::run(EVRUN_NOWAIT);
            const int theirs = peer.handshake_step();
            ASSERT_GE(theirs, 0) << "the raw client's handshake failed";
            if (theirs == 1 && state.ssl != nullptr && SSL_is_init_finished(state.ssl))
                break;
            ASSERT_LT(steady::now(), deadline) << "the handshake did not complete";
            std::this_thread::sleep_for(1ms);
        }
    }

    /**
     * @brief A REAL full send buffer: `client`'s socket holds far more than it may queue, its peer reading nothing --
     *        written whole first (a large send buffer, the peer's window small: `kPeerWindow` on the listener before
     *        the connection; on XNU, whose accepted socket keeps a receive buffer of its own, `kFill` again until the
     *        send queue holds some: `filled`), then held full (`hold_full()`: its send buffer shrunk under what it
     *        holds, or on XNU its send low-water mark raised): the system reports no room until the peer reads
     *        (asserted). OpenSSL holds no record of its own and
     * the io object no output: the write path, which drives a handshake too, has nothing to make again -- the read alone answers the peer. The
     * read's own write is refused by the wrapper while the system reports no room (`refuse::while_full`): a system may still take a small write
     * it reports no room for (Linux appends it to a segment not yet sent, measured), and the loop goes by what the system reports.
     */
    void
    fill_send_buffer(PingClient &client) {
        ASSERT_EQ(client.transport().set_optval(SOL_SOCKET, SO_SNDBUF, 1 << 21), 0);
        filled = 0;
        do {
            client.publish(std::string(kFill, 'f'));
            filled += kFill;
            ASSERT_TRUE(pump_while_not([&] { return client.out().size() == 0u; }))
                << "the case proves nothing: the socket did not take the fill whole";
            pump(20ms);
        } while (!holds_unsent(client.transport()) && filled < kFillBound);
        ASSERT_NO_FATAL_FAILURE(hold_full(client.transport()));
    }

    /// The same through `socket` itself, before an io object owns it.
    void
    fill_directly(qb::io::tcp::ssl::socket &socket) {
        ASSERT_EQ(socket.set_optval(SOL_SOCKET, SO_SNDBUF, 1 << 21), 0);
        const std::string fill(kFill, 'f');
        filled = 0;
        do {
            std::size_t written  = 0;
            const auto  deadline = steady::now() + kBound;
            while (written < fill.size() && steady::now() < deadline) {
                const int n = socket.write(fill.data() + written, fill.size() - written);
                ASSERT_GE(n, 0) << "the fill failed";
                written += static_cast<std::size_t>(n);
            }
            ASSERT_EQ(written, fill.size()) << "the case proves nothing: the socket did not take the fill whole";
            filled += written;
            pump(20ms);
        } while (!holds_unsent(socket) && filled < kFillBound);
        ASSERT_NO_FATAL_FAILURE(hold_full(socket));
    }

    /**
     * @brief A read that waits to write into a REALLY full socket, input queued behind the peer's HelloRequest, while
     *        something else asks for writability too -- `before` the request, or `during` the wait: the loop's events
     *        over `kPasses` passes, then whether the renegotiation completes, the input is read and what was queued
     *        arrives, with one `eos`. OpenSSL reads nothing before the ClientHello leaves: readability watched while
     *        the read waits dispatched it on every pass.
     */
    template <typename Before, typename During>
    void
    read_waits_while_writability_is_asked(char const *what, Before &&before, During &&during) {
        ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
        RawContext context{RawContext::role::server, TLS1_2_VERSION};
        RawPeer    peer{context, RawContext::role::server};
        PingClient client;
        client.answer = false;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        client.start();
        pump(20ms);
        ASSERT_NO_FATAL_FAILURE(fill_send_buffer(client));
        ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
        refuse::while_full = true;
        before(client);
        pump(20ms);
        ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
        ERR_clear_error();
        ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
        ASSERT_TRUE(peer.write("ping\n")) << "the peer could not send behind its HelloRequest";
        pump(50ms);
        ASSERT_EQ(client.disconnected, 0u) << "the client ended the connection (system error " << client.system_error << ")";
        ASSERT_EQ(SSL_in_init(client.transport().ssl_handle()), 1) << "the case proves nothing: the client did not answer the HelloRequest";
        ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
        ASSERT_FALSE(writable_now(client.transport().native_handle())) << "the case proves nothing: the socket reports room";
        ASSERT_EQ(client.pings, 0u) << "the case proves nothing: the input behind the HelloRequest was read";
        const std::size_t eos = client.eos;
        during(client);
        const std::size_t events = events_in(kPasses);
        const std::size_t queued = client.out().size();
        std::cout << "[ MEASURED ] a read waiting to write into a full socket, input queued, " << what << " ("
                  << OpenSSL_version(OPENSSL_VERSION) << "): " << events << " events in " << kPasses << " loop passes, watching "
                  << spell(client.watching()) << ", queued " << queued << '\n';
        EXPECT_LE(events, kIdleEvents) << "the loop was woken while the read waited for the socket to take its write";
        const bool done = peer.pump_until([&] {
            return (client.pings == 1u && peer.received.size() >= filled + queued && SSL_renegotiate_pending(peer.ssl) == 0)
                   || peer.failed != SSL_ERROR_NONE || client.disconnected != 0u;
        });
        pump(20ms);
        std::cout << "[ MEASURED ] ...then " << (done ? "" : "NOT ") << "done: pings " << client.pings << ", the peer read "
                  << peer.received.size() << ", renegotiation pending " << SSL_renegotiate_pending(peer.ssl) << ", eos " << client.eos - eos
                  << ", disconnected " << client.disconnected << '\n';
        EXPECT_TRUE(done) << "the renegotiation, the input or the output never got through";
        EXPECT_EQ(client.disconnected, 0u);
        EXPECT_EQ(peer.received.size(), filled + queued) << "what was queued did not arrive whole";
        EXPECT_EQ(client.eos, eos + 1) << "what was queued, or the empty arm, was not answered by one eos";
    }

    /// The raw server asks `client` (a standalone io object over TLS 1.2) for a renegotiation -- its HelloRequest --
    /// while the client's socket refuses the next write as `refuse_how` says, then sends "ping": whether the
    /// renegotiation completed and the ping was read, with what was measured printed.
    template <typename Client>
    void
    renegotiation_answered(Client &client, RawPeer &peer, std::string_view label, std::function<void()> refuse_how) {
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        ASSERT_EQ(SSL_version(client.transport().ssl_handle()), TLS1_2_VERSION) << "the case proves nothing: no renegotiation after TLS 1.2";
        client.start();
        pump(20ms);
        ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
        refuse_how();
        ASSERT_EQ(SSL_renegotiate(peer.ssl), 1) << "the peer could not ask for a renegotiation";
        ERR_clear_error();
        ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent"; // its ClientHello awaited
        const bool renegotiated = peer.pump_until([&] { return SSL_renegotiate_pending(peer.ssl) == 0 || peer.failed != SSL_ERROR_NONE; });
        std::cout << "[ MEASURED ] " << label << " (" << OpenSSL_version(OPENSSL_VERSION)
                  << "): " << (renegotiated && peer.failed == SSL_ERROR_NONE ? "renegotiated" : "NOT renegotiated") << " within "
                  << kBound.count() << " ms; client writes attempted " << refuse::attempts << ", refusals left " << refuse::would_block
                  << ", failure left " << refuse::fail_with << ", disconnected " << client.disconnected << " (system error "
                  << client.system_error << "), watching " << spell(client.watching()) << ", client in init "
                  << SSL_in_init(client.transport().ssl_handle()) << ", want write " << SSL_want_write(client.transport().ssl_handle())
                  << ", OpenSSL error state " << in_error_state(client.transport().ssl_handle()) << ", peer failure " << peer.failed << '\n';
        ASSERT_EQ(refuse::would_block, 0) << "the case proves nothing: the read wrote no ClientHello";
        ASSERT_EQ(refuse::fail_with, 0) << "the case proves nothing: the read wrote no ClientHello";
        ASSERT_TRUE(renegotiated) << "the renegotiation never completed: the client's ClientHello never left";
        ASSERT_EQ(peer.failed, SSL_ERROR_NONE) << "the peer's read failed";
        EXPECT_EQ(client.disconnected, 0u) << "the client ended a connection OpenSSL keeps (system error " << client.system_error << ")";
        if constexpr (requires { client.eos; }) {
            EXPECT_EQ(client.eos, 0u) << "an eos nobody asked for: the writability was the read's";
        }
        ASSERT_TRUE(peer.write("ping\n"));
        EXPECT_TRUE(peer.pump_until([&] { return client.pings == 1u; })) << "the data after the renegotiation was never read";
    }
};

} // namespace tls_read_wants_write_test

using namespace tls_read_wants_write_test;

// The witness: a full send buffer refuses the ClientHello the client's read writes to answer its server's HelloRequest.
TEST_F(TlsReadWantsWrite, AClientAnswersARenegotiationItsSocketCouldNotTakeAtOnce) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    renegotiation_answered(client, peer, "io<> client, the ClientHello refused once (try again)", [] { refuse::would_block = 1; });
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the client's answer never arrived: " << peer.received;
}

// ...the same write failed once with ENOBUFS (no retry flag: SSL_ERROR_SYSCALL), which the io layer writes again after.
TEST_F(TlsReadWantsWrite, AClientAnswersARenegotiationWhoseClientHelloMetEnobufs) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    renegotiation_answered(client, peer, "io<> client, the ClientHello failed once with ENOBUFS", [] { refuse::fail_with = ENOBUFS; });
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the client's answer never arrived: " << peer.received;
}

// ...and with EINTR.
TEST_F(TlsReadWantsWrite, AClientAnswersARenegotiationWhoseClientHelloMetEintr) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    renegotiation_answered(client, peer, "io<> client, the ClientHello failed once with EINTR", [] { refuse::fail_with = EINTR; });
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the client's answer never arrived: " << peer.received;
}

// An input<> object, which never writes anything of its own: its read writes the ClientHello all the same.
TEST_F(TlsReadWantsWrite, AnInputObjectAnswersARenegotiationItsSocketCouldNotTakeAtOnce) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingInput  input;
    input.switch_protocol<PingInput::Protocol>(input);
    renegotiation_answered(input, peer, "input<> object, the ClientHello refused once (try again)", [] { refuse::would_block = 1; });
}

// A server session whose context allows a client's renegotiation: its read answers the ClientHello.
TEST_F(TlsReadWantsWrite, ASessionAnswersAClientsRenegotiationItsSocketCouldNotTakeAtOnce) {
    RawContext context{RawContext::role::client, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer, SSL_OP_ALLOW_CLIENT_RENEGOTIATION));
    ASSERT_EQ(SSL_version(state.ssl), TLS1_2_VERSION);
    ASSERT_NO_FATAL_FAILURE(refuse::install(state.ssl));
    refuse::would_block = 1;
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    static_cast<void>(SSL_do_handshake(peer.ssl)); // its ClientHello leaves; the ServerHello awaited
    const bool renegotiated = peer.pump_until([&] { return SSL_renegotiate_pending(peer.ssl) == 0 || peer.failed != SSL_ERROR_NONE; });
    std::cout << "[ MEASURED ] session, the ServerHello refused once (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << (renegotiated && peer.failed == SSL_ERROR_NONE ? "renegotiated" : "NOT renegotiated") << "; session writes attempted "
              << refuse::attempts << ", refusals left " << refuse::would_block << ", disconnected " << state.disconnected
              << ", session in init " << SSL_in_init(state.ssl) << ", want write " << SSL_want_write(state.ssl) << '\n';
    ASSERT_EQ(refuse::would_block, 0) << "the case proves nothing: the session's read wrote nothing";
    ASSERT_TRUE(renegotiated) << "the renegotiation never completed: the session's answer never left";
    ASSERT_TRUE(peer.write("ping\n"));
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the session's answer never arrived";
}

// TLS 1.3: a server that asks for the client's certificate after the handshake. The client's read answers its
// CertificateRequest with its Certificate and Finished.
TEST_F(TlsReadWantsWrite, AClientAnswersAPostHandshakeCertificateRequestItsSocketCouldNotTakeAtOnce) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport(), [](SSL *ssl) { SSL_set_post_handshake_auth(ssl, 1); }));
    ASSERT_EQ(SSL_version(client.transport().ssl_handle()), TLS1_3_VERSION);
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::would_block = 1;
    SSL_set_verify(peer.ssl, SSL_VERIFY_PEER, [](int, X509_STORE_CTX *) { return 1; });
    // The client's Finished, as the peer reads it: the end of its answer.
    g_finished_read = 0;
    SSL_set_msg_callback(peer.ssl, [](int write_p, int, int content_type, const void *buf, std::size_t len, SSL *, void *) {
        if (!write_p && content_type == SSL3_RT_HANDSHAKE && len > 0 && static_cast<const unsigned char *>(buf)[0] == SSL3_MT_FINISHED)
            ++g_finished_read;
    });
    ASSERT_EQ(SSL_verify_client_post_handshake(peer.ssl), 1) << "the peer could not ask for the client's certificate";
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's CertificateRequest was not sent"; // alone; the answer awaited
    const bool answered = peer.pump_until([&] { return g_finished_read > 0 || peer.failed != SSL_ERROR_NONE; });
    std::cout << "[ MEASURED ] io<> client, its post-handshake Certificate refused once (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << (answered && peer.failed == SSL_ERROR_NONE ? "answered" : "NOT answered") << " within " << kBound.count()
              << " ms; writes attempted " << refuse::attempts << ", refusals left " << refuse::would_block << ", watching "
              << spell(client.watching()) << ", client want write " << SSL_want_write(client.transport().ssl_handle()) << ", peer failure "
              << peer.failed << ", disconnected " << client.disconnected << '\n';
    ASSERT_EQ(refuse::would_block, 0) << "the case proves nothing: the read wrote nothing";
    ASSERT_TRUE(answered) << "the client's Certificate never left";
    ASSERT_EQ(peer.failed, SSL_ERROR_NONE) << "the peer's read failed";
    ASSERT_TRUE(peer.write("ping\n"));
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the client's answer never arrived: " << peer.received;
}

// TLS 1.3: a KeyUpdate the peer requests is answered by this side's next WRITE, never by its read. Measured: the read
// that takes the request and the data behind it writes nothing.
TEST_F(TlsReadWantsWrite, AKeyUpdateRequestIsAnsweredByTheNextWriteNotByTheRead) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    client.answer = false;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    ASSERT_EQ(SSL_version(client.transport().ssl_handle()), TLS1_3_VERSION);
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    ASSERT_EQ(SSL_key_update(peer.ssl, SSL_KEY_UPDATE_REQUESTED), 1);
    ASSERT_TRUE(peer.write("ping\n")); // the request leaves with it
    ASSERT_TRUE(pump_while_not([&] { return client.pings == 1u; })) << "the client never read the data behind the request";
    pump(20ms);
    std::cout << "[ MEASURED ] KeyUpdate requested (" << OpenSSL_version(OPENSSL_VERSION) << "): the reads wrote " << refuse::attempts
              << " records\n";
    EXPECT_EQ(refuse::attempts, 0u) << "a read wrote: the KeyUpdate answer is the next write's";
    refuse::would_block = 1; // the answer and the data: refused once, written again by the io layer's write path
    client << "pong\n";
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the client's write never arrived";
    EXPECT_GE(refuse::attempts, 2u) << "the write was not made again";
    EXPECT_EQ(client.disconnected, 0u);
}

// A server session drives its own handshake from its reads: its flight -- ServerHello to Finished -- is a read's write
// too, and the socket may not take it at once.
TEST_F(TlsReadWantsWrite, ASessionCompletesItsHandshakeWhenItsSocketCouldNotTakeItsFlightAtOnce) {
    state.refuse_first = 1;
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(start_server(0));
    ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", server_port), qb::io::SocketStatus::Done);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    int        theirs   = 0;
    const auto deadline = steady::now() + kBound;
    while ((theirs = peer.handshake_step()) == 0 && steady::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
    std::cout << "[ MEASURED ] session, its handshake flight refused once (" << OpenSSL_version(OPENSSL_VERSION) << "): the peer's handshake "
              << (theirs == 1   ? "completed"
                  : theirs == 0 ? "NOT completed"
                                : "FAILED")
              << " within " << kBound.count() << " ms; session writes attempted " << refuse::attempts << ", refusals left "
              << refuse::would_block << ", disconnected " << state.disconnected << '\n';
    ASSERT_EQ(refuse::would_block, 0) << "the case proves nothing: the session wrote no flight";
    ASSERT_EQ(theirs, 1) << "the session's handshake flight never left";
    ASSERT_TRUE(peer.write("ping\n"));
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the session's answer never arrived";
}

// The mirror: a write that waits for the PEER -- SSL_write() in a renegotiation the peer has not answered yet -- with the
// socket writable all along. Watching writability then wakes the loop on every pass until the peer answers; the write
// waits for readability, and the read that takes the peer's message makes it again.
TEST_F(TlsReadWantsWrite, AWriteThatWaitsForTheRenegotiationDoesNotWakeTheLoop) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1);
    ASSERT_TRUE(pump_while_not([&] { return SSL_in_init(client.transport().ssl_handle()) == 1; }))
        << "the client never answered the HelloRequest";
    pump(20ms);
    client << "data\n"; // OpenSSL must read the ServerHello before it writes anything
    const std::size_t events = events_in(kPasses);
    std::cout << "[ MEASURED ] a write waiting for the ServerHello (" << OpenSSL_version(OPENSSL_VERSION) << "): " << events << " events in "
              << kPasses << " loop passes, " << client.pending_writes << " pending_write, watching " << spell(client.watching()) << ", queued "
              << client.out().size() << '\n';
    ASSERT_GT(client.out().size(), 0u) << "the case proves nothing: the write did not wait";
    EXPECT_LE(events, kIdleEvents) << "the loop was woken while the write waited for the peer";
    EXPECT_TRUE(peer.pump_until([&] { return SSL_renegotiate_pending(peer.ssl) == 0 && peer.received == "data\n"; }))
        << "the write never completed: " << peer.received;
}

// ...and output queued past one TLS record: the write the read pass makes again takes one record (OpenSSL's partial
// writes), and the rest waits for writability again -- which the write that waited for the peer had stopped watching.
TEST_F(TlsReadWantsWrite, AWriteThatWaitedForTheRenegotiationDeliversEverythingQueued) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1);
    ASSERT_TRUE(pump_while_not([&] { return SSL_in_init(client.transport().ssl_handle()) == 1; }))
        << "the client never answered the HelloRequest";
    pump(20ms);
    const std::string bulk(kBulk, 'b');
    client << bulk; // four records and more: OpenSSL must read the ServerHello before it writes any
    pump(20ms);
    ASSERT_EQ(client.out().size(), bulk.size()) << "the case proves nothing: something was written before the renegotiation";
    const bool all = peer.pump_until([&] { return SSL_renegotiate_pending(peer.ssl) == 0 && peer.received.size() >= bulk.size(); });
    std::cout << "[ MEASURED ] " << bulk.size() << " octets queued while the write waited for the ServerHello ("
              << OpenSSL_version(OPENSSL_VERSION) << "): the peer read " << peer.received.size() << ", queued " << client.out().size()
              << ", watching " << spell(client.watching()) << '\n';
    EXPECT_TRUE(all) << "what the write left queued after the renegotiation was never written";
    EXPECT_EQ(peer.received, bulk);
}

// ...and a greeting a session queues before its handshake completed: it waits to read the client's Finished.
TEST_F(TlsReadWantsWrite, AGreetingQueuedBeforeTheHandshakeCompletedDoesNotWakeTheLoop) {
    state.greet = true;
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(start_server(0));
    ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", server_port), qb::io::SocketStatus::Done);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    ASSERT_EQ(peer.handshake_step(), 0); // the ClientHello leaves
    pump(20ms);                          // the session answers it, and waits for the client's Finished
    ASSERT_NE(state.ssl, nullptr);
    ASSERT_EQ(SSL_is_init_finished(state.ssl), 0) << "the case proves nothing: the session's handshake is over";
    const std::size_t events = events_in(kPasses);
    std::cout << "[ MEASURED ] a greeting waiting for the client's Finished (" << OpenSSL_version(OPENSSL_VERSION) << "): " << events
              << " events in " << kPasses << " loop passes, " << state.pending_writes << " pending_write\n";
    EXPECT_LE(events, kIdleEvents) << "the loop was woken while the greeting waited for the peer";
    int        theirs   = 0;
    const auto deadline = steady::now() + kBound;
    while ((theirs = peer.handshake_step()) == 0 && steady::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(theirs, 1);
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "hello\n"; })) << "the greeting never arrived";
}

// ...and an object on output<> alone, started on a server-side socket whose handshake has not completed: its write
// drives the handshake and waits for the client's Finished.
TEST_F(TlsReadWantsWrite, AnOutputObjectWhoseWriteWaitsForThePeerDoesNotWakeTheLoop) {
    RawContext                 context{RawContext::role::client};
    RawPeer                    peer{context, RawContext::role::client};
    qb::io::tcp::ssl::listener tls_listener;
    tls_listener.init(qb::io::ssl::create_server_context(TLS_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                         qb::io::test::ssl_resource_path("key.pem")));
    ASSERT_EQ(tls_listener.listen_v4(0, "127.0.0.1"), 0);
    ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", tls_listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    PushOutput output;
    ASSERT_EQ(tls_listener.accept(output.transport()), 0);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    ASSERT_EQ(peer.handshake_step(), 0); // the ClientHello leaves
    output.publish("hello\n");
    output.start(); // its write drives the server's handshake: the flight leaves, the client's Finished is awaited
    pump(20ms);
    ASSERT_EQ(SSL_is_init_finished(output.transport().ssl_handle()), 0) << "the case proves nothing: the handshake is over";
    const std::size_t events = events_in(kPasses);
    std::cout << "[ MEASURED ] an output<> write waiting for the client's Finished (" << OpenSSL_version(OPENSSL_VERSION) << "): " << events
              << " events in " << kPasses << " loop passes, watching " << spell(output.watching()) << '\n';
    EXPECT_LE(events, kIdleEvents) << "the loop was woken while the write waited for the peer";
    int        theirs   = 0;
    const auto deadline = steady::now() + kBound;
    while ((theirs = peer.handshake_step()) == 0 && steady::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(theirs, 1);
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "hello\n"; })) << "the output never arrived";
    EXPECT_EQ(output.disconnected, 0u);
    tls_listener.close();
}

// ...and the same object whose handshake flight the socket refuses once: that write waits for room in the socket, not
// for the peer -- watching readability for it would wait for ever, the peer waiting for the flight.
TEST_F(TlsReadWantsWrite, AnOutputObjectCompletesAHandshakeWhoseFlightItsSocketCouldNotTakeAtOnce) {
    RawContext                 context{RawContext::role::client};
    RawPeer                    peer{context, RawContext::role::client};
    qb::io::tcp::ssl::listener tls_listener;
    tls_listener.init(qb::io::ssl::create_server_context(TLS_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                         qb::io::test::ssl_resource_path("key.pem")));
    ASSERT_EQ(tls_listener.listen_v4(0, "127.0.0.1"), 0);
    ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", tls_listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    PushOutput output;
    ASSERT_EQ(tls_listener.accept(output.transport()), 0);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    ASSERT_NO_FATAL_FAILURE(refuse::install(output.transport().ssl_handle()));
    refuse::would_block = 1;             // the flight its write drives
    ASSERT_EQ(peer.handshake_step(), 0); // the ClientHello leaves
    output.publish("hello\n");
    output.start();
    int        theirs   = 0;
    const auto deadline = steady::now() + kBound;
    while ((theirs = peer.handshake_step()) == 0 && steady::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
    std::cout << "[ MEASURED ] an output<> handshake flight refused once (" << OpenSSL_version(OPENSSL_VERSION) << "): the peer's handshake "
              << (theirs == 1   ? "completed"
                  : theirs == 0 ? "NOT completed"
                                : "FAILED")
              << "; writes attempted " << refuse::attempts << ", refusals left " << refuse::would_block << ", watching "
              << spell(output.watching()) << '\n';
    ASSERT_EQ(refuse::would_block, 0) << "the case proves nothing: the write drove no flight";
    ASSERT_EQ(theirs, 1) << "the handshake flight never left";
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "hello\n"; })) << "the output never arrived";
    EXPECT_EQ(output.disconnected, 0u);
    tls_listener.close();
}

// The stall, on a real socket: the ClientHello waits for room the peer makes only by reading; once it has read, the
// peer waits for the ClientHello and sends nothing -- readability never comes.
TEST_F(TlsReadWantsWrite, AClientAnswersARenegotiationOnceItsFullSocketDrains) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0); // the peer's window, inherited at accept
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(fill_send_buffer(client));
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::while_full = true; // the ClientHello: refused while the socket reports no room, until the peer reads
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    pump(50ms); // the client reads it, and the socket refuses its ClientHello
    SSL *const ssl = client.transport().ssl_handle();
    std::cout << "[ MEASURED ] a real full send buffer (" << OpenSSL_version(OPENSSL_VERSION) << "): client in init " << SSL_in_init(ssl)
              << ", want write " << SSL_want_write(ssl) << ", watching " << spell(client.watching()) << ", disconnected " << client.disconnected
              << '\n';
    ASSERT_EQ(client.disconnected, 0u) << "the client ended the connection (system error " << client.system_error << ")";
    ASSERT_EQ(SSL_in_init(ssl), 1) << "the case proves nothing: the client did not answer the HelloRequest";
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
    ASSERT_FALSE(writable_now(client.transport().native_handle())) << "the case proves nothing: the socket reports room";
    // The peer reads everything it was sent -- the socket can take the ClientHello again -- and waits for it.
    const bool renegotiated = peer.pump_until(
        [&] { return (peer.received.size() >= filled && SSL_renegotiate_pending(peer.ssl) == 0) || peer.failed != SSL_ERROR_NONE; });
    std::cout << "[ MEASURED ] ...the peer read " << peer.received.size()
              << " octets: " << (renegotiated && peer.failed == SSL_ERROR_NONE ? "renegotiated" : "NOT renegotiated") << " within "
              << kBound.count() << " ms, watching " << spell(client.watching()) << '\n';
    ASSERT_EQ(peer.received.size(), filled) << "the peer could not read what it was sent";
    ASSERT_TRUE(renegotiated) << "the ClientHello never left once the socket could take it";
    EXPECT_EQ(client.eos, 1u) << "one eos, the fill's: none for the writability that was the read's";
    ASSERT_TRUE(peer.write("ping\n"));
    EXPECT_TRUE(peer.pump_until([&] { return peer.received.size() == filled + 5; })) << "the client's answer never arrived";
}

// ...and with input queued behind the HelloRequest: OpenSSL reads none of it before the ClientHello leaves, so watching
// readability meanwhile dispatched the read again on every pass, for nothing, until the peer read.
TEST_F(TlsReadWantsWrite, AReadThatWaitsToWriteIntoAFullSocketDoesNotWakeTheLoopForInputQueued) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(fill_send_buffer(client));
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::while_full = true; // the ClientHello: refused while the socket reports no room, until the peer reads
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    ASSERT_TRUE(peer.write("ping\n")) << "the peer could not send behind its HelloRequest"; // queued behind it
    pump(50ms);
    SSL *const ssl = client.transport().ssl_handle();
    ASSERT_EQ(client.disconnected, 0u) << "the client ended the connection (system error " << client.system_error << ")";
    ASSERT_EQ(SSL_in_init(ssl), 1) << "the case proves nothing: the client did not answer the HelloRequest";
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
    ASSERT_FALSE(writable_now(client.transport().native_handle())) << "the case proves nothing: the socket reports room";
    ASSERT_EQ(client.pings, 0u) << "the case proves nothing: the input behind the HelloRequest was read";
    const std::size_t events = events_in(kPasses);
    std::cout << "[ MEASURED ] a read waiting to write into a full socket, input queued (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << events << " events in " << kPasses << " loop passes, watching " << spell(client.watching()) << '\n';
    EXPECT_LE(events, kIdleEvents) << "the loop was woken while the read waited for the socket to take its write";
    EXPECT_TRUE(peer.pump_until([&] { return peer.received.size() == filled + 5; })) << "the client's answer never arrived";
    EXPECT_EQ(client.pings, 1u);
}

// The same for an input<> object, filled through its own socket before it starts: it watches writability alone too.
TEST_F(TlsReadWantsWrite, AnInputObjectThatWaitsToWriteIntoAFullSocketDoesNotWakeTheLoopForInputQueued) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingInput  input;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, input.transport()));
    qb::io::tcp::ssl::socket &socket = input.transport();
    ASSERT_NO_FATAL_FAILURE(fill_directly(socket));
    ASSERT_NO_FATAL_FAILURE(refuse::install(socket.ssl_handle()));
    refuse::while_full = true;
    input.switch_protocol<PingInput::Protocol>(input);
    input.start();
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    ASSERT_TRUE(peer.write("ping\n")) << "the peer could not send behind its HelloRequest";
    const std::size_t events = events_in(kPasses);
    std::cout << "[ MEASURED ] an input<> read waiting to write into a full socket, input queued (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << events << " events in " << kPasses << " loop passes, watching " << spell(input.watching()) << '\n';
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
    ASSERT_EQ(input.pings, 0u) << "the case proves nothing: the input behind the HelloRequest was read";
    EXPECT_LE(events, kIdleEvents) << "the loop was woken while the read waited for the socket to take its write";
    EXPECT_TRUE(peer.pump_until([&] { return input.pings == 1u && peer.received.size() == filled; }))
        << "the input behind the HelloRequest was never read (the peer read " << peer.received.size() << ")";
    EXPECT_EQ(input.disconnected, 0u);
}

// ...whatever else asks for writability meanwhile: output queued before the HelloRequest, output published during the
// wait, an empty arm during the wait. Readability watched beside it dispatched the read on every pass, each making a
// write the full socket refused; the output and the arm wait for the handshake the read drives.
TEST_F(TlsReadWantsWrite, AReadThatWaitsToWriteBesideOutputQueuedBeforeDoesNotWakeTheLoop) {
    read_waits_while_writability_is_asked(
        "output queued before", [](PingClient &client) { client.publish(std::string(kBulk, 'b')); }, [](PingClient &) {});
}

TEST_F(TlsReadWantsWrite, AReadThatWaitsToWriteBesideOutputPublishedDuringTheWaitDoesNotWakeTheLoop) {
    read_waits_while_writability_is_asked(
        "output published during the wait", [](PingClient &) {}, [](PingClient &client) { client.publish(std::string("x\n")); });
}

TEST_F(TlsReadWantsWrite, AReadThatWaitsToWriteBesideAnEmptyArmDoesNotWakeTheLoop) {
    read_waits_while_writability_is_asked("an empty arm during the wait", [](PingClient &) {}, [](PingClient &client) { client.publish(); });
}

// Once the write a read waited for has left, the reads go on, output still queued or not: a write pass that leaves
// output queued watches readability again. TLS 1.3: the client's Certificate, refused while its socket is full, leaves
// with the client's next write, which goes on with its own output; the peer, which reads only the fill, then sends.
TEST_F(TlsReadWantsWrite, AReadThatWaitedToWriteReadsAgainWhileOutputWaits) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    client.answer = false;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport(), [](SSL *ssl) { SSL_set_post_handshake_auth(ssl, 1); }));
    ASSERT_EQ(SSL_version(client.transport().ssl_handle()), TLS1_3_VERSION);
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(fill_send_buffer(client));
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::while_full = true;
    SSL_set_verify(peer.ssl, SSL_VERIFY_PEER, [](int, X509_STORE_CTX *) { return 1; });
    ASSERT_EQ(SSL_verify_client_post_handshake(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's CertificateRequest was not sent";
    pump(50ms); // the client reads it; its Certificate waits for room
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no Certificate";
    // Output that leaves after the Certificate, more than one pass writes -- and more than the peer's receive buffer,
    // empty once it has read the fill, takes at once: on XNU, where the fill outgrew kFill, it outgrows the fill too.
    const std::size_t bulk = filled == kFill ? kBulk : kBulk + filled;
    client.publish(std::string(bulk, 'b'));
    // The peer reads the fill alone: room for the Certificate (measured on Linux: the output's first write pass then
    // took nothing, the socket full again -- the output waits whole).
    ASSERT_TRUE(peer.pump_until([&] { return peer.received.size() >= filled; })) << "the peer could not read the fill";
    pump(50ms);
    ASSERT_GT(client.out().size(), 0u) << "the case proves nothing: the output did not wait";
    ASSERT_TRUE(peer.write("ping\n"));
    const bool read = pump_while_not([&] { return client.pings == 1u; }, 2000ms);
    std::cout << "[ MEASURED ] a read that waited to write, output waiting (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << (read ? "the peer's data read" : "the peer's data NOT read") << " with " << client.out().size()
              << " octets queued, watching " << spell(client.watching()) << '\n';
    EXPECT_TRUE(read) << "the reads did not go on once the write they waited for had left";
    EXPECT_TRUE(peer.pump_until([&] { return peer.received.size() == filled + bulk; })) << "the output never arrived whole";
    EXPECT_EQ(client.disconnected, 0u);
}

// An empty arm (publish() with no operand) earns its eos: after a read that waited to write and then took data, the mark
// it left must not make that write pass the read's. TLS 1.3: the client's Certificate is refused once; the data queued
// behind the CertificateRequest ends the wait, and its handler makes the arm while the mark is still left (the next read
// that finds nothing clears it).
TEST_F(TlsReadWantsWrite, AnEmptyArmAfterAReadThatWaitedToWriteEarnsItsEos) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    client.answer      = false;
    client.arm_on_ping = true;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport(), [](SSL *ssl) { SSL_set_post_handshake_auth(ssl, 1); }));
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::would_block = 1;
    SSL_set_verify(peer.ssl, SSL_VERIFY_PEER, [](int, X509_STORE_CTX *) { return 1; });
    ASSERT_EQ(SSL_verify_client_post_handshake(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's CertificateRequest was not sent";
    ASSERT_TRUE(peer.write("ping\n")); // queued behind it: the read that ends the wait takes it
    const std::size_t eos = client.eos;
    ASSERT_TRUE(peer.pump_until([&] { return client.pings == 1u; })) << "the data behind the CertificateRequest was never read";
    ASSERT_EQ(refuse::would_block, 0) << "the case proves nothing: the read wrote no Certificate";
    ASSERT_TRUE(client.marked_at_arm) << "the case proves nothing: the read that took the data left no mark";
    EXPECT_TRUE(pump_while_not([&] { return client.eos == eos + 1; }, 2000ms)) << "the empty arm's eos never came";
    pump(20ms);
    EXPECT_EQ(client.eos, eos + 1) << "one empty arm, one eos";
    EXPECT_EQ(client.disconnected, 0u);
}

// ...and one made while the read waits for a full socket to take its write: the write pass is the read's and the arm's
// alike -- the renegotiation completes, and the eos comes, once.
TEST_F(TlsReadWantsWrite, AnEmptyArmMadeWhileAReadWaitsToWriteEarnsItsEos) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(fill_send_buffer(client));
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::while_full = true;
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    pump(50ms);
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
    ASSERT_EQ(SSL_in_init(client.transport().ssl_handle()), 1) << "the case proves nothing: the client did not answer the HelloRequest";
    const std::size_t eos = client.eos;
    client.publish(); // the empty arm, while the read waits
    const bool renegotiated = peer.pump_until(
        [&] { return (peer.received.size() >= filled && SSL_renegotiate_pending(peer.ssl) == 0) || peer.failed != SSL_ERROR_NONE; });
    pump(20ms);
    std::cout << "[ MEASURED ] an empty arm while a read waits to write (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << (renegotiated ? "renegotiated" : "NOT renegotiated") << ", eos " << client.eos - eos << '\n';
    EXPECT_TRUE(renegotiated) << "the ClientHello never left once the socket could take it";
    EXPECT_EQ(client.eos, eos + 1) << "the empty arm made during the wait was not answered by one eos";
    EXPECT_EQ(client.disconnected, 0u);
}

// A client stopped and started again while its read waits to write keeps waiting for writability, not readability.
TEST_F(TlsReadWantsWrite, AClientStartedAgainWhileItsReadWaitsToWriteReadsAgainWhenItCan) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(fill_send_buffer(client));
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::while_full = true;
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    pump(50ms);
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
    client.stop();
    pump(10ms);
    client.start();
    const bool renegotiated = peer.pump_until(
        [&] { return (peer.received.size() >= filled && SSL_renegotiate_pending(peer.ssl) == 0) || peer.failed != SSL_ERROR_NONE; });
    std::cout << "[ MEASURED ] a client started again while its read waits to write (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << (renegotiated ? "renegotiated" : "NOT renegotiated") << ", watching " << spell(client.watching()) << '\n';
    EXPECT_TRUE(renegotiated) << "the ClientHello never left once the socket could take it";
    ASSERT_TRUE(peer.write("ping\n"));
    EXPECT_TRUE(peer.pump_until([&] { return peer.received.size() == filled + 5; })) << "the client's answer never arrived";
    // The wait is over -- the reads that followed found nothing more to write --: started again, readability.
    client.stop();
    pump(10ms);
    client.start();
    EXPECT_EQ(client.watching(), EV_READ) << "a start() after the wait still waited for writability: " << spell(client.watching());
}

// A socket error a call is made again after (ENOBUFS), met by a server's own handshake flight, which its first read
// drives: the handshake goes on where OpenSSL keeps it -- until 3.4 it ended the connection.
TEST_F(TlsReadWantsWrite, ASessionCompletesItsHandshakeWhoseFlightMetEnobufs) {
    state.fail_first = ENOBUFS;
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(start_server(0));
    ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", server_port), qb::io::SocketStatus::Done);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    int        theirs   = 0;
    const auto deadline = steady::now() + kBound;
    while ((theirs = peer.handshake_step()) == 0 && steady::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
    std::cout << "[ MEASURED ] a session's handshake flight failed once with ENOBUFS (" << OpenSSL_version(OPENSSL_VERSION)
              << "): the peer's handshake "
              << (theirs == 1   ? "completed"
                  : theirs == 0 ? "NOT completed"
                                : "FAILED")
              << "; session writes attempted " << refuse::attempts << ", disconnected " << state.disconnected << " (system error "
              << state.system_error << ")\n";
    ASSERT_EQ(refuse::fail_with, 0) << "the case proves nothing: the session wrote no flight";
    ASSERT_EQ(theirs, 1) << "the session's handshake ended on a socket error OpenSSL lets it go on after";
    ASSERT_TRUE(peer.write("ping\n"));
    EXPECT_TRUE(peer.pump_until([&] { return peer.received == "pong\n"; })) << "the session's answer never arrived";
}

// ...and a client's ClientHello, driven by hand through handshake_status() as the connectors drive it: the step answers
// 0 and handshake_wants_write() true, where it answered -1.
TEST_F(TlsReadWantsWrite, AClientHandshakeWhoseClientHelloMetEnobufsCompletes) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    const int ret = client.n_connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
    ASSERT_TRUE(ret == 0 || qb::io::socket_no_error(qb::io::socket::get_last_errno()));
    ASSERT_EQ(listener.accept(peer.tcp), qb::io::SocketStatus::Done);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    ASSERT_TRUE(qb::io::ssl::attach_socket(client.ssl_handle(), client.native_handle()));
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.ssl_handle()));
    refuse::fail_with = ENOBUFS;
    int        ours = 0, theirs = 0, refusals = 0;
    const auto deadline = steady::now() + kBound;
    while (steady::now() < deadline) {
        ours = client.handshake_status();
        if (ours < 0)
            break;
        if (ours == 0 && refuse::fail_with == 0 && refusals == 0) {
            ++refusals;
            EXPECT_EQ(refuse::attempts, 1u) << "the error met another write than the handshake's first";
            EXPECT_EQ(SSL_get_state(client.ssl_handle()), TLS_ST_CW_CLNT_HELLO) << "the error met another write than the ClientHello";
            EXPECT_TRUE(client.handshake_wants_write()) << "a step kept on a socket error waits for writability";
        }
        theirs = peer.handshake_step();
        if (ours == 1 && theirs == 1)
            break;
        std::this_thread::sleep_for(1ms);
    }
    std::cout << "[ MEASURED ] a ClientHello failed once with ENOBUFS (" << OpenSSL_version(OPENSSL_VERSION) << "): ours " << ours
              << ", theirs " << theirs << '\n';
    ASSERT_EQ(refuse::fail_with, 0) << "the case proves nothing: the client wrote no ClientHello";
    EXPECT_EQ(ours, 1) << "the client's handshake ended on a socket error OpenSSL lets it go on after";
    EXPECT_EQ(theirs, 1);
    client.close();
}

// A write that waits for the peer while close_after_deliver() drains: readability, not writability, until the peer
// answers -- the drain's own path arms writability again when it does. TLS 1.2: the client renegotiates, the session's
// read answers its ClientHello and waits for the client's Finished, and the session's output then waits for it too.
TEST_F(TlsReadWantsWrite, ADrainingSessionWhoseWriteWaitsForThePeerDoesNotWakeTheLoop) {
    RawContext context{RawContext::role::client, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer, SSL_OP_ALLOW_CLIENT_RENEGOTIATION));
    auto session = server->sessions().begin()->second;
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    static_cast<void>(SSL_do_handshake(peer.ssl)); // the ClientHello leaves
    ASSERT_TRUE(pump_while_not([&] { return SSL_in_init(state.ssl) == 1; })) << "the session never answered the ClientHello";
    pump(20ms); // the session's flight is out; it waits for the client's Finished, which the peer does not send yet
    session->publish(std::string(kBulk, 's'));
    session->close_after_deliver();
    const std::size_t events = events_in(kPasses);
    std::cout << "[ MEASURED ] a draining session's write waiting for the client's Finished (" << OpenSSL_version(OPENSSL_VERSION)
              << "): " << events << " events in " << kPasses << " loop passes, queued " << session->out().size() << '\n';
    ASSERT_EQ(session->out().size(), kBulk) << "the case proves nothing: the session wrote before the renegotiation completed";
    EXPECT_LE(events, kIdleEvents) << "the loop was woken while the write waited for the peer";
    EXPECT_TRUE(peer.pump_until([&] { return peer.received.size() == kBulk; })) << "the session's output never arrived";
    EXPECT_EQ(SSL_renegotiate_pending(peer.ssl), 0);
}

// ready_to_read() while the read waits to write leaves it waiting for writability: readability watched would mark the
// read's write pass as asked for -- an eos nobody asked for.
TEST_F(TlsReadWantsWrite, AReadyToReadWhileTheReadWaitsToWriteChangesNothing) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(fill_send_buffer(client));
    ASSERT_NO_FATAL_FAILURE(refuse::install(client.transport().ssl_handle()));
    refuse::while_full = true;
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    pump(50ms);
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
    const std::size_t eos = client.eos;
    client.ready_to_read();
    const bool renegotiated = peer.pump_until(
        [&] { return (peer.received.size() >= filled && SSL_renegotiate_pending(peer.ssl) == 0) || peer.failed != SSL_ERROR_NONE; });
    pump(20ms);
    EXPECT_TRUE(renegotiated) << "the ClientHello never left once the socket could take it";
    EXPECT_EQ(client.eos, eos) << "an eos nobody asked for: the writability was the read's";
}

// An input<> object started again while its read waits to write keeps waiting for writability.
TEST_F(TlsReadWantsWrite, AnInputObjectStartedAgainWhileItsReadWaitsToWriteReadsAgainWhenItCan) {
    ASSERT_EQ(listener.set_optval(SOL_SOCKET, SO_RCVBUF, kPeerWindow), 0);
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingInput  input;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, input.transport()));
    qb::io::tcp::ssl::socket &socket = input.transport();
    ASSERT_NO_FATAL_FAILURE(fill_directly(socket));
    ASSERT_NO_FATAL_FAILURE(refuse::install(socket.ssl_handle()));
    refuse::while_full = true;
    input.switch_protocol<PingInput::Protocol>(input);
    input.start();
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    pump(50ms);
    ASSERT_GE(refuse::attempts, 1u) << "the case proves nothing: the read wrote no ClientHello";
    input.stop();
    pump(10ms);
    input.start();
    const bool renegotiated = peer.pump_until(
        [&] { return (peer.received.size() >= filled && SSL_renegotiate_pending(peer.ssl) == 0) || peer.failed != SSL_ERROR_NONE; });
    EXPECT_TRUE(renegotiated) << "the ClientHello never left once the socket could take it";
    ASSERT_TRUE(peer.write("ping\n"));
    EXPECT_TRUE(peer.pump_until([&] { return input.pings == 1u; })) << "the data after the renegotiation was never read";
}

// A TLS 1.2 server may send application data behind its HelloRequest, and OpenSSL takes it during the renegotiation from
// SSL_read() alone. The client's answer to the first record waits for the handshake its read drives: an SSL_write() made
// then drove the handshake too, met the second record, and the connection ended.
TEST_F(TlsReadWantsWrite, AClientAnswersDataItsServerSentBehindAHelloRequest) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    ASSERT_TRUE(peer.write("ping\n")) << "the peer could not send behind its HelloRequest";
    ASSERT_TRUE(peer.write("ping\n"));
    const bool done = peer.pump_until([&] {
        return (peer.received == "pong\npong\n" && SSL_renegotiate_pending(peer.ssl) == 0) || peer.failed != SSL_ERROR_NONE
               || client.disconnected != 0u;
    });
    pump(20ms);
    std::cout << "[ MEASURED ] two records behind a HelloRequest (" << OpenSSL_version(OPENSSL_VERSION) << "): " << (done ? "" : "NOT ")
              << "done; pings " << client.pings << ", answered during the renegotiation " << client.answered_in_handshake << ", the peer read "
              << peer.received.size() << ", peer failure " << peer.failed << ", disconnected " << client.disconnected << " (reason "
              << client.reason << ", system error " << client.system_error << "), renegotiation pending " << SSL_renegotiate_pending(peer.ssl)
              << '\n';
    ASSERT_GE(client.answered_in_handshake, 1u) << "the case proves nothing: no answer was queued during the renegotiation";
    EXPECT_EQ(client.disconnected, 0u) << "the connection ended during the renegotiation";
    EXPECT_EQ(peer.failed, SSL_ERROR_NONE);
    EXPECT_EQ(peer.received, "pong\npong\n") << "the answers never arrived";
    EXPECT_EQ(SSL_renegotiate_pending(peer.ssl), 0) << "the renegotiation never completed";
}

namespace tls_read_wants_write_test {
/// Whether the wrapper went in front of the socket BIO when the handshake started.
bool g_installed_at_start = false;

/// At the start of a handshake: the wrapper in front of the socket BIO, its next write failed once with ENOBUFS.
void
fail_the_client_hello(SSL const *ssl, int where, int) {
    if ((where & SSL_CB_HANDSHAKE_START) == 0 || g_installed_at_start)
        return;
    g_installed_at_start = true;
    refuse::install(const_cast<SSL *>(ssl));
    refuse::fail_with = ENOBUFS;
}
} // namespace tls_read_wants_write_test

// A blocking connect() has no pass to make a handshake step again: a socket error its ClientHello meets fails it, as it
// did before 3.4 -- never a success with the handshake unfinished.
TEST_F(TlsReadWantsWrite, ABlockingConnectWhoseClientHelloMetEnobufsFails) {
    SSL_CTX *const ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(ctx, nullptr);
    SSL *const ssl = SSL_new(ctx);
    SSL_CTX_free(ctx); // the SSL object holds its own reference
    ASSERT_NE(ssl, nullptr);
    g_installed_at_start = false;
    SSL_set_info_callback(ssl, &fail_the_client_hello);
    qb::io::tcp::ssl::socket client;
    client.init(ssl);
    client.set_insecure();
    // The server end accepted and closed at once: a handshake that met no error ends on the close, never waits.
    std::thread server([this] {
        qb::io::tcp::socket accepted;
        const auto          deadline = steady::now() + kBound;
        while (listener.accept(accepted) != qb::io::SocketStatus::Done && steady::now() < deadline)
            std::this_thread::sleep_for(1ms);
        accepted.close();
    });
    const int   ret = client.connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
    server.join();
    std::cout << "[ MEASURED ] a blocking connect whose ClientHello failed with ENOBUFS (" << OpenSSL_version(OPENSSL_VERSION) << "): connect "
              << ret << ", handshake complete " << client.handshake_complete() << '\n';
    ASSERT_TRUE(g_installed_at_start) << "the case proves nothing: no handshake started";
    ASSERT_EQ(refuse::fail_with, 0) << "the case proves nothing: the ClientHello met no error";
    EXPECT_EQ(ret, -1) << "a blocking connect answered success with its handshake unfinished";
    EXPECT_FALSE(client.handshake_complete());
    client.close();
}

// A HelloRequest that comes while OpenSSL holds an application record it could not write whole, two records of data
// behind it, the client answering each: the record, the answers and the renegotiation all get through. OpenSSL decides
// when the renegotiation starts behind a pending write; the io object writes no record of its own meanwhile.
TEST_F(TlsReadWantsWrite, ARenegotiationBehindAHalfWrittenRecordCompletes) {
    RawContext context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer    peer{context, RawContext::role::server};
    PingClient client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    SSL *const ssl = client.transport().ssl_handle();
    ASSERT_NO_FATAL_FAILURE(refuse::install(ssl));
    refuse::half_once        = 1;
    const std::string record = std::string(1000, 'x') + "\n";
    client.publish(record);
    pump(20ms);
    ASSERT_TRUE(refuse::hold) << "the case proves nothing: no record was taken in half";
    ASSERT_EQ(SSL_want_write(ssl), 1) << "the case proves nothing: OpenSSL holds no record";
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1);
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent";
    ASSERT_TRUE(peer.write("ping\n")) << "the peer could not send behind its HelloRequest";
    ASSERT_TRUE(peer.write("ping\n"));
    pump(50ms); // the client reads what it can while its record waits
    std::cout << "[ MEASURED ] a HelloRequest behind a half-written record (" << OpenSSL_version(OPENSSL_VERSION) << "): pings " << client.pings
              << ", in init " << SSL_in_init(ssl) << ", state " << SSL_state_string_long(ssl) << ", disconnected " << client.disconnected
              << '\n';
    refuse::hold               = false;
    const std::string expected = record + "pong\npong\n";
    const bool        done     = peer.pump_until([&] {
        return (peer.received == expected && SSL_renegotiate_pending(peer.ssl) == 0) || peer.failed != SSL_ERROR_NONE
               || client.disconnected != 0u;
    });
    pump(20ms);
    std::cout << "[ MEASURED ] ...then " << (done ? "" : "NOT ") << "done: the peer read " << peer.received.size() << " of " << expected.size()
              << ", peer failure " << peer.failed << ", disconnected " << client.disconnected << " (reason " << client.reason
              << ", system error " << client.system_error << "), renegotiation pending " << SSL_renegotiate_pending(peer.ssl) << '\n';
    EXPECT_EQ(client.disconnected, 0u) << "the connection ended";
    EXPECT_EQ(peer.failed, SSL_ERROR_NONE);
    EXPECT_EQ(peer.received, expected) << "the record or the answers did not arrive";
    EXPECT_EQ(SSL_renegotiate_pending(peer.ssl), 0) << "the renegotiation never completed";
}
