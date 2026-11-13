/**
 * @file qb/tests/io/system/tls/tls-close-notify.cpp
 * @brief A TLS connection this side ends tells its peer so -- one `close_notify` alert before the TCP FIN, never
 *        awaited -- and what a raw OpenSSL peer reads of it (Huly QB-1270).
 *
 * TLS has its own end of stream: the `close_notify` alert. A peer that reads the TCP FIN without it cannot tell a
 * stream its sender ended from one something cut, and OpenSSL 3 reports it as an error (`SSL_ERROR_SSL`,
 * `SSL_R_UNEXPECTED_EOF_WHILE_READING`). qb sent none until 3.4: every connection it ended read as truncated.
 *
 * The peer here is OpenSSL alone -- an `SSL` object over a plain TCP socket, driven by the test: what it reads is
 * what a browser, curl, nginx or any OpenSSL program reads. Each case ends a connection one way and asserts how the
 * stream ended for the peer: the last bytes, then `SSL_ERROR_ZERO_RETURN` (the alert), then the FIN and nothing else.
 *
 * - **Sent**: a standalone object's `disconnect()`, `disconnect_now()` and `close_after_deliver()`; a server session
 *   that ends its connection, handed to the loop or lingering through `close_after_deliver()` (the alert follows the
 *   delivered bytes and precedes the half-close); an object, or a stream, closed or destroyed while connected with
 *   nothing left to write; `ssl::socket::disconnect()` called directly; a close "at once" -- a linger of zero, a size
 *   guard -- whose output was written; the answer to a peer that ended the session with its own `close_notify`
 *   (RFC 8446 6.1), by a client, a session, an `input<>` object or session, or the socket alone.
 * - **Not sent**: before the handshake completed; after the peer's bare EOF, a fatal error, or a reset -- nothing is
 *   even written into a reset connection; when this side drops output it had queued, whoever ended the connection --
 *   that stream IS cut, and saying otherwise would tell the peer a truncated reply is whole: a stream closed or
 *   destroyed with output queued, a session extracted with output queued (its stream marked cut: the next owner's
 *   close says nothing either); after `quiet_shutdown()`; in OpenSSL's own quiet-shutdown mode.
 * - **Never in the way**: one call, no wait -- a socket that cannot take the alert is closed without it, a blocking
 *   socket included; nothing is left in the thread's OpenSSL error queue; no SIGPIPE is raised.
 * - **Sessions stay resumable**: a session saved from a connection resumes after that connection is closed, by this
 *   side or by its peer -- OpenSSL discards the session of an `SSL` object freed without its shutdown state set,
 *   which is what every qb close did since the `SSL_shutdown()` call was dropped.
 * - **The loop still closes after the peer**: what the peer answers -- its own `close_notify`, late records -- is
 *   read and dropped as bytes, and its EOF ends the close at once.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>
#include <qb/io/transport/stcp.h>
#include <qb/system/allocator/pipe.h>

#include "../../shared/close_after_peer_support.h"
#include "../../shared/ssl_fixtures.h"

using namespace std::chrono_literals;

namespace tls_close_notify_test {

using namespace qb::io::test::close_after_peer;

/// How a TLS stream ended for the raw OpenSSL peer that read it.
struct TlsEnd {
    std::string   received;                      ///< The application octets read before the end.
    int           error        = SSL_ERROR_NONE; ///< `SSL_get_error()` of the read that ended it (`WANT_READ`: still open).
    unsigned long first_error  = 0;              ///< The first entry of the error queue at that read.
    std::size_t   octets_after = 0;              ///< What the TCP connection carried after the TLS end.
    bool          fin          = false;          ///< Whether the TCP connection then ended with a FIN.
    int           tcp_error    = 0;              ///< ...or with this socket error.

    [[nodiscard]] bool
    clean() const noexcept {
        return error == SSL_ERROR_ZERO_RETURN;
    }

    [[nodiscard]] bool
    unexpected_eof() const noexcept {
        return error == SSL_ERROR_SSL && ERR_GET_REASON(first_error) == SSL_R_UNEXPECTED_EOF_WHILE_READING;
    }

    /// The codes, spelled: "SSL_ERROR_SSL (1), reason 294: unexpected eof while reading; then 0 octets, FIN".
    [[nodiscard]] std::string
    text() const {
        std::string out;
        switch (error) {
            case SSL_ERROR_ZERO_RETURN:
                out = "SSL_ERROR_ZERO_RETURN";
                break;
            case SSL_ERROR_SSL:
                out = "SSL_ERROR_SSL";
                break;
            case SSL_ERROR_SYSCALL:
                out = "SSL_ERROR_SYSCALL";
                break;
            case SSL_ERROR_WANT_READ:
                out = "SSL_ERROR_WANT_READ (still open)";
                break;
            default:
                out = "SSL_get_error";
                break;
        }
        out += " (" + std::to_string(error) + ")";
        if (first_error != 0) {
            char reason[256] = {};
            ERR_error_string_n(first_error, reason, sizeof(reason));
            out += ", reason " + std::to_string(ERR_GET_REASON(first_error)) + ": " + reason;
        }
        out += "; " + std::to_string(received.size()) + " octets before, " + std::to_string(octets_after) + " after, ";
        out += fin ? "FIN" : tcp_error ? "socket error " + std::to_string(tcp_error) : "no FIN";
        return out;
    }
};

/// What a case's peer read, on the test's output: the codes the report quotes, per platform.
void
note(std::string_view what, TlsEnd const &end) {
    std::cout << "[   PEER   ] " << what << ": " << end.text() << '\n';
}

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
 * @brief The peer: OpenSSL over a plain TCP socket, nothing of qb's TLS socket in between.
 * @details Non-blocking, pumped by the test's thread, which is also the loop's. Its reads are `SSL_read()` and
 *          `SSL_get_error()`: what any OpenSSL program sees of the connection qb ends.
 */
class RawPeer {
    RawContext::role _role;

public:
    qb::io::tcp::socket tcp;
    SSL                *ssl = nullptr;

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

    /// Writes `what` whole, the loop running meanwhile.
    [[nodiscard]] bool
    write(std::string_view what, std::chrono::milliseconds budget = 5000ms) {
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

    /// This end's own `close_notify`, once: what an OpenSSL peer answers ours with.
    int
    close_notify() {
        ERR_clear_error();
        return SSL_shutdown(ssl);
    }

    /**
     * @brief Reads the stream to its end: the application octets, how TLS ended, then what TCP still carried.
     * @param stop_at When not zero, returns once that many octets are read and the stream is still open.
     */
    [[nodiscard]] TlsEnd
    read_to_end(std::chrono::milliseconds budget = 5000ms, std::size_t stop_at = 0) {
        TlsEnd     out;
        const auto deadline = steady::now() + budget;
        char       buffer[16384];
        for (;;) {
            qb::io::async::run(EVRUN_NOWAIT);
            ERR_clear_error();
            const int n      = SSL_read(ssl, buffer, static_cast<int>(sizeof(buffer)));
            const int system = n > 0 ? 0 : qb::io::socket::get_last_errno(); // before anything else replaces it
            if (n > 0) {
                out.received.append(buffer, static_cast<std::size_t>(n));
                if (stop_at != 0 && out.received.size() >= stop_at) {
                    out.error = SSL_ERROR_WANT_READ;
                    return out;
                }
                continue;
            }
            out.error = SSL_get_error(ssl, n);
            if (out.error != SSL_ERROR_WANT_READ && out.error != SSL_ERROR_WANT_WRITE) {
                out.first_error = ERR_peek_error();
                if (out.error == SSL_ERROR_SYSCALL && system != 0) { // the connection was reset: that is its whole end
                    out.tcp_error = system;
                    return out;
                }
                break;
            }
            out.error = SSL_ERROR_WANT_READ;
            if (steady::now() >= deadline)
                return out;
            std::this_thread::sleep_for(1ms);
        }
        // The TLS stream is over. What the TCP connection still carries is read raw: after one close_notify, the FIN.
        for (;;) {
            qb::io::async::run(EVRUN_NOWAIT);
            const int n = qb::io::socket::recv(tcp.native_handle(), buffer, static_cast<int>(sizeof(buffer)));
            if (n > 0) {
                out.octets_after += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                out.fin = true;
                break;
            }
            const int error = qb::io::socket::get_last_errno();
            if (!qb::io::socket::not_recv_error(error)) {
                out.tcp_error = error;
                break;
            }
            if (steady::now() >= deadline)
                break;
            std::this_thread::sleep_for(1ms);
        }
        return out;
    }
};

/// Sends until the socket takes nothing more, down to the octet: not even an alert fits. Returns what it took.
std::size_t
fill(::socket_type socket) {
    const std::string chunk(std::size_t{64} << 10, 'f');
    std::size_t       sent = 0;
    for (;;) {
        const int n = qb::io::socket::send(socket, chunk.data(), static_cast<int>(chunk.size()));
        if (n <= 0)
            break;
        sent += static_cast<std::size_t>(n);
    }
    for (;;) {
        const int n = qb::io::socket::send(socket, chunk.data(), 1);
        if (n <= 0)
            break;
        sent += static_cast<std::size_t>(n);
    }
    return sent;
}

/// `ssl::socket::mark_cut()`, where the tree has it.
template <typename Socket>
bool
mark_cut(Socket &socket) {
    if constexpr (requires { socket.mark_cut(); }) {
        socket.mark_cut();
        return true;
    } else {
        return false;
    }
}

/// `ssl::socket::close_notify()`, where the tree has it: the witness builds against a tree that sends none.
template <typename Socket>
bool
send_close_notify(Socket &socket) {
    if constexpr (requires { socket.close_notify(); }) {
        return socket.close_notify();
    } else {
        ADD_FAILURE() << "this tree's ssl::socket has no close_notify()";
        return false;
    }
}

/// `ssl::socket::quiet_shutdown()`, likewise.
template <typename Socket>
void
end_quietly(Socket &socket) {
    if constexpr (requires { socket.quiet_shutdown(); })
        socket.quiet_shutdown();
    else
        ADD_FAILURE() << "this tree's ssl::socket has no quiet_shutdown()";
}

/// A standalone TLS client.
class TlsClient : public qb::io::use<TlsClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::text::command<TlsClient>;

    std::size_t disconnected_events = 0;
    std::size_t messages            = 0;
    int         reason              = -99;
    int         system_error        = -99;
    std::string last;

    std::function<void(TlsClient &)> disconnected_hook; ///< Runs at the end of on(event::disconnected&&).

    void
    on(Protocol::message &&message) {
        ++messages;
        last = message.text;
    }

    void
    on(qb::io::async::event::disconnected &&event) {
        ++disconnected_events;
        reason       = event.reason;
        system_error = event.error_code.value();
        if (disconnected_hook)
            disconnected_hook(*this);
    }

    void
    drop_now() {
        this->disconnect_now();
    }

    /// What the object still has queued: zero once everything published was handed to TLS.
    [[nodiscard]] std::size_t
    queued() noexcept {
        return this->out().size();
    }
};

// What the session under test saw and left: one session per case.
struct State {
    int                destroyed    = 0;
    int                disconnected = 0;
    steady::time_point destroyed_at{};
    ::socket_type      handle           = qb::io::invalid_socket;
    qb::duration       linger           = 20s;   ///< Long: a session that ends at once did not wait for its bound.
    bool               clear_in_hook    = false; ///< on(event::disconnected&&) empties the session's output queue.
    bool               queue_in_hook    = false; ///< ...or queues output nobody will write.
    std::size_t        held             = 0;     ///< What a session asked to "hold" had queued when it returned.
    bool               clear_on_extract = false; ///< on(event::extracted&&) empties the session's output queue.
    bool               queue_on_extract = false; ///< ...or queues output the session will never write.
    int                extracted        = 0;
    std::size_t        max_message_size = 0;
};
State *g_state = nullptr;

/// What the "cut" command queues: more than one TLS record, so the one write a disconnect() grants leaves the rest.
inline constexpr std::size_t CUT = std::size_t{64} << 10;

class TlsServer;

class TlsSession : public qb::io::use<TlsSession>::tcp::ssl::client<TlsServer> {
public:
    using Protocol = qb::protocol::text::command<TlsSession>;

    explicit TlsSession(IOServer &server)
        : client(server) {}

    ~TlsSession() {
        ++g_state->destroyed;
        g_state->destroyed_at = steady::now();
    }

    void
    on(Protocol::message &&message) {
        if (message.text == "bye") { // says its last bytes and ends the connection, everything written
            *this << LAST;
            disconnect();
        } else if (message.text == "cad") { // the lingering close: deliver, tell the peer, wait for it
            *this << LAST;
            close_after_deliver();
        } else if (message.text == "line") { // a reply a qb client frames, then the lingering close
            *this << LAST << '\n';
            close_after_deliver();
        } else if (message.text == "now") { // at once, by decision: no wait for the peer
            set_linger(qb::duration::zero());
            *this << LAST;
            disconnect();
        } else if (message.text == "hold") { // queues more than one pass writes, and stays connected
            publish(std::string(CUT, 'c'));
            g_state->held = this->out().size();
        } else if (message.text == "cut") { // ends with output still queued: it is dropped
            publish(std::string(CUT, 'c'));
            disconnect();
        }
    }

    void
    on(qb::io::async::event::disconnected &&) {
        ++g_state->disconnected;
        if (g_state->clear_in_hook) // what a reconnecting client does with a dead connection's bytes
            this->out().reset();
        if (g_state->queue_in_hook)
            this->out() << LAST;
    }

    void
    on(qb::io::async::event::extracted &&) {
        ++g_state->extracted;
        if (g_state->clear_on_extract)
            this->out().reset();
        if (g_state->queue_on_extract)
            this->out() << LAST;
    }
};

class TlsServer : public qb::io::use<TlsServer>::tcp::ssl::server<TlsSession> {
public:
    void
    on(IOSession &session) {
        session.set_linger(g_state->linger);
        if (g_state->max_message_size)
            session.set_max_message_size(g_state->max_message_size);
        g_state->handle = session.transport().native_handle();
    }
};

/// The owner a session built on `output<>` alone reports to.
struct Registry {
    std::size_t released = 0;

    void
    disconnected(int) noexcept {
        ++released;
    }
};

/// A server-side session on `output<>` alone. Its transport is a TLS STREAM, as a shipped session's is: the stream's
/// destructor closes the socket, through `ssl::socket::disconnect()`.
class TlsOutputSession : public qb::io::async::output<TlsOutputSession> {
    qb::io::transport::stcp   _stream;
    qb::allocator::pipe<char> _out;
    Registry                 &_server;

public:
    constexpr static const bool has_server = true;

    std::size_t disconnected_events = 0;
    bool        clear_in_hook       = false; ///< on(event::disconnected&&) empties the output queue.
    bool        queue_in_hook       = false; ///< ...or queues output nobody will write.
    bool        close_in_hook       = false; ///< ...or closes the transport itself.

    explicit TlsOutputSession(Registry &owner)
        : _server(owner) {}

    Registry &
    server() noexcept {
        return _server;
    }
    [[nodiscard]] int
    id() const noexcept {
        return 1;
    }
    qb::io::tcp::ssl::socket &
    transport() noexcept {
        return _stream.transport();
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
        const int ret = transport().write(_out.begin(), _out.size());
        if (ret > 0)
            _out.free_front(static_cast<std::size_t>(ret));
        return ret;
    }

    void
    on(qb::io::async::event::disconnected &&) noexcept {
        ++disconnected_events;
        if (clear_in_hook)
            _out.reset();
        if (queue_in_hook)
            _out << LAST;
        if (close_in_hook)
            static_cast<void>(transport().disconnect());
    }
};

/// A standalone object on `output<>` alone over a TLS socket: it writes what it is given.
class TlsOutput : public qb::io::async::output<TlsOutput> {
    qb::io::tcp::ssl::socket  _transport;
    qb::allocator::pipe<char> _out;

public:
    std::size_t disconnected_events = 0;
    bool        clear_in_hook       = false; ///< on(event::disconnected&&) empties the output queue.
    bool        queue_in_hook       = false; ///< ...or queues output nobody will write.
    bool        close_in_hook       = false; ///< ...or closes the transport itself.

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
        return static_cast<std::size_t>(-1); // no cap: publish() admits everything
    }

    int
    write() noexcept {
        if (_out.empty()) // as `ostream::write()`: a TLS socket is never handed an empty write, which it refuses
            return 0;
        const int ret = _transport.write(_out.begin(), _out.size());
        if (ret > 0)
            _out.free_front(static_cast<std::size_t>(ret));
        return ret;
    }

    void
    on(qb::io::async::event::disconnected &&) noexcept {
        ++disconnected_events;
        if (clear_in_hook)
            _out.reset();
        if (queue_in_hook)
            _out << LAST;
        if (close_in_hook)
            static_cast<void>(transport().disconnect());
    }
};

/// A standalone object on `input<>` alone over a TLS socket: it reads and keeps.
class TlsInput : public qb::io::async::input<TlsInput> {
    qb::io::tcp::ssl::socket  _transport;
    qb::allocator::pipe<char> _in;

public:
    std::size_t disconnected_events = 0;

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
    on(qb::io::async::event::disconnected &&) noexcept {
        ++disconnected_events;
    }
};

/// A server-side session on `input<>` alone. Its transport is a TLS STREAM, as a shipped session's is: the stream's
/// destructor closes the socket, through `ssl::socket::disconnect()`.
class TlsInputSession : public qb::io::async::input<TlsInputSession> {
    qb::io::transport::stcp   _stream;
    qb::allocator::pipe<char> _in;
    Registry                 &_server;

public:
    constexpr static const bool has_server = true;

    std::size_t disconnected_events = 0;

    explicit TlsInputSession(Registry &owner)
        : _server(owner) {}

    Registry &
    server() noexcept {
        return _server;
    }
    [[nodiscard]] int
    id() const noexcept {
        return 1;
    }
    qb::io::tcp::ssl::socket &
    transport() noexcept {
        return _stream.transport();
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
        const int             ret   = transport().read(into, chunk);
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
    on(qb::io::async::event::disconnected &&) noexcept {
        ++disconnected_events;
    }
};

/// A protocol that never finds a message.
template <typename IO>
class NoMessageOf : public qb::io::async::AProtocol<IO> {
public:
    explicit NoMessageOf(IO &io) noexcept
        : qb::io::async::AProtocol<IO>(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return 0u;
    }
    void
    onMessage(std::size_t) noexcept final {}
    void
    reset() noexcept final {}
};
using NoMessage = NoMessageOf<TlsInput>;

/// The peer resets the connection: a close with a linger of zero seconds and no shutdown first is an RST alone, not
/// a FIN (which a shutdown would send before it, and the reader would read as an EOF).
void
reset_connection(qb::io::tcp::socket &peer) {
    struct linger hard{};
    hard.l_onoff  = 1;
    hard.l_linger = 0;
    ASSERT_EQ(::setsockopt(peer.native_handle(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&hard), sizeof(hard)), 0);
    peer.close(QB_SD_NONE);
}

/// What the write callback below counts: every write a socket BIO it watches attempts, taken or refused.
std::size_t g_write_attempts = 0;

/// Counts, from here on, every write the `SSL` object's socket BIO attempts -- an alert written into a dead connection
/// is one even when the system refuses it, which neither the peer nor `BIO_number_written()` would show. The callback
/// lives as long as the BIO; the counter is global, so nothing dangles when the BIO goes with its `SSL` object.
void
count_write_attempts(SSL *ssl) {
    BIO *const bio = SSL_get_wbio(ssl);
    ASSERT_NE(bio, nullptr);
    g_write_attempts = 0;
    BIO_set_callback_ex(bio, [](BIO *, int oper, const char *, std::size_t, int, long, int ret, std::size_t *) -> long {
        if (oper == BIO_CB_WRITE)
            ++g_write_attempts;
        return ret;
    });
}

/**
 * @brief A filter BIO over a socket BIO that fails the next write, or read, once with a system error the caller
 *        names: what a socket that refuses one call does. It sets no retry flag, so OpenSSL reports the call as
 *        `SSL_ERROR_SYSCALL` -- as it does for `ENOBUFS` from a real socket, which its socket BIO does not retry.
 *        Or it answers every write "try again later", the retry flag set: what a socket BIO answers for a send
 *        buffer that takes nothing (`EWOULDBLOCK`), on every system and at every moment.
 */
namespace inject_once {
int  fail_write    = 0;     ///< The system error the next write fails with, once; 0: none.
int  fail_read     = 0;     ///< The same for the next read.
bool refuse_writes = false; ///< Every write would block, until cleared.

int
write(BIO *bio, const char *in, int size) {
    BIO_clear_retry_flags(bio);
    if (refuse_writes) {
        BIO_set_retry_write(bio);
        return -1;
    }
    if (const int error = std::exchange(fail_write, 0)) {
        qb::io::socket::set_last_errno(error);
        return -1;
    }
    const int ret = BIO_write(BIO_next(bio), in, size);
    BIO_copy_next_retry(bio);
    return ret;
}
int
read(BIO *bio, char *out, int size) {
    BIO_clear_retry_flags(bio);
    if (const int error = std::exchange(fail_read, 0)) {
        qb::io::socket::set_last_errno(error);
        return -1;
    }
    const int ret = BIO_read(BIO_next(bio), out, size);
    BIO_copy_next_retry(bio);
    return ret;
}
long
ctrl(BIO *bio, int cmd, long num, void *ptr) {
    return BIO_ctrl(BIO_next(bio), cmd, num, ptr);
}
int
create(BIO *bio) {
    BIO_set_init(bio, 1);
    return 1;
}
BIO_METHOD *
method() {
    static BIO_METHOD *const methods = [] {
        BIO_METHOD *const m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_FILTER, "qb-test-fail-once");
        BIO_meth_set_write(m, &inject_once::write);
        BIO_meth_set_read(m, &inject_once::read);
        BIO_meth_set_ctrl(m, &inject_once::ctrl);
        BIO_meth_set_create(m, &inject_once::create);
        return m;
    }();
    return methods;
}

/// The filter over the socket BIO `ssl` writes through (`writes`) or reads through: the descriptor it carries is
/// still the socket's (the filter passes every control on).
void
install(SSL *ssl, bool writes) {
    BIO *const sock = writes ? SSL_get_wbio(ssl) : SSL_get_rbio(ssl);
    ASSERT_NE(sock, nullptr);
    BIO *const filter = BIO_new(method());
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(BIO_up_ref(sock), 1); // the filter's own reference: the SSL object lets go of the one it replaces
    BIO_push(filter, sock);
    if (writes)
        SSL_set0_wbio(ssl, filter);
    else
        SSL_set0_rbio(ssl, filter);
}
} // namespace inject_once

#if !defined(_WIN32)
/// While it lives, SIGPIPE is counted rather than ignored: a write into a connection its peer reset raises it, and an
/// application that put the default disposition back would end there.
class SigpipeCount {
    struct sigaction                  _previous{};
    static volatile std::sig_atomic_t raised;

    static void
    count(int) noexcept {
        raised = raised + 1;
    }

public:
    SigpipeCount() {
        raised = 0;
        struct sigaction counting{};
        counting.sa_handler = &SigpipeCount::count;
        sigemptyset(&counting.sa_mask);
        ::sigaction(SIGPIPE, &counting, &_previous);
    }
    SigpipeCount(SigpipeCount const &)            = delete;
    SigpipeCount &operator=(SigpipeCount const &) = delete;
    ~SigpipeCount() {
        ::sigaction(SIGPIPE, &_previous, nullptr);
    }

    [[nodiscard]] int
    count() const noexcept {
        return static_cast<int>(raised);
    }
};
volatile std::sig_atomic_t SigpipeCount::raised = 0;
#else
/// Windows raises no SIGPIPE: nothing to count.
struct SigpipeCount {
    [[nodiscard]] static int
    count() noexcept {
        return 0;
    }
};
#endif

/**
 * @brief Whether OpenSSL ends the TLS session on every socket error it reports (`SSL_ERROR_SYSCALL`), the ones qb-io
 *        makes the call again after included.
 * @details OpenSSL 4 does: from the failing call on its state is "error" (`SSL_in_init()` is 1) and every call fails
 *          with `SSL_ERROR_SSL` and no system call -- measured, 4.1.0 on macOS: a reset met by a read or a write, an
 *          `ENOBUFS` or `EINTR` write, an `EINTR` read. OpenSSL 3 keeps the session as it was, and a call made again
 *          goes on (3.5.7 on Linux, 3.5.9 on macOS, 3.6.3 on Windows). Both document the error as fatal
 *          (`SSL_get_error(3)`: "no further I/O operations should be performed on the connection"); 4 holds to it.
 *          Read at run time: the library the binary runs with decides, not the headers it was built with.
 */
const bool kSocketErrorEndsTheSession = OPENSSL_version_major() >= 4;

/// What `ssl::socket::read()` and `write()` report for a socket error OpenSSL ended the session on (OpenSSL 4): a
/// connection aborted on this side -- never an error the io layer makes the call again after.
#if defined(_WIN32)
constexpr int kSessionAborted = WSAECONNABORTED;
#else
constexpr int kSessionAborted = ECONNABORTED;
#endif

/// Whether OpenSSL ended `ssl`'s session: its error state, which `SSL_state_string()` names "SSLERR" -- what
/// `ssl::socket::read()` and `write()` read to report a socket error that ended the session.
bool
in_error_state(SSL *ssl) {
    const char *state = SSL_state_string(ssl);
    return state && std::string_view(state) == "SSLERR";
}

/**
 * @brief Whether a socket filled to the octet, its peer reading nothing, still takes nothing a moment later.
 * @details Linux and Windows keep it full. macOS does not: its receive-buffer autotuning opens room within
 *          microseconds to milliseconds of the fill, and a 24-octet alert written right after it was taken 4 times
 *          in 12 with OpenSSL 4.1.0 (and the socket took 131 101 octets more within 200 ms of a fill with 3.5.9).
 */
constexpr bool kAFilledSocketStaysFull =
#if defined(__APPLE__)
    false;
#else
    true;
#endif

void first_connection(RawPeer &peer, qb::io::tcp::ssl::socket &client, qb::io::ssl::Session &session);

class TlsCloseNotify : public CleanLoop {
protected:
    State                      state;
    qb::io::tcp::listener      listener; ///< Plain TCP: the raw server peer accepts on it.
    std::uint16_t              port = 0;
    std::unique_ptr<TlsServer> server; ///< qb's TLS server, for the session cases.
    std::uint16_t              server_port = 0;
    qb::io::tcp::ssl::listener tls_listener; ///< For a server-side TLS socket the case drives itself.

    void
    SetUp() override {
        CleanLoop::SetUp();
        g_state                    = &state;
        inject_once::fail_write    = 0;
        inject_once::fail_read     = 0;
        inject_once::refuse_writes = false;
        ASSERT_TRUE(qb::io::test::require_ssl_files());
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        port = listener.local_endpoint().port();
        ASSERT_NE(port, 0u);
    }

    void
    TearDown() override {
        server.reset();
        tls_listener.close();
        listener.close();
        CleanLoop::TearDown();
        g_state = nullptr;
    }

    /// `client` connected to `peer`, a raw OpenSSL server, both handshakes complete; `client` is non-blocking.
    void
    connect_to(RawPeer &peer, qb::io::tcp::ssl::socket &client) {
        client.set_insecure();
        const int ret = client.n_connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
        const int err = qb::io::socket::get_last_errno();
        ASSERT_TRUE(ret == 0 || qb::io::socket_no_error(err)) << "n_connect: " << ret << ", error " << err;
        ASSERT_EQ(listener.accept(peer.tcp), qb::io::SocketStatus::Done);
        ASSERT_NO_FATAL_FAILURE(peer.attach());
        const auto deadline = steady::now() + 5s;
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

    /// qb's TLS server listens.
    void
    start_server() {
        server = std::make_unique<TlsServer>();
        server->transport().init(
            qb::io::ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem")));
        ASSERT_EQ(server->transport().listen_v4(0, "127.0.0.1"), 0);
        server_port = server->transport().local_endpoint().port();
        server->start();
    }

    /// `peer`, a raw OpenSSL client, connects to qb's TLS server and completes its handshake with the session.
    void
    connect_to_server(RawPeer &peer) {
        ASSERT_NO_FATAL_FAILURE(start_server());
        ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", server_port), qb::io::SocketStatus::Done);
        ASSERT_NO_FATAL_FAILURE(peer.attach());
        const auto deadline = steady::now() + 5s;
        for (;;) {
            qb::io::async::run(EVRUN_NOWAIT);
            const int theirs = peer.handshake_step();
            ASSERT_GE(theirs, 0) << "the raw client's handshake failed";
            if (theirs == 1)
                break;
            ASSERT_LT(steady::now(), deadline) << "the handshake did not complete";
            std::this_thread::sleep_for(1ms);
        }
    }

    /// `accepted`, a qb TLS socket on the server's side of a connection `peer`, a raw OpenSSL client, opened: both
    /// handshakes complete, `accepted` non-blocking.
    void
    accept_from(RawPeer &peer, qb::io::tcp::ssl::socket &accepted) {
        tls_listener.init(qb::io::ssl::create_server_context(TLS_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                             qb::io::test::ssl_resource_path("key.pem")));
        ASSERT_EQ(tls_listener.listen_v4(0, "127.0.0.1"), 0);
        ASSERT_EQ(peer.tcp.connect_v4("127.0.0.1", tls_listener.local_endpoint().port()), qb::io::SocketStatus::Done);
        ASSERT_EQ(tls_listener.accept(accepted), 0);
        ASSERT_EQ(accepted.set_nonblocking(true), 0);
        ASSERT_NO_FATAL_FAILURE(peer.attach());
        const auto deadline = steady::now() + 5s;
        for (;;) {
            const int ours   = accepted.handshake_status();
            const int theirs = peer.handshake_step();
            ASSERT_GE(ours, 0) << "the accepted socket's handshake failed: " << accepted.get_last_ssl_error_string();
            ASSERT_GE(theirs, 0) << "the raw client's handshake failed";
            if (ours == 1 && theirs == 1)
                break;
            ASSERT_LT(steady::now(), deadline) << "the handshake did not complete";
            std::this_thread::sleep_for(1ms);
        }
    }

    /// A session saved from a first connection on a bare TLS socket that `end` ends -- and then lets go of -- resumes.
    template <typename End>
    void
    expect_resumption_after(End &&end) {
        RawContext           context{RawContext::role::server};
        qb::io::ssl::Session session{};
        {
            RawPeer                  peer{context, RawContext::role::server};
            qb::io::tcp::ssl::socket client;
            ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
            ASSERT_NO_FATAL_FAILURE(first_connection(peer, client, session));
            end(client);
        }
        RawPeer                  peer{context, RawContext::role::server};
        qb::io::tcp::ssl::socket client;
        client.resume(session);
        qb::io::ssl::free_session(session);
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
        static_cast<void>(client.disconnect());
        client.close();
    }

    /// A standalone client whose raw peer ends the TLS session FIRST -- its close_notify, its sending side shut down --
    /// and then reads to the end: what that peer read.
    TlsEnd
    peer_ends_a_client(int max_version) {
        TlsEnd end;
        [&] {
            RawContext context{RawContext::role::server, max_version};
            RawPeer    peer{context, RawContext::role::server};
            TlsClient  client;
            ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
            client.start();
            pump(20ms); // the client reads what the handshake left for it (TLS 1.3: the session tickets)
            ASSERT_EQ(peer.close_notify(), 0) << "the peer's own close_notify was not sent";
            ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
            ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
            EXPECT_EQ(client.reason, 0) << "the peer's close_notify must read as the peer's end of the connection";
            EXPECT_EQ(client.system_error, 0);
            EXPECT_EQ(closing(), 0u) << "a connection its peer ended is closed outright";
            end = peer.read_to_end();
        }();
        return end;
    }

    /// The same for a server session: the raw client ends the TLS session first and waits for the session's answer.
    TlsEnd
    peer_ends_a_session(int max_version) {
        TlsEnd end;
        [&] {
            RawContext context{RawContext::role::client, max_version};
            RawPeer    peer{context, RawContext::role::client};
            ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
            pump(20ms);
            ASSERT_EQ(state.destroyed, 0);
            ASSERT_EQ(peer.close_notify(), 0) << "the peer's own close_notify was not sent";
            ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
            ASSERT_TRUE(pump_while_not([&] { return state.destroyed == 1; })) << "the session did not end on its peer's close_notify";
            end = peer.read_to_end();
        }();
        return end;
    }

    /// The whole of a standalone client's own close, as its raw peer read it: `how` ends the connection.
    template <typename How>
    TlsEnd
    client_ends(How &&how, int max_version = 0) {
        RawContext context{RawContext::role::server, max_version};
        RawPeer    peer{context, RawContext::role::server};
        TlsClient  client;
        TlsEnd     end;
        [&] {
            ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
            client.start();
            client.publish(LAST);
            ASSERT_TRUE(pump_while_not([&] { return client.queued() == 0u; })) << "the client's last bytes were never written";
            how(client);
            ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; })) << "the client was never disposed";
            EXPECT_FALSE(client.transport().is_open());
            end = peer.read_to_end();
        }();
        return end;
    }

    /// The whole of a server session's own close, as its raw peer read it: `command` is what the peer asks for.
    TlsEnd
    session_ends(std::string_view command, RawPeer &peer) {
        TlsEnd end;
        [&] {
            ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
            ASSERT_TRUE(peer.write(std::string(command) + "\n"));
            end = peer.read_to_end();
        }();
        return end;
    }
};

/**
 * @brief One octet from the server, read by the client: nothing is left unread on the client's side.
 * @details A TLS 1.3 server sends its session tickets after the handshake, and a socket driven by hand has not read
 *          them. A direct `disconnect()` is a TCP shutdown in both directions: with input unread it is a reset on
 *          Windows, which destroys what the peer has not read yet -- the alert with the rest. The io objects read
 *          them in their first dispatch, and their own close goes through the loop.
 */
void
settle(RawPeer &peer, qb::io::tcp::ssl::socket &client) {
    ASSERT_TRUE(peer.write("x"));
    char       octet    = 0;
    const auto deadline = steady::now() + 5s;
    int        got      = 0;
    while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(got, 1) << "the client never read the server's octet";
}

/// The first connection of a resumption case: settled, and the session saved while the connection is up.
void
first_connection(RawPeer &peer, qb::io::tcp::ssl::socket &client, qb::io::ssl::Session &session) {
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    session = client.get_session();
    ASSERT_TRUE(session.is_valid());
}

/// One alert, then the FIN, and nothing else: how a stream this side ended reads to an OpenSSL peer.
#define EXPECT_CLEAN_END(end, last_bytes)                                                             \
    do {                                                                                              \
        EXPECT_EQ((end).received, (last_bytes)) << "the peer did not read this side's last bytes";    \
        EXPECT_TRUE((end).clean()) << "the peer read no close_notify: " << (end).text();              \
        EXPECT_EQ((end).octets_after, 0u) << "something followed the close_notify: " << (end).text(); \
        EXPECT_TRUE((end).fin) << "no FIN followed the close_notify: " << (end).text();               \
    } while (false)

} // namespace tls_close_notify_test

using namespace tls_close_notify_test;

// ─────────────────────────────── a standalone object ends its connection ───────────────────────────────

TEST_F(TlsCloseNotify, AClientThatDisconnectsSendsCloseNotifyBeforeTheFin) {
    const TlsEnd end = client_ends([](TlsClient &client) { client.disconnect(); });
    note("client disconnect()", end);
    EXPECT_CLEAN_END(end, LAST);
}

TEST_F(TlsCloseNotify, AClientThatDisconnectsSendsCloseNotifyOverTls12) {
    const TlsEnd end = client_ends([](TlsClient &client) { client.disconnect(); }, TLS1_2_VERSION);
    note("client disconnect(), TLS 1.2", end);
    EXPECT_CLEAN_END(end, LAST);
}

TEST_F(TlsCloseNotify, AClientThatDisconnectsNowSendsCloseNotifyBeforeTheFin) {
    const TlsEnd end = client_ends([](TlsClient &client) { client.drop_now(); });
    note("client disconnect_now()", end);
    EXPECT_CLEAN_END(end, LAST);
}

// close_after_deliver() on a standalone object: the delivery first, then the alert, then the descriptor goes to the loop.
TEST_F(TlsCloseNotify, AClientThatClosesAfterDeliverSendsCloseNotifyBeforeTheFin) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    client.publish(LAST);
    client.close_after_deliver(); // asked for while the last bytes are still queued: they are delivered first
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; })) << "the client was never disposed";
    const TlsEnd end = peer.read_to_end();
    note("client close_after_deliver()", end);
    EXPECT_CLEAN_END(end, LAST);
}

// A linger of zero is "do not wait for the peer", not "cut the stream": what was written is ended properly, then the
// descriptor is closed in the call.
TEST_F(TlsCloseNotify, AClientThatClosesAtOnceStillSendsCloseNotify) {
    const TlsEnd end = client_ends([](TlsClient &client) {
        client.set_linger(qb::duration::zero());
        client.disconnect();
    });
    note("client disconnect(), linger zero", end);
    EXPECT_EQ(closing(), 0u) << "a linger of zero left the descriptor to the loop";
    EXPECT_CLEAN_END(end, LAST);
}

// "At once" with the peer's bytes UNREAD is a reset -- `set_linger(zero)` says so -- and what the peer had received
// and not read yet is its TCP stack's to keep or to destroy: Linux keeps it (the peer reads the alert, then the
// reset), Windows destroys it (the peer reads the reset alone). Either way the peer is never shown a FIN with no
// alert: the stream ends cleanly or it ends on an error.
TEST_F(TlsCloseNotify, AnAtOnceCloseWithInputUnreadIsAlertedOrReset) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.set_linger(qb::duration::zero());
    client.start();
    pump(20ms);
    ASSERT_TRUE(peer.write("unread\n"));
    std::this_thread::sleep_for(20ms); // it is in the client's socket, and no loop pass reads it:
    client.disconnect();               // the dispatch this feeds disposes at once
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_EQ(client.messages, 0u) << "the case proves nothing: the client read the peer's bytes";
    const TlsEnd end = peer.read_to_end();
    note("client disconnect(), linger zero, input unread", end);
    EXPECT_FALSE(end.unexpected_eof()) << "the peer read an end of stream with no alert: " << end.text();
#if defined(__linux__)
    EXPECT_TRUE(end.clean()) << "Linux keeps what the peer had received: the alert, written before the reset: " << end.text();
    EXPECT_FALSE(end.fin) << "the case proves nothing: the close with input unread was not a reset: " << end.text();
#else
    EXPECT_TRUE(end.clean() || end.error == SSL_ERROR_SYSCALL) << end.text();
#endif
}

// A size guard closes at once too, and the peer is the one that sent too much: it is told all the same, when what
// this side had to say was written.
TEST_F(TlsCloseNotify, AClientItsSizeGuardEndsStillSendsCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.set_max_message_size(16);
    client.start();
    ASSERT_TRUE(peer.write(std::string(512, 'x') + "\n"));
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_EQ(client.reason, static_cast<int>(qb::io::async::event::disconnect_reason::message_too_large));
    EXPECT_EQ(closing(), 0u) << "a size guard left the descriptor to the loop";
    const TlsEnd end = peer.read_to_end();
    note("client message_too_large", end);
    EXPECT_CLEAN_END(end, "");
}

// The destructor of an object that is still connected closes at once, as before -- and ends the TLS stream first.
TEST_F(TlsCloseNotify, AClientDestroyedWhileConnectedSendsCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    auto       client = std::make_unique<TlsClient>();
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client->transport()));
    client->start();
    client->publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return client->queued() == 0u; }));
    client.reset();
    const TlsEnd end = peer.read_to_end();
    note("client destroyed while connected", end);
    EXPECT_CLEAN_END(end, LAST);
}

// The two one-way bases take the same path: an `output<>` object that wrote everything it was given...
TEST_F(TlsCloseNotify, AnOutputObjectThatDisconnectsSendsCloseNotifyBeforeTheFin) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsOutput  object;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, object.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, object.transport()));
    object.start();
    object.publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return object.pendingWrite() == 0u; }));
    object.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return object.disconnected_events == 1u; }));
    const TlsEnd end = peer.read_to_end();
    note("output<> disconnect()", end);
    EXPECT_CLEAN_END(end, LAST);
}

// ...and one that drops what it still had queued: no alert.
TEST_F(TlsCloseNotify, AnOutputObjectThatDropsQueuedOutputSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsOutput  object;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, object.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, object.transport()));
    object.start();
    pump(20ms);
    ASSERT_EQ(object.disconnected_events, 0u) << "the case proves nothing: the object ended before it queued anything";
    object.publish(std::string(CUT, 'c'));
    object.disconnect(); // the dispatch this feeds disposes before any write
    ASSERT_TRUE(pump_while_not([&] { return object.disconnected_events == 1u; }));
    EXPECT_EQ(object.pendingWrite(), CUT) << "the case proves nothing: nothing was left queued";
    const TlsEnd end = peer.read_to_end();
    note("output<> disconnect() with output queued", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// ...whatever its hook does with the queue afterwards.
TEST_F(TlsCloseNotify, AnOutputObjectWhoseHookClearsItsQueueSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsOutput  object;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, object.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, object.transport()));
    object.clear_in_hook = true;
    object.start();
    pump(20ms);
    ASSERT_EQ(object.disconnected_events, 0u) << "the case proves nothing: the object ended before it queued anything";
    object.publish(std::string(CUT, 'c'));
    object.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return object.disconnected_events == 1u; }));
    EXPECT_EQ(object.pendingWrite(), 0u) << "the case proves nothing: the hook left the queue as it was";
    const TlsEnd end = peer.read_to_end();
    note("output<> disconnect(), its hook clears the queue", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, AnOutputObjectWhoseHookClosesItsTransportWithOutputQueuedSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsOutput  object;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, object.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, object.transport()));
    object.close_in_hook = true;
    object.start();
    pump(20ms);
    ASSERT_EQ(object.disconnected_events, 0u) << "the case proves nothing: the object ended before it queued anything";
    object.publish(std::string(CUT, 'c'));
    object.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return object.disconnected_events == 1u; }));
    EXPECT_EQ(object.pendingWrite(), CUT) << "the case proves nothing: nothing was left queued";
    const TlsEnd end = peer.read_to_end();
    note("output<> disconnect() with output queued, its hook closes the transport", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, AnOutputObjectWhoseHookQueuesOutputSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsOutput  object;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, object.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, object.transport()));
    object.queue_in_hook = true;
    object.start();
    pump(20ms);
    ASSERT_EQ(object.disconnected_events, 0u);
    object.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return object.disconnected_events == 1u; }));
    EXPECT_EQ(object.pendingWrite(), LAST.size()) << "the case proves nothing: the hook queued nothing";
    const TlsEnd end = peer.read_to_end();
    note("output<> disconnect(), its hook queues output", end);
    EXPECT_TRUE(end.received.empty());
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// An `input<>` object has no output to drop: its own end of the connection is always a whole stream's.
TEST_F(TlsCloseNotify, AnInputObjectThatDisconnectsSendsCloseNotifyBeforeTheFin) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsInput   object;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, object.transport()));
    ASSERT_NE(object.switch_protocol<NoMessage>(object), nullptr);
    object.start();
    pump(20ms);
    ASSERT_EQ(object.disconnected_events, 0u) << "the case proves nothing: the object ended before its disconnect()";
    object.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return object.disconnected_events == 1u; }));
    const TlsEnd end = peer.read_to_end();
    note("input<> disconnect()", end);
    EXPECT_CLEAN_END(end, "");
}

// The socket alone, no io object: disconnect() is the call that ends a TLS connection.
TEST_F(TlsCloseNotify, ASocketThatDisconnectsSendsCloseNotifyBeforeTheFin) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_EQ(client.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size()));
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("ssl::socket::disconnect()", end);
    EXPECT_CLEAN_END(end, LAST);
    client.close();
}

// A connection upgraded to TLS after a plain-text exchange (`init_client()`: STARTTLS) ends as any other.
TEST_F(TlsCloseNotify, ASocketUpgradedToTlsSendsCloseNotifyToo) {
    RawContext          context{RawContext::role::server};
    RawPeer             peer{context, RawContext::role::server};
    qb::io::tcp::socket plain;
    ASSERT_EQ(plain.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
    ASSERT_EQ(listener.accept(peer.tcp), qb::io::SocketStatus::Done);
    qb::io::tcp::ssl::socket client(nullptr, plain);
    client.set_insecure();
    ASSERT_EQ(client.init_client("localhost"), 0);
    ASSERT_EQ(client.set_nonblocking(true), 0);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    const auto deadline = steady::now() + 5s;
    for (;;) {
        const int ours   = client.handshake_status();
        const int theirs = peer.handshake_step();
        ASSERT_GE(ours, 0);
        ASSERT_GE(theirs, 0);
        if (ours == 1 && theirs == 1)
            break;
        ASSERT_LT(steady::now(), deadline) << "the handshake did not complete";
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("init_client() upgrade, disconnect()", end);
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// ─────────────────────────────── a server session ends its connection ───────────────────────────────

// disconnect() with everything written: the server releases the session at once and the loop closes the descriptor
// after the peer -- the alert leaves before the descriptor does.
TEST_F(TlsCloseNotify, ASessionThatDisconnectsSendsCloseNotifyBeforeTheFin) {
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("bye", peer);
    note("session disconnect()", end);
    EXPECT_CLEAN_END(end, LAST);
    EXPECT_EQ(state.destroyed, 1);
}

// close_after_deliver(): the alert goes after the delivered bytes and before the half-close, and the session then
// waits for its peer. A peer that answers with its own close_notify has ended the stream: the session is released
// at once, not at linger().
TEST_F(TlsCloseNotify, ASessionThatClosesAfterDeliverSendsCloseNotifyAndEndsOnThePeersOwn) {
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("cad", peer);
    note("session close_after_deliver()", end);
    EXPECT_CLEAN_END(end, LAST);
    EXPECT_EQ(state.destroyed, 0) << "the session did not wait for its peer";
    const auto answered = steady::now();
    static_cast<void>(peer.close_notify()); // and the TCP connection stays open
    EXPECT_TRUE(pump_while_not([&] { return state.destroyed == 1; }, 2000ms))
        << "the peer's close_notify did not end the session's linger (" << ms_since(answered) << " ms of a 20 s bound)";
}

TEST_F(TlsCloseNotify, ASessionThatClosesAtOnceStillSendsCloseNotify) {
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("now", peer);
    note("session disconnect(), linger zero", end);
    EXPECT_CLEAN_END(end, LAST);
    EXPECT_EQ(closing(), 0u) << "a linger of zero left the descriptor to the loop";
}

// A session its size guard ends closes with the session, at once; what it had to say was written: it says so.
TEST_F(TlsCloseNotify, ASessionItsSizeGuardEndsStillSendsCloseNotify) {
    state.max_message_size = 16;
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends(std::string(512, 'x'), peer);
    note("session message_too_large", end);
    EXPECT_CLEAN_END(end, "");
    EXPECT_EQ(state.destroyed, 1);
    EXPECT_EQ(closing(), 0u) << "a size guard left the descriptor to the loop";
}

TEST_F(TlsCloseNotify, ASessionThatClosesAfterDeliverSendsCloseNotifyOverTls12) {
    RawContext   context{RawContext::role::client, TLS1_2_VERSION};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("cad", peer);
    note("session close_after_deliver(), TLS 1.2", end);
    EXPECT_CLEAN_END(end, LAST);
}

// The session's linger reads its peer through TLS after its own alert. A TLS 1.3 peer may still update its keys and
// ask the session to update its own: the request is processed, never answered -- the session writes nothing after its
// close_notify -- and the linger goes on to the peer's end.
TEST_F(TlsCloseNotify, APeersKeyUpdateDuringTheLingerDoesNotEndIt) {
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("cad", peer);
    EXPECT_CLEAN_END(end, LAST);
    ASSERT_EQ(state.destroyed, 0);
    ASSERT_EQ(SSL_version(peer.ssl), TLS1_3_VERSION);
    ASSERT_EQ(SSL_key_update(peer.ssl, SSL_KEY_UPDATE_REQUESTED), 1);
    ASSERT_TRUE(peer.write("after the key update\n")); // the KeyUpdate leaves with it, then data under the new key
    pump(100ms);
    EXPECT_EQ(state.destroyed, 0) << "a key update, or what followed it, ended the session's linger";
    static_cast<void>(peer.close_notify());
    EXPECT_TRUE(pump_while_not([&] { return state.destroyed == 1; }, 2000ms)) << "the peer's close_notify did not end the linger";
}

// A lingering session has ended its stream: output published to it afterwards -- a broadcast that reaches it, a late
// reply -- cannot be sent. Its write fails and the session is released at once, as before the alert existed. OpenSSL
// refuses that write with no system call, so the thread's last socket error is what an earlier read left -- a
// would-block, after any read that found an incomplete record: the io layer must not read it as "try again", for
// ever, on a socket that stays writable. What the read leaves is the system's (a would-block on Linux; Windows may
// have reset it by the time of the write): the case that decides on every system is the next one, at the socket.
TEST_F(TlsCloseNotify, ASessionPublishedToWhileItLingersIsReleasedAtOnce) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    ASSERT_TRUE(peer.write("cad\n"));
    const TlsEnd end = peer.read_to_end(2000ms, LAST.size());
    ASSERT_EQ(end.received, LAST);
    pump(50ms);
    ASSERT_EQ(state.destroyed, 0) << "the case proves nothing: the session did not linger";
    const char head[3] = {0x17, 0x03, 0x03}; // the first octets of a record: the session's read finds no whole one
    ASSERT_EQ(qb::io::socket::send(peer.tcp.native_handle(), head, 3), 3);
    pump(20ms);
    ASSERT_EQ(state.destroyed, 0) << "the case proves nothing: an incomplete record ended the linger";
    const auto before = steady::now();
    server->stream("late\n"); // a broadcast: every session of the server is published to
    EXPECT_TRUE(pump_while_not([&] { return state.destroyed == 1; }, 3000ms))
        << "the session was never released: its write was armed again and again";
    EXPECT_LT(ms_since(before), 1000.0) << "the session was held " << ms_since(before) << " ms (its bound is 20 s)";
}

// The same at the socket, where it is decided: a write after this side's close_notify is an error whatever an earlier
// call left as the last socket error...
TEST_F(TlsCloseNotify, AWriteAfterCloseNotifyFailsAndIsNoWouldBlock) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_TRUE(send_close_notify(client));
    qb::io::socket::set_last_errno(EWOULDBLOCK); // what a read that found nothing leaves behind
    EXPECT_EQ(client.write("x", 1), -1) << "a write after the end of the stream was taken";
    EXPECT_FALSE(qb::io::socket::not_send_error(qb::io::socket::get_last_errno()))
        << "a write OpenSSL refused reads as would-block (" << qb::io::socket::get_last_errno()
        << "): the io layer would try it again for ever";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// ...and so is a read after the peer's close_notify was read: OpenSSL answers it at once, with no system call, and
// it is an end of stream with no error, as a plain socket reports its EOF -- never a would-block.
TEST_F(TlsCloseNotify, AReadAfterThePeersCloseNotifyIsAnEndAndNoWouldBlock) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_EQ(peer.close_notify(), 0);
    char       octet    = 0;
    int        got      = 0;
    const auto deadline = steady::now() + 5s;
    while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(got, -1) << "the client never read the peer's close_notify";
    ASSERT_NE(SSL_get_shutdown(client.ssl_handle()) & SSL_RECEIVED_SHUTDOWN, 0)
        << "the case proves nothing: the read did not end on the peer's alert";
    qb::io::socket::set_last_errno(EWOULDBLOCK); // what any read that found nothing leaves behind
    EXPECT_EQ(client.read(&octet, 1), -1);
    EXPECT_FALSE(qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
        << "the end of the stream reads as would-block (" << qb::io::socket::get_last_errno() << "): the io layer would wait for ever";
    static_cast<void>(client.disconnect());
    client.close();
}

// ─────────────────────────────── the peer ended the session first: it is answered ───────────────────────────────
// Each side sends its close_notify before it closes its sending direction (RFC 8446 6.1), whoever was first. A peer
// that sent its own and still reads -- OpenSSL's two-step shutdown -- reads this side's alert, then the FIN.

TEST_F(TlsCloseNotify, APeersCloseNotifyIsAnswered) {
    const TlsEnd end = peer_ends_a_client(0);
    note("peer-ended (its close_notify), the client answers", end);
    EXPECT_CLEAN_END(end, "");
}

TEST_F(TlsCloseNotify, APeersCloseNotifyIsAnsweredOverTls12) {
    const TlsEnd end = peer_ends_a_client(TLS1_2_VERSION);
    note("peer-ended (its close_notify), the client answers, TLS 1.2", end);
    EXPECT_CLEAN_END(end, "");
}

TEST_F(TlsCloseNotify, ASessionAnswersItsPeersCloseNotify) {
    const TlsEnd end = peer_ends_a_session(0);
    note("peer-ended (its close_notify), the session answers", end);
    EXPECT_CLEAN_END(end, "");
}

TEST_F(TlsCloseNotify, ASessionAnswersItsPeersCloseNotifyOverTls12) {
    const TlsEnd end = peer_ends_a_session(TLS1_2_VERSION);
    note("peer-ended (its close_notify), the session answers, TLS 1.2", end);
    EXPECT_CLEAN_END(end, "");
}

// input<> has no queue: the peer's close_notify is answered by a standalone object, through end_transport...
TEST_F(TlsCloseNotify, AnInputObjectAnswersItsPeersCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsInput   object;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, object.transport()));
    ASSERT_NE(object.switch_protocol<NoMessage>(object), nullptr);
    object.start();
    pump(20ms);
    ASSERT_EQ(peer.close_notify(), 0);
    ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
    ASSERT_TRUE(pump_while_not([&] { return object.disconnected_events == 1u; }));
    EXPECT_EQ(closing(), 0u) << "a connection its peer ended is closed outright";
    const TlsEnd end = peer.read_to_end();
    note("input<> object, peer-ended (its close_notify), answered", end);
    EXPECT_CLEAN_END(end, "");
}

// ...and by a session, when its stream closes after its server released it.
TEST_F(TlsCloseNotify, AnInputSessionAnswersItsPeersCloseNotify) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    Registry   registry;
    auto       session = std::make_unique<TlsInputSession>(registry);
    ASSERT_NO_FATAL_FAILURE(accept_from(peer, session->transport()));
    ASSERT_NE(session->switch_protocol<NoMessageOf<TlsInputSession>>(*session), nullptr);
    session->start();
    pump(20ms);
    ASSERT_EQ(session->disconnected_events, 0u);
    ASSERT_EQ(peer.close_notify(), 0);
    ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
    ASSERT_TRUE(pump_while_not([&] { return registry.released == 1u; })) << "the session was never handed back";
    EXPECT_EQ(session->disconnected_events, 1u);
    session.reset(); // its owner lets it go: the stream closes the socket
    const TlsEnd end = peer.read_to_end();
    note("input<> session, peer-ended (its close_notify), answered", end);
    EXPECT_CLEAN_END(end, "");
}

// A peer that closed at once after its alert -- what most do: the answer meets a connection its peer has left. It
// costs nothing: the system takes the write and the peer's answers it with a reset nobody reads, no SIGPIPE is
// raised, the application is told the peer ended the connection, nothing is left in the thread's OpenSSL error
// queue, and both ends of the session are recorded (the session resumes: the cases below).
TEST_F(TlsCloseNotify, AnAnswerToAPeerThatClosedAtOnceCostsNothing) {
    for (const int version : {0, TLS1_2_VERSION}) {
        RawContext context{RawContext::role::server, version};
        RawPeer    peer{context, RawContext::role::server};
        TlsClient  client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        client.start();
        pump(20ms);
        SSL *const ssl = client.transport().ssl_handle();
        ASSERT_NE(ssl, nullptr);
        const auto written = BIO_number_written(SSL_get_wbio(ssl)); // what the client wrote so far: its handshake
        ASSERT_EQ(peer.close_notify(), 0);
        const SigpipeCount sigpipes;
        peer.tcp.close(); // at once: this side's answer is written to a connection its peer has left
        ERR_clear_error();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
        EXPECT_EQ(client.reason, 0) << "the peer's close_notify must read as the peer's end of the connection";
        EXPECT_EQ(ERR_peek_error(), 0ul) << "the answer left an error in the thread's queue";
        EXPECT_FALSE(client.transport().is_open());
        EXPECT_EQ(closing(), 0u);
        ASSERT_EQ(client.transport().ssl_handle(), ssl);
        EXPECT_GT(BIO_number_written(SSL_get_wbio(ssl)), written) << "the case proves nothing: no answer was written";
        EXPECT_EQ(SSL_get_shutdown(ssl), SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
        pump(20ms); // the loop goes on: nothing of that connection is left to wake it
        EXPECT_EQ(sigpipes.count(), 0) << "the answer raised SIGPIPE";
    }
}

// The peer RESETS the connection: OpenSSL reads a socket error (SSL_ERROR_SYSCALL), not an end of its stream. OpenSSL 3
// keeps the session as it was, OpenSSL 4 ends it there (kSocketErrorEndsTheSession) -- either way no alert may follow
// on a connection that is gone: nothing is written into it, and no SIGPIPE is raised. By a client, a session and the
// socket alone, the reset met by a read or by a write. The socket alone asserts which of the two OpenSSL did: with 3
// what stops the alert is the end the socket recorded when it met the reset, with 4 it is OpenSSL's own state.
TEST_F(TlsCloseNotify, AClientWhosePeerResetTheConnectionWritesNothingIntoIt) {
    for (const int version : {0, TLS1_2_VERSION}) {
        RawContext context{RawContext::role::server, version};
        RawPeer    peer{context, RawContext::role::server};
        TlsClient  client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        client.start();
        pump(20ms);
        ASSERT_NO_FATAL_FAILURE(count_write_attempts(client.transport().ssl_handle()));
        const SigpipeCount sigpipes;
        ASSERT_NO_FATAL_FAILURE(reset_connection(peer.tcp));
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
        EXPECT_NE(client.system_error, 0) << "the case proves nothing: the client read no reset";
        EXPECT_EQ(g_write_attempts, 0u) << "something was written into a connection its peer reset";
        EXPECT_EQ(sigpipes.count(), 0) << "a write into a connection its peer reset raised SIGPIPE";
        EXPECT_EQ(ERR_peek_error(), 0ul);
        EXPECT_FALSE(client.transport().is_open());
        EXPECT_EQ(closing(), 0u) << "a connection its peer reset is closed outright";
    }
}

TEST_F(TlsCloseNotify, ASessionWhosePeerResetTheConnectionWritesNothingIntoIt) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    pump(20ms);
    ASSERT_EQ(server->sessions().size(), 1u);
    ASSERT_NO_FATAL_FAILURE(count_write_attempts(server->sessions().begin()->second->transport().ssl_handle()));
    const SigpipeCount sigpipes;
    ASSERT_NO_FATAL_FAILURE(reset_connection(peer.tcp));
    ASSERT_TRUE(pump_while_not([&] { return state.destroyed == 1; })) << "the session did not end on its peer's reset";
    EXPECT_EQ(state.disconnected, 1);
    EXPECT_EQ(g_write_attempts, 0u) << "something was written into a connection its peer reset";
    EXPECT_EQ(sigpipes.count(), 0) << "a write into a connection its peer reset raised SIGPIPE";
}

TEST_F(TlsCloseNotify, ASocketThatReadAResetWritesNothingMore) {
    for (const int version : {0, TLS1_2_VERSION}) {
        RawContext               context{RawContext::role::server, version};
        RawPeer                  peer{context, RawContext::role::server};
        qb::io::tcp::ssl::socket client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        ASSERT_NO_FATAL_FAILURE(settle(peer, client));
        ASSERT_NO_FATAL_FAILURE(count_write_attempts(client.ssl_handle()));
        const SigpipeCount sigpipes;
        ASSERT_NO_FATAL_FAILURE(reset_connection(peer.tcp));
        char       octet    = 0;
        int        got      = 0;
        const auto deadline = steady::now() + 5s;
        while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
            std::this_thread::sleep_for(1ms);
        const int error = qb::io::socket::get_last_errno();
        ASSERT_EQ(got, -1) << "the socket never read the reset";
        EXPECT_FALSE(qb::io::socket::not_recv_error(error)) << "the case proves nothing: the read met no socket error";
        EXPECT_NE(error, 0) << "the read's socket error was not left for its caller";
        EXPECT_EQ(SSL_in_init(client.ssl_handle()), kSocketErrorEndsTheSession ? 1 : 0)
            << (kSocketErrorEndsTheSession ? "OpenSSL 4 kept a session it ends on a socket error"
                                           : "the case proves nothing: OpenSSL took the reset for a fatal error");
        EXPECT_FALSE(send_close_notify(client)) << "an alert was attempted after a reset";
        static_cast<void>(client.disconnect());
        EXPECT_EQ(g_write_attempts, 0u) << "something was written into a connection its peer reset";
        EXPECT_EQ(sigpipes.count(), 0) << "a write into a connection its peer reset raised SIGPIPE";
        EXPECT_EQ(ERR_peek_error(), 0ul);
        client.close();
    }
}

// A socket error the io layer reads as "try again" is not a reset: the io layer makes the call again. ENOBUFS on a
// write is one -- OpenSSL's socket BIO reports it as SSL_ERROR_SYSCALL, qb-io writes again (socket::not_send_error()).
// Whether the session goes on is OpenSSL's to say. OpenSSL 3 takes the write made again, and the stream ends whole.
// OpenSSL 4 ended the session on the error (kSocketErrorEndsTheSession): the write reports the session aborted
// (kSessionAborted), an error the io layer does not write again after -- it ends the session at once, never writes
// again for ever --; a write made all the same fails as well; nothing more is written into the connection, no alert
// either, and the peer reads the stream cut.
TEST_F(TlsCloseNotify, AWriteErrorTheIoLayerRetriesGoesOnUnlessOpenSslEndedTheSession) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_NO_FATAL_FAILURE(inject_once::install(client.ssl_handle(), true));
    inject_once::fail_write = ENOBUFS;
    ASSERT_EQ(client.write(LAST.data(), LAST.size()), -1) << "the case proves nothing: the write did not fail";
    ASSERT_EQ(inject_once::fail_write, 0) << "the case proves nothing: the injected error was not met";
    const int error = qb::io::socket::get_last_errno();
    EXPECT_EQ(SSL_in_init(client.ssl_handle()), kSocketErrorEndsTheSession ? 1 : 0)
        << (kSocketErrorEndsTheSession ? "OpenSSL 4 kept a session it ends on a socket error"
                                       : "OpenSSL 3 ended the session on a socket error");
    EXPECT_EQ(in_error_state(client.ssl_handle()), kSocketErrorEndsTheSession) << "OpenSSL's error state is not as the major says";
    ERR_clear_error();
    ASSERT_NO_FATAL_FAILURE(count_write_attempts(client.ssl_handle()));
    if (!kSocketErrorEndsTheSession) {
        EXPECT_TRUE(qb::io::socket::not_send_error(error)) << "the io layer would not write again (" << error << ")";
        EXPECT_EQ(client.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size())) << "the write the io layer makes again failed";
        EXPECT_TRUE(send_close_notify(client)) << "a stream whose output was all written could not end whole";
        static_cast<void>(client.disconnect());
        const TlsEnd end = peer.read_to_end();
        note("ssl::socket: one write refused with ENOBUFS, written again, disconnect()", end);
        EXPECT_CLEAN_END(end, LAST);
    } else {
        EXPECT_EQ(error, kSessionAborted) << "a session OpenSSL ended on ENOBUFS was not reported aborted (" << error << ")";
        qb::io::socket::set_last_errno(EWOULDBLOCK); // what any call that found nothing leaves behind
        EXPECT_EQ(client.write(LAST.data(), LAST.size()), -1) << "a write was taken into a session OpenSSL ended";
        EXPECT_FALSE(qb::io::socket::not_send_error(qb::io::socket::get_last_errno()))
            << "a write made again failed with an error that reads as try again (" << qb::io::socket::get_last_errno() << ")";
        EXPECT_FALSE(send_close_notify(client)) << "an alert was reported into a session OpenSSL ended";
        static_cast<void>(client.disconnect());
        EXPECT_EQ(g_write_attempts, 0u) << "something was written into a session OpenSSL ended";
        EXPECT_EQ(ERR_peek_error(), 0ul);
        const TlsEnd end = peer.read_to_end();
        note("ssl::socket: one write refused with ENOBUFS, the session ended by OpenSSL, disconnect()", end);
        EXPECT_EQ(end.received, "") << "the peer read bytes of a write OpenSSL refused";
        EXPECT_TRUE(end.unexpected_eof()) << "a session OpenSSL ended was not ended as a cut stream: " << end.text();
    }
    client.close();
}

// A handshake message in flight puts OpenSSL in init without ending anything: a peer's KeyUpdate request is answered
// with this side's next write. A socket error the io layer writes again after, met by that write, ends nothing -- with
// OpenSSL 3 and with OpenSSL 4 alike (measured: 3.5.7, 3.5.9, 3.6.3, 4.1.0), unlike the same error on an idle
// OpenSSL 4 session --: the write made again goes on and the stream ends whole. Being in init is not being ended, and
// the session must not be reported aborted for it.
TEST_F(TlsCloseNotify, AWriteErrorWhileAKeyUpdateIsAnsweredGoesOn) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_EQ(SSL_version(client.ssl_handle()), TLS1_3_VERSION) << "the case proves nothing: no KeyUpdate before TLS 1.3";
    ASSERT_EQ(SSL_key_update(peer.ssl, SSL_KEY_UPDATE_REQUESTED), 1);
    ASSERT_TRUE(peer.write("k")); // the request leaves with it
    char       octet    = 0;
    int        got      = 0;
    const auto deadline = steady::now() + 5s;
    while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(got, 1) << "the client never read the octet behind the KeyUpdate request";
    ASSERT_NO_FATAL_FAILURE(inject_once::install(client.ssl_handle(), true));
    inject_once::fail_write = ENOBUFS;
    ASSERT_EQ(client.write(LAST.data(), LAST.size()), -1) << "the case proves nothing: the write did not fail";
    ASSERT_EQ(inject_once::fail_write, 0) << "the case proves nothing: the injected error was not met";
    const int error = qb::io::socket::get_last_errno();
    EXPECT_EQ(SSL_in_init(client.ssl_handle()), 1) << "the case proves nothing: no handshake message was in flight";
    EXPECT_FALSE(in_error_state(client.ssl_handle())) << "OpenSSL ended the session in a KeyUpdate";
    EXPECT_TRUE(qb::io::socket::not_send_error(error)) << "a session OpenSSL keeps was reported ended (" << error << ")";
    EXPECT_EQ(client.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size())) << "the write the io layer makes again failed";
    EXPECT_TRUE(send_close_notify(client)) << "a stream whose output was all written could not end whole";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("ssl::socket: a KeyUpdate answered, one write refused with ENOBUFS, written again, disconnect()", end);
    EXPECT_CLEAN_END(end, LAST);
    client.close();
}

// ...and a read, while a renegotiation the peer asked for is answered (TLS 1.2): the client's read sent its ClientHello
// and waits in init for the ServerHello when a socket error the io layer reads again after meets it. Whether that ends
// the session is OpenSSL's state to say (OpenSSL 3 keeps it: measured, 3.5.7 and 3.6.3), and the socket follows it: a
// session kept goes on -- the read made again completes the renegotiation and the stream ends whole --, a session ended
// is reported aborted and the peer reads it cut. Which one OpenSSL 4 does is printed.
TEST_F(TlsCloseNotify, AReadErrorWhileARenegotiationIsAnsweredFollowsOpenSsl) {
    RawContext               context{RawContext::role::server, TLS1_2_VERSION};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_EQ(SSL_renegotiate(peer.ssl), 1) << "the peer could not ask for a renegotiation";
    ERR_clear_error();
    ASSERT_EQ(SSL_do_handshake(peer.ssl), 1) << "the peer's HelloRequest was not sent"; // sent; its ClientHello awaited
    char       octet    = 0;
    const auto deadline = steady::now() + 5s;
    while (!SSL_in_init(client.ssl_handle()) && steady::now() < deadline) {
        ASSERT_EQ(client.read(&octet, 1), 0) << "the client read something other than the HelloRequest";
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(SSL_in_init(client.ssl_handle()), 1) << "the case proves nothing: the client never answered the request";
    ASSERT_NO_FATAL_FAILURE(inject_once::install(client.ssl_handle(), false));
    inject_once::fail_read = EINTR;
    ASSERT_EQ(client.read(&octet, 1), -1) << "the case proves nothing: the read did not fail";
    ASSERT_EQ(inject_once::fail_read, 0) << "the case proves nothing: the injected error was not met";
    const int  error = qb::io::socket::get_last_errno();
    const bool ended = in_error_state(client.ssl_handle());
    std::cout << "[  OPENSSL ] " << OpenSSL_version(OPENSSL_VERSION) << ": a read error in a renegotiation "
              << (ended ? "ended the session" : "kept the session") << '\n';
    if (!kSocketErrorEndsTheSession) {
        EXPECT_FALSE(ended) << "OpenSSL 3 ended a session in a renegotiation";
    }
    if (!ended) {
        EXPECT_TRUE(qb::io::socket::not_recv_error(error)) << "a session OpenSSL keeps was reported ended (" << error << ")";
        // The renegotiation completes as the two sides read: the client's reads made again, the peer's reads.
        char       sink    = 0;
        const auto settled = steady::now() + 5s;
        while ((SSL_in_init(client.ssl_handle()) || SSL_in_init(peer.ssl)) && steady::now() < settled) {
            static_cast<void>(client.read(&octet, 1));
            ERR_clear_error();
            static_cast<void>(SSL_read(peer.ssl, &sink, 1));
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_EQ(SSL_in_init(client.ssl_handle()), 0) << "the renegotiation never completed on the client";
        ASSERT_EQ(SSL_in_init(peer.ssl), 0) << "the renegotiation never completed on the peer";
        ASSERT_TRUE(peer.write("z"));
        int        got  = 0;
        const auto read = steady::now() + 5s;
        while ((got = client.read(&octet, 1)) == 0 && steady::now() < read)
            std::this_thread::sleep_for(1ms);
        EXPECT_EQ(got, 1) << "the session did not go on after the renegotiation";
        EXPECT_EQ(octet, 'z');
        EXPECT_TRUE(send_close_notify(client)) << "a stream whose output was all written could not end whole";
        static_cast<void>(client.disconnect());
        const TlsEnd end = peer.read_to_end();
        note("ssl::socket: a renegotiation answered, one read refused with EINTR, read again, disconnect()", end);
        EXPECT_CLEAN_END(end, "");
    } else {
        EXPECT_EQ(error, kSessionAborted) << "a session OpenSSL ended on EINTR was not reported aborted (" << error << ")";
        qb::io::socket::set_last_errno(EWOULDBLOCK);
        EXPECT_EQ(client.read(&octet, 1), -1) << "the case proves nothing: OpenSSL answered a read of a session it ended";
        static_cast<void>(client.disconnect());
        const TlsEnd end = peer.read_to_end();
        note("ssl::socket: a renegotiation answered, one read refused with EINTR, the session ended by OpenSSL", end);
        EXPECT_NE(end.error, SSL_ERROR_WANT_READ) << "the peer read no end of the connection: " << end.text();
        EXPECT_FALSE(end.clean()) << "a session OpenSSL ended was ended as a whole stream: " << end.text();
    }
    client.close();
}

/// How the peer reads the end of a connection this side shuts down, or closes, with input it never read and no alert
/// sent: a FIN on Linux and macOS -- its OpenSSL reads an unexpected EOF -- and a reset on Windows (measured: the
/// at-once close with input unread above, `SSL_ERROR_SYSCALL` and 10054, and
/// `TCPSocket.AShutdownOfBothDirectionsWithInputUnreadEndsAsTheSystemsOwn`).
static bool
cut_with_input_unread(TlsEnd const &end) {
#if defined(_WIN32)
    return end.error == SSL_ERROR_SYSCALL && end.tcp_error != 0;
#else
    return end.unexpected_eof();
#endif
}

// ...and so is an EINTR OpenSSL reports on a read (its socket BIO takes a real one for "try again" itself and reports
// SSL_ERROR_WANT_READ: this is the contract of the layer, kept the same both ways). OpenSSL 3: the read made again
// delivers. OpenSSL 4: the read reports the session aborted, and a read made all the same fails as well.
TEST_F(TlsCloseNotify, AReadErrorTheIoLayerRetriesGoesOnUnlessOpenSslEndedTheSession) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_NO_FATAL_FAILURE(inject_once::install(client.ssl_handle(), false));
    ASSERT_TRUE(peer.write("y"));
    // Read once the record is here, as the io layer reads: with OpenSSL 4 it is never read, and how the peer reads the
    // end must not depend on whether the record reached this side before the shutdown below.
    ASSERT_GT(qb::io::socket::handle_read_ready(client.native_handle(), 5s), 0) << "the peer's record never arrived";
    inject_once::fail_read = EINTR;
    char octet             = 0;
    ASSERT_EQ(client.read(&octet, 1), -1) << "the case proves nothing: the read did not fail";
    ASSERT_EQ(inject_once::fail_read, 0) << "the case proves nothing: the injected error was not met";
    const int error = qb::io::socket::get_last_errno();
    EXPECT_EQ(SSL_in_init(client.ssl_handle()), kSocketErrorEndsTheSession ? 1 : 0)
        << (kSocketErrorEndsTheSession ? "OpenSSL 4 kept a session it ends on a socket error"
                                       : "OpenSSL 3 ended the session on a socket error");
    EXPECT_EQ(in_error_state(client.ssl_handle()), kSocketErrorEndsTheSession) << "OpenSSL's error state is not as the major says";
    if (!kSocketErrorEndsTheSession) {
        EXPECT_TRUE(qb::io::socket::not_recv_error(error)) << "the io layer would not read again (" << error << ")";
        int        got      = 0;
        const auto deadline = steady::now() + 5s;
        while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
            std::this_thread::sleep_for(1ms);
        EXPECT_EQ(got, 1) << "the read the io layer makes again failed";
        EXPECT_EQ(octet, 'y');
        EXPECT_TRUE(send_close_notify(client)) << "a stream whose output was all written could not end whole";
        static_cast<void>(client.disconnect());
        const TlsEnd end = peer.read_to_end();
        note("ssl::socket: one read refused with EINTR, read again, disconnect()", end);
        EXPECT_CLEAN_END(end, "");
    } else {
        EXPECT_EQ(error, kSessionAborted) << "a session OpenSSL ended on EINTR was not reported aborted (" << error << ")";
        qb::io::socket::set_last_errno(EWOULDBLOCK); // what any read that found nothing leaves behind
        EXPECT_EQ(client.read(&octet, 1), -1) << "a read was answered from a session OpenSSL ended";
        EXPECT_FALSE(qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
            << "a read made again failed with an error that reads as try again (" << qb::io::socket::get_last_errno() << ")";
        ASSERT_NO_FATAL_FAILURE(count_write_attempts(client.ssl_handle()));
        EXPECT_FALSE(send_close_notify(client)) << "an alert was reported into a session OpenSSL ended";
        static_cast<void>(client.disconnect());
        EXPECT_EQ(g_write_attempts, 0u) << "something was written into a session OpenSSL ended";
        EXPECT_EQ(ERR_peek_error(), 0ul);
        const TlsEnd end = peer.read_to_end();
        note("ssl::socket: one read refused with EINTR, the session ended by OpenSSL, disconnect()", end);
        EXPECT_TRUE(cut_with_input_unread(end)) << "a session OpenSSL ended was not ended as a cut stream: " << end.text();
    }
    client.close();
}

// The same through the io layer, where "try again" decides whether a session is ever released: a client whose read
// fails once with an error the io layer reads again after. OpenSSL 3: the read made again delivers the message and the
// session goes on. OpenSSL 4: the session is over, and the client is released at once with the session reported
// aborted -- its descriptor stays readable (the peer's bytes are still in it), so a failure that read as "try again"
// would be read again on every loop pass.
TEST_F(TlsCloseNotify, AClientWhoseReadFailsOnceGoesOnUnlessOpenSslEndedTheSession) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms); // the client reads what the handshake left for it (TLS 1.3: the session tickets)
    ASSERT_NO_FATAL_FAILURE(inject_once::install(client.transport().ssl_handle(), false));
    inject_once::fail_read = EINTR;
    ASSERT_TRUE(peer.write("hello\n"));
    const auto before = steady::now();
    if (!kSocketErrorEndsTheSession) {
        ASSERT_TRUE(pump_while_not([&] { return client.messages == 1u; })) << "the message after the read made again was never delivered";
        EXPECT_EQ(inject_once::fail_read, 0) << "the case proves nothing: no read failed";
        EXPECT_EQ(client.disconnected_events, 0u) << "a read the io layer makes again ended the session";
        client.disconnect();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
        const TlsEnd end = peer.read_to_end();
        EXPECT_CLEAN_END(end, "");
    } else {
        EXPECT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }, 3000ms))
            << "the client was never released: its read is made again and again";
        EXPECT_LT(ms_since(before), 1000.0) << "the client was held " << ms_since(before) << " ms";
        EXPECT_EQ(inject_once::fail_read, 0) << "the case proves nothing: no read failed";
        EXPECT_EQ(client.messages, 0u);
        EXPECT_EQ(client.system_error, kSessionAborted) << "the session was not reported aborted (" << client.system_error << ")";
        const TlsEnd end = peer.read_to_end();
        note("client, one read refused with EINTR, the session ended by OpenSSL, system error " + std::to_string(client.system_error), end);
        EXPECT_TRUE(cut_with_input_unread(end)) << "a session OpenSSL ended was not ended as a cut stream: " << end.text();
    }
}

// ...and whose write fails once (ENOBUFS): OpenSSL 3 writes it again and the peer reads it; OpenSSL 4 ended the session,
// and the client is released at once with the session reported aborted, nothing written, the peer reading the stream cut.
TEST_F(TlsCloseNotify, AClientWhoseWriteFailsOnceGoesOnUnlessOpenSslEndedTheSession) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_NO_FATAL_FAILURE(inject_once::install(client.transport().ssl_handle(), true));
    inject_once::fail_write = ENOBUFS;
    const auto before       = steady::now();
    client.publish(LAST);
    if (!kSocketErrorEndsTheSession) {
        ASSERT_TRUE(pump_while_not([&] { return client.queued() == 0u; })) << "the write made again never left";
        EXPECT_EQ(inject_once::fail_write, 0) << "the case proves nothing: no write failed";
        EXPECT_EQ(client.disconnected_events, 0u) << "a write the io layer makes again ended the session";
        client.disconnect();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
        const TlsEnd end = peer.read_to_end();
        EXPECT_CLEAN_END(end, LAST);
    } else {
        EXPECT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }, 3000ms))
            << "the client was never released: its write is made again and again";
        EXPECT_LT(ms_since(before), 1000.0) << "the client was held " << ms_since(before) << " ms";
        EXPECT_EQ(inject_once::fail_write, 0) << "the case proves nothing: no write failed";
        EXPECT_EQ(client.system_error, kSessionAborted) << "the session was not reported aborted (" << client.system_error << ")";
        const TlsEnd end = peer.read_to_end();
        note("client, one write refused with ENOBUFS, the session ended by OpenSSL, system error " + std::to_string(client.system_error), end);
        EXPECT_EQ(end.received, "") << "the peer read bytes of a write OpenSSL refused";
        EXPECT_TRUE(end.unexpected_eof()) << "a session OpenSSL ended was not ended as a cut stream: " << end.text();
    }
}

// The reset met by a write: the write fails (and, unless the process ignores it, raises SIGPIPE itself, as any write
// into a reset connection does -- that is not the close's doing, and not counted here); nothing is written after it.
TEST_F(TlsCloseNotify, ASocketThatWroteIntoAResetWritesNothingMore) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_NO_FATAL_FAILURE(reset_connection(peer.tcp));
    int        got      = 0;
    const auto deadline = steady::now() + 5s;
    while ((got = client.write(LAST.data(), LAST.size())) >= 0 && steady::now() < deadline)
        std::this_thread::sleep_for(5ms); // a write the system took before the reset arrived
    ASSERT_EQ(got, -1) << "the socket never refused a write after its peer reset the connection";
    EXPECT_EQ(SSL_in_init(client.ssl_handle()), kSocketErrorEndsTheSession ? 1 : 0)
        << (kSocketErrorEndsTheSession ? "OpenSSL 4 kept a session it ends on a socket error"
                                       : "the case proves nothing: OpenSSL took the reset for a fatal error");
    ERR_clear_error(); // what the failed write left there is the write's: the close must add nothing
    ASSERT_NO_FATAL_FAILURE(count_write_attempts(client.ssl_handle()));
    const SigpipeCount sigpipes;
    EXPECT_FALSE(send_close_notify(client)) << "an alert was attempted after a reset";
    static_cast<void>(client.disconnect());
    EXPECT_EQ(g_write_attempts, 0u) << "something was written into a connection its peer reset";
    EXPECT_EQ(sigpipes.count(), 0) << "a write into a connection its peer reset raised SIGPIPE";
    EXPECT_EQ(ERR_peek_error(), 0ul);
    client.close();
}

// A peer that ended the session while this side still had output queued: the output is dropped, the stream is cut,
// and the answer is withheld -- the alert says the stream is whole.
TEST_F(TlsCloseNotify, APeersCloseNotifyIsNotAnsweredWhenOutputIsDropped) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    client.publish(std::string(CUT, 'c')); // more than one pass writes; the socket has room for an alert
    ASSERT_EQ(peer.close_notify(), 0);
    ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_NE(client.queued(), 0u) << "the case proves nothing: nothing was left queued";
    const TlsEnd end = peer.read_to_end();
    note("peer-ended (its close_notify), output still queued", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// ...and so is it when the output is queued by on(event::disconnected&&) itself: the stream is judged again after it.
TEST_F(TlsCloseNotify, APeersCloseNotifyIsNotAnsweredWhenTheHookQueuesOutput) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.disconnected_hook = [](TlsClient &self) {
        self.out() << LAST;
    };
    client.start();
    pump(20ms);
    ASSERT_EQ(peer.close_notify(), 0);
    ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_EQ(client.queued(), LAST.size()) << "the case proves nothing: the hook queued nothing";
    const TlsEnd end = peer.read_to_end();
    note("peer-ended (its close_notify), the hook queues output", end);
    EXPECT_TRUE(end.received.empty());
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// The peer's bare EOF -- no close_notify -- is a fatal error for OpenSSL 3: it holds no session to end, and nothing
// that says the stream is whole is written.
TEST_F(TlsCloseNotify, APeersBareEofIsNotAnswered) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    const TlsEnd end = peer.read_to_end();
    note("peer-ended (its bare EOF)", end);
    EXPECT_FALSE(end.clean()) << "a close_notify answered an end of stream with no alert: " << end.text();
}

// ─────────────────────────────── a session extracted with output queued ───────────────────────────────
// io_handler::extractSession() gives the transport to another owner; what the session still had queued dies with the
// session. Those octets are missing from the middle of the stream: it goes on, and its end must not say it is whole.

TEST_F(TlsCloseNotify, ASessionExtractedWithOutputQueuedLeavesAStreamThatEndsWithoutAlert) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    ASSERT_TRUE(peer.write("hold\n"));
    ASSERT_TRUE(pump_while_not([&] { return state.held != 0; })) << "the session never queued its reply";
    ASSERT_EQ(server->sessions().size(), 1u);
    const auto id = server->sessions().begin()->first;
    ASSERT_NE(server->sessions().begin()->second->out().size(), 0u) << "the case proves nothing: nothing is queued";
    auto [extracted, found] = server->extractSession(id);
    ASSERT_TRUE(found);
    EXPECT_EQ(state.destroyed, 1) << "the session outlived its extraction";
    qb::io::tcp::ssl::socket owner;
    owner = std::move(extracted); // the mark travels with the socket, by construction and by assignment
    ASSERT_TRUE(owner.is_open());
    ASSERT_EQ(owner.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size())) << "the stream must go on under its next owner";
    static_cast<void>(owner.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("session extracted with output queued, its next owner disconnect()s", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything the session queued was written";
    EXPECT_FALSE(end.clean()) << "a stream with octets missing was ended as a whole one: " << end.text();
    owner.close();
}

// ...and with nothing queued the stream is whole: its next owner ends it with the alert.
TEST_F(TlsCloseNotify, ASessionExtractedWithNothingQueuedLeavesAStreamThatEndsWhole) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    pump(20ms);
    ASSERT_EQ(server->sessions().size(), 1u);
    const auto id = server->sessions().begin()->first;
    ASSERT_EQ(server->sessions().begin()->second->out().size(), 0u);
    auto [extracted, found] = server->extractSession(id);
    ASSERT_TRUE(found);
    ASSERT_EQ(extracted.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size()));
    static_cast<void>(extracted.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("session extracted with nothing queued, its next owner disconnect()s", end);
    EXPECT_CLEAN_END(end, LAST);
    extracted.close();
}

// A session whose extracted hook empties its queue: what was queued when the extraction began dies with the session
// all the same -- the queue is read before the hook as well as after it.
TEST_F(TlsCloseNotify, ASessionWhoseExtractedHookEmptiesItsQueueLeavesAStreamThatEndsWithoutAlert) {
    state.clear_on_extract = true;
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    ASSERT_TRUE(peer.write("hold\n"));
    ASSERT_TRUE(pump_while_not([&] { return state.held != 0; })) << "the session never queued its reply";
    ASSERT_EQ(server->sessions().size(), 1u);
    const auto id      = server->sessions().begin()->first;
    auto       session = server->sessions().begin()->second;
    ASSERT_NE(session->out().size(), 0u) << "the case proves nothing: nothing is queued";
    auto [extracted, found] = server->extractSession(id);
    ASSERT_TRUE(found);
    EXPECT_EQ(state.extracted, 1) << "the session's extracted hook did not run";
    EXPECT_EQ(session->out().size(), 0u) << "the case proves nothing: the hook left the queue as it was";
    session.reset();
    ASSERT_EQ(extracted.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size())) << "the stream must go on";
    static_cast<void>(extracted.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("session extracted with output queued, its hook empties the queue, its next owner disconnect()s", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything the session queued was written";
    EXPECT_FALSE(end.clean()) << "a stream with octets missing was ended as a whole one: " << end.text();
    extracted.close();
}

// ...and so it does when that hook queues output: it dies with the session too, the queue is read after the hook.
TEST_F(TlsCloseNotify, ASessionWhoseExtractedHookQueuesOutputLeavesAStreamThatEndsWithoutAlert) {
    state.queue_on_extract = true;
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    pump(20ms);
    ASSERT_EQ(server->sessions().size(), 1u);
    const auto id      = server->sessions().begin()->first;
    auto       session = server->sessions().begin()->second;
    ASSERT_EQ(session->out().size(), 0u);
    auto [extracted, found] = server->extractSession(id);
    ASSERT_TRUE(found);
    EXPECT_EQ(state.extracted, 1) << "the session's extracted hook did not run";
    EXPECT_EQ(session->out().size(), LAST.size()) << "the case proves nothing: the hook queued nothing";
    session.reset();
    ASSERT_EQ(extracted.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size())) << "the stream must go on";
    static_cast<void>(extracted.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("session extracted, its hook queues output, its next owner disconnect()s", end);
    EXPECT_EQ(end.received, LAST) << "the case proves nothing: what the hook queued was written";
    EXPECT_FALSE(end.clean()) << "a stream with octets missing was ended as a whole one: " << end.text();
    extracted.close();
}

// The mark is one session's: the socket's next connection starts without it.
TEST_F(TlsCloseNotify, ASocketMarkedCutEndsWithoutAlertAndItsNextConnectionEndsWhole) {
    RawContext               context{RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    {
        RawPeer peer{context, RawContext::role::server};
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        ASSERT_NO_FATAL_FAILURE(settle(peer, client));
        ASSERT_TRUE(mark_cut(client)) << "this tree cannot mark a stream cut";
        ASSERT_EQ(client.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size())) << "a stream marked cut goes on";
        EXPECT_FALSE(send_close_notify(client)) << "a stream marked cut said it was whole";
        static_cast<void>(client.disconnect());
        const TlsEnd end = peer.read_to_end();
        note("ssl::socket marked cut, disconnect()", end);
        EXPECT_EQ(end.received, LAST);
        EXPECT_FALSE(end.clean()) << "a stream marked cut was ended as a whole one: " << end.text();
        client.close();
    }
    RawPeer peer{context, RawContext::role::server};
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("the same socket's next connection, disconnect()", end);
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// ...and so does another SSL handle given to the socket: the mark is the session's.
TEST_F(TlsCloseNotify, ASocketGivenAnotherSslHandleStartsWithoutTheMark) {
    RawContext          context{RawContext::role::server};
    RawContext          ours{RawContext::role::client};
    RawPeer             peer{context, RawContext::role::server};
    qb::io::tcp::socket plain;
    ASSERT_EQ(plain.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
    ASSERT_EQ(listener.accept(peer.tcp), qb::io::SocketStatus::Done);
    qb::io::tcp::ssl::socket client(nullptr, plain);
    ASSERT_TRUE(mark_cut(client)) << "this tree cannot mark a stream cut";
    SSL *const handle = SSL_new(ours.ctx);
    ASSERT_NE(handle, nullptr);
    SSL_set_connect_state(handle);
    client.init(handle); // owned by the socket from here
    ASSERT_EQ(client.set_nonblocking(true), 0);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    const auto deadline = steady::now() + 5s;
    for (;;) {
        const int ours_step = client.handshake_status();
        const int theirs    = peer.handshake_step();
        ASSERT_GE(ours_step, 0);
        ASSERT_GE(theirs, 0);
        if (ours_step == 1 && theirs == 1)
            break;
        ASSERT_LT(steady::now(), deadline) << "the handshake did not complete";
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("another SSL handle on a socket that was marked cut, disconnect()", end);
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// An SSL object in OpenSSL's own quiet-shutdown mode -- the application's choice, on its handle or its context --
// records both ends and writes nothing: close_notify() says so.
TEST_F(TlsCloseNotify, AnSslObjectInOpenSslsQuietModeSendsNothingAndSaysSo) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    SSL_set_quiet_shutdown(client.ssl_handle(), 1);
    EXPECT_FALSE(send_close_notify(client)) << "an alert OpenSSL did not write was reported as sent";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("OpenSSL's quiet-shutdown mode, close_notify(), disconnect()", end);
    EXPECT_FALSE(end.clean()) << end.text();
    client.close();
}

// ─────────────────────────────── when none is sent ───────────────────────────────

// Output this side drops is a stream it cut. A standalone object that disconnects with output queued: the peer reads
// what TLS was given -- nothing here -- and an end with no alert.
TEST_F(TlsCloseNotify, AClientThatDropsQueuedOutputSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    ASSERT_EQ(client.disconnected_events, 0u) << "the case proves nothing: the client ended before it queued anything";
    client.publish(std::string(CUT, 'c')); // queued in the object, never handed to TLS:
    client.disconnect();                   // the dispatch this feeds disposes before any write
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_EQ(client.reason, 1) << "the case proves nothing: the client's own disconnect() did not end the connection";
    const TlsEnd end = peer.read_to_end();
    note("client disconnect() with output queued", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
    EXPECT_TRUE(end.unexpected_eof()) << end.text();
}

// What was queued is read BEFORE on(event::disconnected&&) runs: a hook that clears the queue -- what a reconnecting
// client does with the bytes of a dead connection -- drops that output all the same.
TEST_F(TlsCloseNotify, AClientWhoseHookClearsItsQueueSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.disconnected_hook = [](TlsClient &self) {
        self.out().reset();
    };
    client.start();
    pump(20ms);
    ASSERT_EQ(client.disconnected_events, 0u) << "the case proves nothing: the client ended before it queued anything";
    client.publish(std::string(CUT, 'c'));
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_EQ(client.queued(), 0u) << "the case proves nothing: the hook left the queue as it was";
    const TlsEnd end = peer.read_to_end();
    note("client disconnect(), its hook clears the queue", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// The stream is judged before the hook runs: a hook that closes the transport itself -- ssl::socket::disconnect()
// ends a TLS stream with the alert -- finds a stream already ended without one.
TEST_F(TlsCloseNotify, AClientWhoseHookClosesItsTransportWithOutputQueuedSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.disconnected_hook = [](TlsClient &self) {
        static_cast<void>(self.transport().disconnect());
    };
    client.start();
    pump(20ms);
    ASSERT_EQ(client.disconnected_events, 0u) << "the case proves nothing: the client ended before it queued anything";
    client.publish(std::string(CUT, 'c'));
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_NE(client.queued(), 0u) << "the case proves nothing: nothing was left queued";
    const TlsEnd end = peer.read_to_end();
    note("client disconnect() with output queued, its hook closes the transport", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// ...and output a hook queues is dropped too: it is read again after the hook.
TEST_F(TlsCloseNotify, AClientWhoseHookQueuesOutputSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.disconnected_hook = [](TlsClient &self) {
        self.out() << LAST;
    };
    client.start();
    pump(20ms);
    ASSERT_EQ(client.disconnected_events, 0u);
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_EQ(client.queued(), LAST.size()) << "the case proves nothing: the hook queued nothing";
    const TlsEnd end = peer.read_to_end();
    note("client disconnect(), its hook queues output", end);
    EXPECT_TRUE(end.received.empty());
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// The same for a session: disconnect() with output queued grants one write, drops the rest, and the session closes
// with its socket -- whose destructor must not say the stream ended.
TEST_F(TlsCloseNotify, ASessionThatDropsQueuedOutputSendsNoCloseNotify) {
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("cut", peer);
    note("session disconnect() with output queued", end);
    EXPECT_GT(end.received.size(), 0u) << "the case proves nothing: the one write a disconnect() grants wrote nothing";
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a reply cut short was ended as a whole one: " << end.text();
    EXPECT_EQ(state.destroyed, 1);
}

// A session whose hook clears its queue has nothing left to write: it is handed to the loop as one that wrote
// everything -- and must not be ended as one.
TEST_F(TlsCloseNotify, ASessionWhoseHookClearsItsQueueSendsNoCloseNotify) {
    state.clear_in_hook = true;
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("cut", peer);
    note("session disconnect(), its hook clears the queue", end);
    EXPECT_GT(end.received.size(), 0u);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a reply cut short was ended as a whole one: " << end.text();
    EXPECT_EQ(state.destroyed, 1);
}

// A session built on `output<>` alone that drops what it had queued: its stream closes with it, and says nothing.
TEST_F(TlsCloseNotify, AnOutputSessionThatDropsQueuedOutputSendsNoCloseNotify) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    Registry   registry;
    auto       session = std::make_unique<TlsOutputSession>(registry);
    ASSERT_NO_FATAL_FAILURE(accept_from(peer, session->transport()));
    session->start();
    pump(20ms);
    ASSERT_EQ(session->disconnected_events, 0u) << "the case proves nothing: the session ended before it queued anything";
    session->publish(std::string(CUT, 'c'));
    session->disconnect();
    ASSERT_TRUE(pump_while_not([&] { return registry.released == 1u; })) << "the session was never handed back";
    EXPECT_EQ(session->pendingWrite(), CUT) << "the case proves nothing: nothing was left queued";
    session.reset(); // its owner lets it go: the stream closes the socket
    const TlsEnd end = peer.read_to_end();
    note("output<> session disconnect() with output queued", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, AnOutputSessionWhoseHookClearsItsQueueSendsNoCloseNotify) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    Registry   registry;
    auto       session = std::make_unique<TlsOutputSession>(registry);
    ASSERT_NO_FATAL_FAILURE(accept_from(peer, session->transport()));
    session->clear_in_hook = true;
    session->start();
    pump(20ms);
    ASSERT_EQ(session->disconnected_events, 0u) << "the case proves nothing: the session ended before it queued anything";
    session->publish(std::string(CUT, 'c'));
    session->disconnect();
    ASSERT_TRUE(pump_while_not([&] { return registry.released == 1u; })) << "the session was never handed back";
    EXPECT_EQ(session->pendingWrite(), 0u) << "the case proves nothing: the hook left the queue as it was";
    session.reset();
    const TlsEnd end = peer.read_to_end();
    note("output<> session disconnect(), its hook clears the queue", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, AnOutputSessionWhoseHookQueuesOutputSendsNoCloseNotify) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    Registry   registry;
    auto       session = std::make_unique<TlsOutputSession>(registry);
    ASSERT_NO_FATAL_FAILURE(accept_from(peer, session->transport()));
    session->queue_in_hook = true;
    session->start();
    pump(20ms);
    ASSERT_EQ(session->disconnected_events, 0u);
    session->disconnect();
    ASSERT_TRUE(pump_while_not([&] { return registry.released == 1u; })) << "the session was never handed back";
    EXPECT_EQ(session->pendingWrite(), LAST.size()) << "the case proves nothing: the hook queued nothing";
    session.reset();
    const TlsEnd end = peer.read_to_end();
    note("output<> session disconnect(), its hook queues output", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, AnOutputSessionWhoseHookClosesItsTransportWithOutputQueuedSendsNoCloseNotify) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    Registry   registry;
    auto       session = std::make_unique<TlsOutputSession>(registry);
    ASSERT_NO_FATAL_FAILURE(accept_from(peer, session->transport()));
    session->close_in_hook = true;
    session->start();
    pump(20ms);
    ASSERT_EQ(session->disconnected_events, 0u) << "the case proves nothing: the session ended before it queued anything";
    session->publish(std::string(CUT, 'c'));
    session->disconnect();
    ASSERT_TRUE(pump_while_not([&] { return registry.released == 1u; })) << "the session was never handed back";
    EXPECT_EQ(session->pendingWrite(), CUT) << "the case proves nothing: nothing was left queued";
    session.reset();
    const TlsEnd end = peer.read_to_end();
    note("output<> session disconnect() with output queued, its hook closes the transport", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// A session whose hook queues output nobody will write: it is dropped with the session, which says nothing.
TEST_F(TlsCloseNotify, ASessionWhoseHookQueuesOutputSendsNoCloseNotify) {
    state.queue_in_hook = true;
    RawContext   context{RawContext::role::client};
    RawPeer      peer{context, RawContext::role::client};
    const TlsEnd end = session_ends("bye", peer);
    note("session disconnect(), its hook queues output", end);
    EXPECT_EQ(end.received, LAST) << "the case proves nothing: the reply before the close was not delivered";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
    EXPECT_EQ(state.destroyed, 1);
}

// ...and so does it when something still holds the session after its server released it and closes the transport
// itself: the stream is judged again after the hook, at the dispose, not left to the session's destructor.
TEST_F(TlsCloseNotify, ASessionHeldPastItsReleaseWhoseHookQueuedOutputSendsNoCloseNotify) {
    state.queue_in_hook = true;
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    ASSERT_EQ(server->sessions().size(), 1u);
    auto held = server->sessions().begin()->second; // the session outlives its server's release
    ASSERT_TRUE(peer.write("bye\n"));
    ASSERT_TRUE(pump_while_not([&] { return server->sessions().empty(); })) << "the server never released the session";
    EXPECT_EQ(state.disconnected, 1);
    EXPECT_EQ(state.destroyed, 0) << "the case proves nothing: the session did not outlive its release";
    EXPECT_EQ(held->out().size(), LAST.size()) << "the case proves nothing: the hook queued nothing";
    static_cast<void>(held->transport().disconnect()); // its holder closes it
    const TlsEnd end = peer.read_to_end();
    note("session disconnect(), its hook queues output, held past its release, then disconnect()", end);
    EXPECT_EQ(end.received, LAST) << "the case proves nothing: the reply before the close was not delivered";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
    held.reset();
    EXPECT_EQ(state.destroyed, 1);
}

// A fatal TLS error ends the session for OpenSSL, which sends the alert the error calls for: no close_notify follows.
TEST_F(TlsCloseNotify, AConnectionThatEndedOnAFatalTlsErrorSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    client.start();
    pump(20ms);
    const std::string garbage(64, '\x7f'); // no TLS record starts so
    ASSERT_EQ(qb::io::socket::send(peer.tcp.native_handle(), garbage.data(), static_cast<int>(garbage.size())),
              static_cast<int>(garbage.size()));
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; })) << "the client did not end on a record it cannot read";
    const TlsEnd end = peer.read_to_end();
    note("fatal TLS error", end);
    EXPECT_FALSE(end.clean()) << "a close_notify followed a fatal error: " << end.text();
    EXPECT_EQ(end.octets_after, 0u) << "something followed the fatal alert on the wire: " << end.text();
}

// Before the handshake completed there is no TLS stream to end: the peer reads handshake records, then the FIN.
TEST_F(TlsCloseNotify, ASocketThatDisconnectsBeforeItsHandshakeSendsNoAlert) {
    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    const int ret = client.n_connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
    ASSERT_TRUE(ret == 0 || qb::io::socket_no_error(qb::io::socket::get_last_errno()));
    qb::io::tcp::socket peer; // speaks no TLS: it reads what arrives
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    ASSERT_EQ(client.handshake_status(), 0) << "the ClientHello leaves, and the peer answers nothing";
    static_cast<void>(client.disconnect());
    client.close();
    const Read read = read_all(peer, 2000ms, false);
    EXPECT_EQ(read.end, "EOF");
    ASSERT_GE(read.received.size(), 5u) << "no ClientHello reached the peer";
    // Every TLS record the peer got is a handshake record (22): no alert (21).
    for (std::size_t at = 0; at + 5 <= read.received.size();) {
        const auto type   = static_cast<unsigned char>(read.received[at]);
        const auto length = (static_cast<std::size_t>(static_cast<unsigned char>(read.received[at + 3])) << 8)
                            | static_cast<std::size_t>(static_cast<unsigned char>(read.received[at + 4]));
        EXPECT_EQ(type, 22u) << "a record of type " << static_cast<unsigned>(type) << " at octet " << at;
        at += 5 + length;
    }
}

// Before the handshake completes there is no session to end, quietly or otherwise: a quiet_shutdown() made then --
// what the io layer does for a connection its peer ended, whenever that is -- records nothing, and the connection
// that goes on to shake hands is ended as any other.
TEST_F(TlsCloseNotify, AQuietShutdownBeforeTheHandshakeCompletesChangesNothing) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    const int ret = client.n_connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
    ASSERT_TRUE(ret == 0 || qb::io::socket_no_error(qb::io::socket::get_last_errno()));
    ASSERT_EQ(listener.accept(peer.tcp), qb::io::SocketStatus::Done);
    ASSERT_NO_FATAL_FAILURE(peer.attach());
    ASSERT_EQ(client.handshake_status(), 0) << "the ClientHello leaves: the handshake is under way";
    end_quietly(client);
    EXPECT_EQ(SSL_get_shutdown(client.ssl_handle()), 0) << "a session that does not exist yet was recorded as ended";
    const auto deadline = steady::now() + 5s;
    for (;;) {
        const int ours   = client.handshake_status();
        const int theirs = peer.handshake_step();
        ASSERT_GE(ours, 0);
        ASSERT_GE(theirs, 0);
        if (ours == 1 && theirs == 1)
            break;
        ASSERT_LT(steady::now(), deadline) << "the handshake did not complete";
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("quiet_shutdown() mid-handshake, then disconnect()", end);
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// quiet_shutdown(): the socket's owner says the stream is not to be ended -- disconnect() then sends nothing.
TEST_F(TlsCloseNotify, ASocketEndedQuietlySendsNoCloseNotify) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    end_quietly(client);
    EXPECT_NE(SSL_get_shutdown(client.ssl_handle()) & SSL_SENT_SHUTDOWN, 0) << "OpenSSL was not told the session ended";
    EXPECT_FALSE(send_close_notify(client)) << "a close_notify after quiet_shutdown()";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("quiet_shutdown(), disconnect()", end);
    EXPECT_FALSE(end.clean()) << end.text();
    EXPECT_TRUE(end.unexpected_eof()) << end.text();
    client.close();
}

// The socket alone: its peer's close_notify was read, and close_notify() answers it -- true: the alert was written.
TEST_F(TlsCloseNotify, ASocketWhosePeerEndedTheSessionAnswersWithItsOwn) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_EQ(peer.close_notify(), 0);
    ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
    char       octet    = 0;
    int        got      = 0;
    const auto deadline = steady::now() + 5s;
    while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(got, -1) << "the client never read its peer's close_notify";
    EXPECT_TRUE(send_close_notify(client)) << "the answer to the peer's close_notify was not reported as written";
    EXPECT_FALSE(send_close_notify(client)) << "the answer was sent twice";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("ssl::socket: peer-ended, close_notify(), disconnect()", end);
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// ...and disconnect() alone answers it too.
TEST_F(TlsCloseNotify, ASocketWhosePeerEndedTheSessionAnswersFromDisconnect) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_EQ(peer.close_notify(), 0);
    ASSERT_EQ(::shutdown(peer.tcp.native_handle(), QB_SD_SEND), 0);
    char       octet    = 0;
    int        got      = 0;
    const auto deadline = steady::now() + 5s;
    while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(got, -1) << "the client never read its peer's close_notify";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("ssl::socket: peer-ended, disconnect()", end);
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// An SSL object that writes a descriptor this socket does not own -- a number it closed, or gave away, may name
// another connection by now -- is not asked for the alert.
TEST_F(TlsCloseNotify, AnSslObjectThatWritesAnotherDescriptorIsNotGivenTheAlert) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    Pair                bystander; // an unrelated plain connection
    qb::io::tcp::socket unrelated;
    ASSERT_NO_FATAL_FAILURE(listen_on(bystander));
    ASSERT_EQ(unrelated.connect_v4("127.0.0.1", bystander.port), qb::io::SocketStatus::Done);
    ASSERT_EQ(bystander.listener.accept(bystander.peer), qb::io::SocketStatus::Done);
    ASSERT_EQ(SSL_set_fd(client.ssl_handle(), static_cast<int>(unrelated.native_handle())), 1);
    EXPECT_FALSE(send_close_notify(client)) << "the alert was written to a descriptor the socket does not own";
    static_cast<void>(client.disconnect());
    EXPECT_EQ(read_all(bystander.peer, 200ms, false).received.size(), 0u) << "a TLS record was written into an unrelated connection";
    SSL_set_bio(client.ssl_handle(), nullptr, nullptr);
    client.close();
}

// ─────────────────────────────── one alert, never in the way ───────────────────────────────

TEST_F(TlsCloseNotify, CloseNotifyIsSentOnce) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ERR_clear_error();
    EXPECT_TRUE(send_close_notify(client)) << "the alert was not written on an idle connection";
    EXPECT_FALSE(send_close_notify(client)) << "a second close_notify";
    static_cast<void>(client.disconnect()); // ...and none from the close either
    EXPECT_EQ(ERR_peek_error(), 0u) << "the close left an entry in the thread's OpenSSL error queue";
    const TlsEnd end = peer.read_to_end();
    note("close_notify() twice, disconnect()", end);
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// The alert closes this side's sending direction alone: what the peer still sends is read as before, and a second
// call neither sends nor reads -- OpenSSL's second SSL_shutdown() would read for the peer's alert and discard the
// application data it met.
TEST_F(TlsCloseNotify, WhatThePeerStillSendsCanBeReadAfterCloseNotify) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    EXPECT_TRUE(send_close_notify(client));
    ASSERT_TRUE(peer.write("y"));
    pump(20ms);
    EXPECT_FALSE(send_close_notify(client));
    char       octet    = 0;
    int        got      = 0;
    const auto deadline = steady::now() + 5s;
    while ((got = client.read(&octet, 1)) == 0 && steady::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_EQ(got, 1) << "the peer's octet was lost to a second close_notify()";
    EXPECT_EQ(octet, 'y');
    static_cast<void>(client.disconnect());
    client.close();
}

// A write the socket refuses outright -- its peer reset the connection, and this side has not read that -- fails
// inside OpenSSL, which records it: the close leaves the thread's error queue as it found it.
TEST_F(TlsCloseNotify, AnAlertTheSocketRefusesLeavesNoErrorBehind) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_NO_FATAL_FAILURE(reset_connection(peer.tcp));
    // The reset has arrived once the socket refuses a write (the octet is noise: nobody reads this connection again).
    bool       refused  = false;
    const auto deadline = steady::now() + 5s;
    while (!refused && steady::now() < deadline) {
        refused = qb::io::socket::send(client.native_handle(), "a", 1) < 0 && !qb::io::socket::not_send_error(qb::io::socket::get_last_errno());
        if (!refused)
            std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(refused) << "the case proves nothing: the socket still takes a write after its peer reset the connection";
    ERR_clear_error();
    EXPECT_FALSE(send_close_notify(client)) << "an alert was written into a connection its peer reset";
    EXPECT_EQ(ERR_peek_error(), 0u) << "the refused alert left an entry in the thread's OpenSSL error queue";
    static_cast<void>(client.disconnect());
    client.close();
}

// close_notify() alone leaves the socket's blocking mode to its caller, who may go on reading what the peer still
// sends; disconnect() is the call that makes the socket non-blocking, because it closes the connection.
TEST_F(TlsCloseNotify, CloseNotifyLeavesABlockingSocketBlocking) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_EQ(client.set_nonblocking(false), 0);
    ASSERT_EQ(client.test_nonblocking(), 0);
    EXPECT_TRUE(send_close_notify(client));
    EXPECT_EQ(client.test_nonblocking(), 0) << "close_notify() changed the socket's blocking mode";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    EXPECT_CLEAN_END(end, "");
    client.close();
}

// A socket filled to the octet: the close goes on at once, with the alert only if the socket took it, and leaves no
// error behind. Where a filled socket stays full (kAFilledSocketStaysFull: Linux, Windows) it takes nothing and the
// close goes on without the alert; on macOS the kernel may have made room by then, and the answer must say which it was.
TEST_F(TlsCloseNotify, AFilledSocketIsClosedAtOnceWithTheAlertOnlyIfItTookIt) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_GT(fill(client.native_handle()), 0u);
    const auto written = BIO_number_written(SSL_get_wbio(client.ssl_handle()));
    ERR_clear_error();
    const bool sent = send_close_notify(client);
    const auto took = BIO_number_written(SSL_get_wbio(client.ssl_handle())) - written;
    EXPECT_EQ(sent, took != 0) << "close_notify() answered " << sent << " where the socket took " << took << " octets";
    if (kAFilledSocketStaysFull) {
        EXPECT_FALSE(sent) << "an alert was written into a socket that takes nothing";
    }
    EXPECT_EQ(ERR_peek_error(), 0u) << "the alert left an entry in the thread's OpenSSL error queue";
    const auto before = steady::now();
    EXPECT_EQ(client.disconnect(), 0);
    EXPECT_LT(ms_since(before), 1000.0) << "the close waited for a socket that takes nothing";
    client.close();
}

// ...and, on every system, a socket that takes nothing at the moment of the alert: what a socket BIO answers for a full
// send buffer -- "try again later" -- answered to every write. The close goes on without the alert, at once, with no
// error left behind, and the peer reads the end of the connection with no close_notify.
TEST_F(TlsCloseNotify, ASocketThatCannotTakeTheAlertIsClosedWithoutIt) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    ASSERT_NO_FATAL_FAILURE(inject_once::install(client.ssl_handle(), true));
    ASSERT_NO_FATAL_FAILURE(count_write_attempts(client.ssl_handle()));
    inject_once::refuse_writes = true;
    ERR_clear_error();
    EXPECT_FALSE(send_close_notify(client)) << "an alert was reported written into a socket that takes nothing";
    EXPECT_EQ(g_write_attempts, 1u) << "the alert was not attempted once: " << g_write_attempts;
    EXPECT_EQ(ERR_peek_error(), 0u) << "the alert that was not written left an entry in the thread's OpenSSL error queue";
    const auto before = steady::now();
    EXPECT_EQ(client.disconnect(), 0);
    EXPECT_LT(ms_since(before), 1000.0) << "the close waited for a socket that takes nothing";
    EXPECT_EQ(g_write_attempts, 1u) << "the close attempted the alert again";
    const TlsEnd end = peer.read_to_end();
    note("a socket that takes nothing, close_notify(), disconnect()", end);
    EXPECT_TRUE(end.unexpected_eof()) << "the peer read an alert the socket never took: " << end.text();
    client.close();
}

// A record OpenSSL could not write whole is still its to finish: the alert may not overtake it, and since nothing
// will finish it the stream is cut -- the peer reads no clean end.
TEST_F(TlsCloseNotify, ASocketWithARecordItCouldNotWriteSendsNoAlert) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    const std::string chunk(std::size_t{64} << 10, 'p');
    std::size_t       written = 0;
    for (int ret = 0; (ret = client.write(chunk.data(), chunk.size())) > 0;)
        written += static_cast<std::size_t>(ret);
    ASSERT_GT(written, 0u);
    ERR_clear_error();
    EXPECT_FALSE(send_close_notify(client)) << "an alert overtook a record that was not written whole";
    EXPECT_EQ(ERR_peek_error(), 0u) << "the alert that was not written left an entry in the thread's OpenSSL error queue";
    static_cast<void>(client.disconnect());
    const TlsEnd end = peer.read_to_end();
    note("a record pending, disconnect()", end);
    EXPECT_GE(end.received.size(), written) << "the peer did not read what TLS had taken whole";
    EXPECT_FALSE(end.clean()) << "a stream cut inside a record was ended as a whole one: " << end.text();
    client.close();
}

// The same on a BLOCKING socket, where a write waits for room: the close must not. A helper drains the peer after
// two seconds so that a close that did block is reported, not hung. Where a filled socket does not stay full (macOS:
// kAFilledSocketStaysFull) the alert may find room and the bound alone proves nothing: what keeps the close from
// waiting -- the socket made non-blocking first -- is asserted on every system.
TEST_F(TlsCloseNotify, ABlockingSocketThatCannotTakeTheAlertIsNotWaitedFor) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_GT(fill(client.native_handle()), 0u);
    ASSERT_EQ(client.set_nonblocking(false), 0);

    std::atomic<bool> closed{false};
    std::thread       rescue([&] {
        const auto deadline = steady::now() + 2s;
        while (!closed.load() && steady::now() < deadline)
            std::this_thread::sleep_for(5ms);
        char buffer[16384];
        while (!closed.load()) { // the close is stuck in a write: make room
            if (qb::io::socket::recv(peer.tcp.native_handle(), buffer, static_cast<int>(sizeof(buffer))) <= 0)
                std::this_thread::sleep_for(1ms);
        }
    });
    const auto        before = steady::now();
    static_cast<void>(client.disconnect());
    const double took = ms_since(before);
    closed.store(true);
    rescue.join();
    EXPECT_LT(took, 1000.0) << "disconnect() waited " << took << " ms for a blocking socket to take the alert";
    // What keeps it from waiting, whether or not the kernel made room meanwhile (kAFilledSocketStaysFull).
    EXPECT_EQ(client.test_nonblocking(), 1) << "disconnect() attempted the alert on a blocking socket";
    client.close();
}

// An io object that was never started still has the blocking socket its connect left it: its close hands the
// descriptor to the loop all the same, and must not wait for the alert either (on macOS the alert may find room: the
// bound is asserted there too, the full socket only where it stays full -- the case above holds the mechanism).
TEST_F(TlsCloseNotify, AnObjectNeverStartedIsNotWaitedForEither) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    ASSERT_GT(fill(client.transport().native_handle()), 0u);
    ASSERT_EQ(client.transport().set_nonblocking(false), 0);

    std::atomic<bool> closed{false};
    std::thread       rescue([&] {
        const auto deadline = steady::now() + 2s;
        while (!closed.load() && steady::now() < deadline)
            std::this_thread::sleep_for(5ms);
        char buffer[16384];
        while (!closed.load()) { // the close is stuck in a write: make room
            if (qb::io::socket::recv(peer.tcp.native_handle(), buffer, static_cast<int>(sizeof(buffer))) <= 0)
                std::this_thread::sleep_for(1ms);
        }
    });
    const auto        before = steady::now();
    client.drop_now();
    const double took = ms_since(before);
    closed.store(true);
    rescue.join();
    EXPECT_LT(took, 1000.0) << "the close waited " << took << " ms for a blocking socket to take the alert";
    EXPECT_EQ(client.disconnected_events, 1u);
    EXPECT_FALSE(client.transport().is_open());
}

// ─────────────────────────────── a stream closed or destroyed with output queued ───────────────────────────────
// Nothing disposed these: no dispose() judged the stream. The stream itself knows what it drops.

TEST_F(TlsCloseNotify, AClientDestroyedWithOutputQueuedSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    {
        TlsClient client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        client.start();
        pump(20ms);
        ASSERT_EQ(client.disconnected_events, 0u);
        client.publish(std::string(LAST));
        ASSERT_EQ(client.queued(), LAST.size()) << "the case proves nothing: nothing is queued";
    } // destroyed while connected: no loop pass wrote what it queued
    const TlsEnd end = peer.read_to_end();
    note("client destroyed with output queued", end);
    EXPECT_TRUE(end.received.empty()) << "the case proves nothing: what was queued was written";
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, AServerDestroyedWithASessionThatHasOutputQueuedSendsNoCloseNotify) {
    RawContext context{RawContext::role::client};
    RawPeer    peer{context, RawContext::role::client};
    ASSERT_NO_FATAL_FAILURE(connect_to_server(peer));
    ASSERT_TRUE(peer.write("hold\n"));
    ASSERT_TRUE(pump_while_not([&] { return state.held != 0; })) << "the session never queued its reply";
    server.reset(); // its sessions go with it, undisposed, most of the reply still queued
    const TlsEnd end = peer.read_to_end();
    note("server destroyed, a session with output queued", end);
    EXPECT_LT(end.received.size(), CUT) << "the case proves nothing: everything queued was written";
    EXPECT_FALSE(end.clean()) << "a reply cut short was ended as a whole one: " << end.text();
    EXPECT_EQ(state.destroyed, 1);
}

TEST_F(TlsCloseNotify, ATlsStreamClosedWithOutputQueuedSendsNoCloseNotify) {
    RawContext              context{RawContext::role::server};
    RawPeer                 peer{context, RawContext::role::server};
    qb::io::transport::stcp stream;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, stream.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, stream.transport()));
    ASSERT_NE(stream.publish(LAST.data(), LAST.size()), nullptr);
    stream.close();
    const TlsEnd end = peer.read_to_end();
    note("stcp close() with output queued", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// ...and with nothing queued it is a whole stream's end, by close() as by the destructor.
TEST_F(TlsCloseNotify, ATlsStreamClosedWithNothingQueuedSendsCloseNotify) {
    RawContext              context{RawContext::role::server};
    RawPeer                 peer{context, RawContext::role::server};
    qb::io::transport::stcp stream;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, stream.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, stream.transport()));
    ASSERT_NE(stream.publish(LAST.data(), LAST.size()), nullptr);
    ASSERT_EQ(stream.write(), static_cast<int>(LAST.size()));
    stream.close();
    const TlsEnd end = peer.read_to_end();
    note("stcp close(), everything written", end);
    EXPECT_CLEAN_END(end, LAST);
}

TEST_F(TlsCloseNotify, ATlsStreamDestroyedWithOutputQueuedSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    {
        qb::io::transport::stcp stream;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, stream.transport()));
        ASSERT_NO_FATAL_FAILURE(settle(peer, stream.transport()));
        ASSERT_NE(stream.publish(LAST.data(), LAST.size()), nullptr);
    }
    const TlsEnd end = peer.read_to_end();
    note("stcp destroyed with output queued", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, AnOutputStreamClosedWithOutputQueuedSendsNoCloseNotify) {
    RawContext                                context{RawContext::role::server};
    RawPeer                                   peer{context, RawContext::role::server};
    qb::io::ostream<qb::io::tcp::ssl::socket> stream;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, stream.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, stream.transport()));
    ASSERT_NE(stream.publish(LAST.data(), LAST.size()), nullptr);
    stream.close();
    const TlsEnd end = peer.read_to_end();
    note("ostream<ssl::socket> close() with output queued", end);
    EXPECT_FALSE(end.clean()) << "a stream cut short was ended as a whole one: " << end.text();
}

// ...and with everything written it is a whole stream's end.
TEST_F(TlsCloseNotify, AnOutputStreamClosedWithNothingQueuedSendsCloseNotify) {
    RawContext                                context{RawContext::role::server};
    RawPeer                                   peer{context, RawContext::role::server};
    qb::io::ostream<qb::io::tcp::ssl::socket> stream;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, stream.transport()));
    ASSERT_NO_FATAL_FAILURE(settle(peer, stream.transport()));
    ASSERT_NE(stream.publish(LAST.data(), LAST.size()), nullptr);
    ASSERT_EQ(stream.write(), static_cast<int>(LAST.size()));
    stream.close();
    const TlsEnd end = peer.read_to_end();
    note("ostream<ssl::socket> close(), everything written", end);
    EXPECT_CLEAN_END(end, LAST);
}

// ─────────────────────────────── sessions stay resumable ───────────────────────────────

// The alert sent, the peer's not awaited, the SSL object freed: the session resumes. Before, freeing the SSL object
// of a connection whose shutdown state was never set made OpenSSL discard its session.
TEST_F(TlsCloseNotify, ASessionResumesAfterAConnectionThisSideClosed) {
    RawContext           context{RawContext::role::server};
    qb::io::ssl::Session session{};
    {
        RawPeer                  peer{context, RawContext::role::server};
        qb::io::tcp::ssl::socket client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        ASSERT_NO_FATAL_FAILURE(first_connection(peer, client, session));
        static_cast<void>(client.disconnect());
        client.close();
    } // the SSL object is freed here
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.resume(session);
    qb::io::ssl::free_session(session);
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
    EXPECT_EQ(SSL_session_reused(peer.ssl), 1) << "the server made a full handshake";
    static_cast<void>(client.disconnect());
    client.close();
}

TEST_F(TlsCloseNotify, ASessionResumesOverTls12AfterAConnectionThisSideClosed) {
    RawContext           context{RawContext::role::server, TLS1_2_VERSION};
    qb::io::ssl::Session session{};
    {
        RawPeer                  peer{context, RawContext::role::server};
        qb::io::tcp::ssl::socket client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        ASSERT_NO_FATAL_FAILURE(first_connection(peer, client, session));
        static_cast<void>(client.disconnect());
        client.close();
    }
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.resume(session);
    qb::io::ssl::free_session(session);
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
    EXPECT_EQ(SSL_session_reused(peer.ssl), 1) << "the server made a full handshake";
    static_cast<void>(client.disconnect());
    client.close();
}

// The same through the io object, both ways a connection ends: the client's own disconnect(), and the server's
// close_notify -- after which this side says nothing, and its session must stay usable all the same.
TEST_F(TlsCloseNotify, ASessionResumesAfterAnIoObjectEndedItsConnection) {
    RawContext           context{RawContext::role::server};
    qb::io::ssl::Session session{};
    {
        RawPeer   peer{context, RawContext::role::server};
        TlsClient client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        ASSERT_NO_FATAL_FAILURE(first_connection(peer, client.transport(), session));
        client.start();
        client.disconnect();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    }
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.resume(session);
    qb::io::ssl::free_session(session);
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
    static_cast<void>(client.disconnect());
    client.close();
}

TEST_F(TlsCloseNotify, ASessionResumesAfterThePeerEndedTheConnection) {
    RawContext           context{RawContext::role::server};
    qb::io::ssl::Session session{};
    {
        RawPeer   peer{context, RawContext::role::server};
        TlsClient client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        ASSERT_NO_FATAL_FAILURE(first_connection(peer, client.transport(), session));
        client.start();
        ASSERT_EQ(peer.close_notify(), 0);
        peer.tcp.close();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
        EXPECT_EQ(client.reason, 0);
    }
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.resume(session);
    qb::io::ssl::free_session(session);
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
    static_cast<void>(client.disconnect());
    client.close();
}

TEST_F(TlsCloseNotify, ASessionResumesOverTls12AfterThePeerEndedTheConnection) {
    RawContext           context{RawContext::role::server, TLS1_2_VERSION};
    qb::io::ssl::Session session{};
    {
        RawPeer   peer{context, RawContext::role::server};
        TlsClient client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        ASSERT_NO_FATAL_FAILURE(first_connection(peer, client.transport(), session));
        client.start();
        ASSERT_EQ(peer.close_notify(), 0);
        peer.tcp.close();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
        EXPECT_EQ(client.reason, 0);
    }
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.resume(session);
    qb::io::ssl::free_session(session);
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
    static_cast<void>(client.disconnect());
    client.close();
}

// A reset is a socket error, not a TLS one: a session saved from that connection resumes, as after any other close.
// OpenSSL 3 keeps the session, and this side's end is recorded when the reset is read (no alert follows it); OpenSSL 4
// ends the session there itself, and does not discard the session of an SSL object it left in its error state.
TEST_F(TlsCloseNotify, ASessionResumesAfterThePeerResetTheConnection) {
    for (const int version : {0, TLS1_2_VERSION}) {
        RawContext           context{RawContext::role::server, version};
        qb::io::ssl::Session session{};
        {
            RawPeer   peer{context, RawContext::role::server};
            TlsClient client;
            ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
            ASSERT_NO_FATAL_FAILURE(first_connection(peer, client.transport(), session));
            client.start();
            ASSERT_NO_FATAL_FAILURE(reset_connection(peer.tcp));
            ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
            EXPECT_NE(client.system_error, 0) << "the case proves nothing: the client read no reset";
        }
        RawPeer                  peer{context, RawContext::role::server};
        qb::io::tcp::ssl::socket client;
        client.resume(session);
        qb::io::ssl::free_session(session);
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
        static_cast<void>(client.disconnect());
        client.close();
    }
}

// A connection ended with output dropped sends no alert, and its session is still usable: OpenSSL is told the session
// is over on every close, the silent ones included.
TEST_F(TlsCloseNotify, ASessionResumesAfterAConnectionCutShort) {
    RawContext           context{RawContext::role::server};
    qb::io::ssl::Session session{};
    {
        RawPeer   peer{context, RawContext::role::server};
        TlsClient client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
        ASSERT_NO_FATAL_FAILURE(first_connection(peer, client.transport(), session));
        client.start();
        pump(20ms);
        client.publish(std::string(CUT, 'c'));
        client.disconnect();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
        const TlsEnd end = peer.read_to_end();
        ASSERT_FALSE(end.clean()) << "the case proves nothing: the connection was not cut short";
    }
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    client.resume(session);
    qb::io::ssl::free_session(session);
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    EXPECT_EQ(SSL_session_reused(client.ssl_handle()), 1) << "the client made a full handshake: its saved session was discarded";
    static_cast<void>(client.disconnect());
    client.close();
}

// A bare socket destroyed while connected -- no disconnect(): nothing ended the session -- still leaves a session
// that resumes: OpenSSL is told before the SSL object is freed. So does one whose handle is replaced.
TEST_F(TlsCloseNotify, ASessionResumesAfterASocketWasDestroyedWhileConnected) {
    expect_resumption_after([](qb::io::tcp::ssl::socket &) {});
}

TEST_F(TlsCloseNotify, ASessionResumesAfterASocketWasGivenAnotherHandle) {
    expect_resumption_after([](qb::io::tcp::ssl::socket &client) { client.init(); });
}

// ...and writes nothing: nothing said the stream was whole. The peer reads the end of the connection, no alert.
TEST_F(TlsCloseNotify, ASocketDestroyedWhileConnectedSendsNoCloseNotify) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    {
        qb::io::tcp::ssl::socket client;
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    }
    const TlsEnd end = peer.read_to_end();
    note("ssl::socket destroyed while connected", end);
    EXPECT_FALSE(end.clean()) << "a socket nobody disconnected ended its stream as a whole one: " << end.text();
}

TEST_F(TlsCloseNotify, ASocketGivenAnotherHandleWhileConnectedSendsNoCloseNotify) {
    RawContext               context{RawContext::role::server};
    RawPeer                  peer{context, RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
    ASSERT_NO_FATAL_FAILURE(settle(peer, client));
    client.init();
    client.close();
    const TlsEnd end = peer.read_to_end();
    note("ssl::socket re-init()ed while connected", end);
    EXPECT_FALSE(end.clean()) << "a socket nobody disconnected ended its stream as a whole one: " << end.text();
}

// A socket object that connects again keeps its SSL object. Whatever the last connection's close left in it -- an
// alert a full socket never took -- the next connection starts with its own handshake, not with that alert: OpenSSL
// clears the object when a handshake starts on it. A guard on that, which no code of qb's provides. The socket that
// takes nothing is the filter that answers every write "try again later" (kAFilledSocketStaysFull: a filled socket
// does not stay full on macOS).
TEST_F(TlsCloseNotify, AReconnectAfterAnAlertThatWasNotWrittenShakesHands) {
    RawContext               context{RawContext::role::server};
    qb::io::tcp::ssl::socket client;
    {
        RawPeer peer{context, RawContext::role::server};
        ASSERT_NO_FATAL_FAILURE(connect_to(peer, client));
        ASSERT_NO_FATAL_FAILURE(inject_once::install(client.ssl_handle(), true));
        inject_once::refuse_writes = true;
        ASSERT_FALSE(send_close_notify(client)) << "the case proves nothing: the socket took the alert";
        static_cast<void>(client.disconnect());
        client.close();
        inject_once::refuse_writes = false;
    }
    RawPeer peer{context, RawContext::role::server};
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client)) << "the reconnection did not shake hands";
    ASSERT_EQ(client.write(LAST.data(), LAST.size()), static_cast<int>(LAST.size()));
    const TlsEnd end = peer.read_to_end(2000ms, LAST.size());
    EXPECT_EQ(end.received, LAST);
    static_cast<void>(client.disconnect());
    client.close();
}

// ─────────────────────────────── the loop still closes after the peer ───────────────────────────────

// A peer that answers the alert with its own and closes -- what OpenSSL peers do -- ends the close at once: the loop
// drops the answer as bytes and closes on the EOF, not at linger().
TEST_F(TlsCloseNotify, APeerThatAnswersWithItsOwnCloseNotifyEndsTheCloseAtOnce) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    const auto handle = client.transport().native_handle();
    client.set_linger(20s);
    client.start();
    pump(20ms);
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    ASSERT_EQ(closing(), 1u) << "the peer is still open: the loop is closing the descriptor after it";
    const TlsEnd end = peer.read_to_end(5000ms);
    EXPECT_TRUE(end.clean()) << end.text();
    const auto answered = steady::now();
    static_cast<void>(peer.close_notify());
    peer.tcp.close();
    EXPECT_TRUE(pump_while_not([&] { return closing() == 0u; }, 2000ms))
        << "the peer's answer and EOF did not end the close (" << ms_since(answered) << " ms of a 20 s bound)";
    EXPECT_FALSE(descriptor_open(handle));
}

// A peer that answers and stays: its close_notify and whatever it sends afterwards are bytes to the loop, dropped;
// only its EOF, an error or the bound end the close.
TEST_F(TlsCloseNotify, WhatThePeerSendsAfterTheAlertIsDroppedAndEndsNothing) {
    RawContext context{RawContext::role::server};
    RawPeer    peer{context, RawContext::role::server};
    TlsClient  client;
    ASSERT_NO_FATAL_FAILURE(connect_to(peer, client.transport()));
    const auto handle = client.transport().native_handle();
    client.set_linger(20s);
    client.start();
    pump(20ms);
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    const TlsEnd end = peer.read_to_end(5000ms);
    EXPECT_TRUE(end.clean()) << end.text();
    static_cast<void>(peer.close_notify());
    const std::string late(4096, 'n');
    EXPECT_EQ(qb::io::socket::send(peer.tcp.native_handle(), late.data(), static_cast<int>(late.size())), static_cast<int>(late.size()));
    pump(100ms);
    EXPECT_EQ(closing(), 1u) << "what the peer sent ended the close: only its EOF, an error or the bound may";
    EXPECT_TRUE(descriptor_open(handle));
    peer.tcp.close();
    EXPECT_TRUE(pump_while_not([&] { return closing() == 0u; }, 2000ms));
}

// ─────────────────────────────── qb reading qb ───────────────────────────────

// qb's own reader is what it was: a qb client whose qb server closes after its reply reads the reply, then the end
// of the connection as its peer's -- no error, one event -- and the lingering session ends when the client closes.
TEST_F(TlsCloseNotify, AQbClientReadsAQbServersCloseAsThePeersEnd) {
    ASSERT_NO_FATAL_FAILURE(start_server());
    TlsClient client;
    client.transport().set_insecure();
    const int ret = client.transport().n_connect(qb::io::endpoint{"127.0.0.1", server_port}, "localhost");
    ASSERT_TRUE(ret == 0 || qb::io::socket_no_error(qb::io::socket::get_last_errno()));
    ASSERT_TRUE(pump_while_not([&] { return client.transport().handshake_status() != 0; })) << "the handshake did not complete";
    ASSERT_EQ(client.transport().handshake_status(), 1);
    client.start();
    client.publish("line\n");
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_EQ(client.messages, 1u);
    EXPECT_EQ(client.last, LAST);
    EXPECT_EQ(client.reason, 0) << "the server's close must read as the peer's end of the connection";
    EXPECT_EQ(client.system_error, 0);
    EXPECT_FALSE(client.transport().is_open());
    EXPECT_EQ(closing(), 0u) << "a connection its peer ended is closed outright";
    EXPECT_TRUE(pump_while_not([&] { return state.destroyed == 1; }, 2000ms)) << "the lingering session outlived its peer's close";
}
