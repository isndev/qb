/**
 * @file qb/tests/io/system/tcp/close-after-peer-tls.cpp
 * @brief A TLS socket that gives its descriptor away detaches its `SSL` object from it (Huly QB-1270).
 *
 * An io object that ends its own connection gives its descriptor to its thread's loop, which closes it after the
 * peer -- reading the socket raw: nothing decrypts what is dropped. From then on the NUMBER is the loop's, then
 * nobody's, then whatever socket the system hands it to next. The `SSL` object of the ended connection stays in the
 * `ssl::socket` until its next `init()` or its destruction, and it is a healthy object: handshake finished, keys in
 * place. Left bound to the number -- what the inherited `tcp::socket::release_handle()` did -- a `write()` on that
 * transport "completes" the handshake it already had and writes a TLS record of the ended connection into the
 * unrelated connection that now owns the number (measured on Linux and Windows: 27 octets, an application-data
 * record). `ssl::socket::release_handle()` frees the socket BIO first: the `SSL` object names no descriptor, and a
 * `read()` or `write()` after it fails.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/close_after_peer_support.h"
#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_pump.h"

using namespace std::chrono_literals;

namespace close_after_peer_tls_test {

using namespace qb::io::test::close_after_peer;

/// A standalone TLS client.
class TlsClient : public qb::io::use<TlsClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::text::command<TlsClient>;

    std::size_t disconnected_events = 0;

    void
    on(Protocol::message &&) {}

    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
    }
};

/// An unrelated plain TCP connection made to take a descriptor number another connection gave up.
struct Bystander {
    qb::io::tcp::listener            listener;
    qb::io::tcp::socket              connected;
    qb::io::tcp::socket              accepted;
    std::vector<qb::io::tcp::socket> spare;
    bool                             got = false; ///< Whether `connected` has the wanted number.

    /// Before the number is released: the listener must not be the one that takes it.
    void
    listen() {
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    }

    /// Connections until one gets `wanted` (POSIX hands out the lowest free number; Windows reuses a handle often).
    void
    take(::socket_type wanted) {
        const auto port = listener.local_endpoint().port();
        for (int i = 0; i < 64 && !got; ++i) {
            qb::io::tcp::socket s;
            ASSERT_EQ(s.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
            qb::io::tcp::socket a;
            ASSERT_EQ(listener.accept(a), qb::io::SocketStatus::Done);
            if (s.native_handle() == wanted) {
                connected = std::move(s);
                accepted  = std::move(a);
                got       = true;
            } else {
                spare.push_back(std::move(s));
                spare.push_back(std::move(a));
            }
        }
    }

    /// What the bystander's peer received within 200 ms, no loop pass run.
    [[nodiscard]] std::string
    received() {
        return got ? read_all(accepted, 200ms, false).received : std::string{};
    }
};

class CloseAfterPeerTls : public CleanLoop {
protected:
    qb::io::tcp::ssl::listener listener;
    std::uint16_t              port = 0;
    qb::io::test::tls_pair     pair;

    /// A TLS connection over loopback, handshake complete, in `pair`.
    void
    connect_tls() {
        ASSERT_TRUE(qb::io::test::require_ssl_files());
        listener.init(qb::io::ssl::create_server_context(TLS_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                         qb::io::test::ssl_resource_path("key.pem")));
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
        port = listener.local_endpoint().port();
        pair.client.set_insecure();
        ASSERT_TRUE(qb::io::test::open_tls_connection(listener, port, pair));
        ASSERT_TRUE(qb::io::test::complete_tls_handshake(pair));
    }

    void
    TearDown() override {
        // The fixture's own sockets are closed before the census: its members outlive TearDown().
        pair.client.close();
        pair.server.close();
        listener.close();
        CleanLoop::TearDown();
    }
};

} // namespace close_after_peer_tls_test

using namespace close_after_peer_tls_test;

// The mechanism, at the socket: the hand-over sequence of detail::end_transport -- release_handle(), disconnect(),
// close() -- then what the loop does at the end. Once the number names an unrelated socket, one stray write() on the
// TLS socket must reach nothing.
TEST_F(CloseAfterPeerTls, ASocketThatGaveItsDescriptorAwayWritesNothingOnTheNumber) {
    ASSERT_NO_FATAL_FAILURE(connect_tls());
    ASSERT_TRUE(qb::io::test::tls_round_trip(pair, "ping", "pong"));
    Bystander bystander;
    ASSERT_NO_FATAL_FAILURE(bystander.listen());

    const auto released = pair.client.release_handle();
    ASSERT_NE(released, qb::io::invalid_socket);
    EXPECT_EQ(SSL_get_rfd(pair.client.ssl_handle()), -1) << "the SSL object still reads the descriptor the socket gave away";
    EXPECT_EQ(SSL_get_wfd(pair.client.ssl_handle()), -1) << "the SSL object still writes the descriptor the socket gave away";
    EXPECT_FALSE(pair.client.is_open());
    EXPECT_FALSE(pair.client.handshake_complete()) << "a socket that holds no descriptor still says its handshake is complete";
    static_cast<void>(pair.client.disconnect());
    pair.client.close();
    // ...and the loop's end of it: the half-close, the peer closes, the descriptor is closed.
    static_cast<void>(::shutdown(released, QB_SD_SEND));
    pair.server.close();
    static_cast<void>(::QB_CLOSESOCKET(released));

    ASSERT_NO_FATAL_FAILURE(bystander.take(released));
#if !defined(_WIN32)
    ASSERT_TRUE(bystander.got) << "the system did not hand the released number out again: the case proves nothing";
#endif
    EXPECT_LE(pair.client.write("STRAY", 5), 0) << "a write on a socket that owns no descriptor reached a descriptor";
    EXPECT_EQ(bystander.received().size(), 0u) << "a TLS record of the ended connection was written into an unrelated connection";
    // Freeing the detached SSL object (the next transport's move-assignment, the destructor) closes nothing either.
    pair.client = qb::io::tcp::ssl::socket{};
    EXPECT_TRUE(!bystander.got || descriptor_open(released)) << "SSL_free closed a descriptor it does not own";
}

// The same through the io object: a TLS client ends its own connection, its peer closes, the loop closes the number, an
// unrelated connection takes it, and the application writes once on the transport it still holds.
TEST_F(CloseAfterPeerTls, AWriteOnTheTransportAfterAnOwnEndedDisposeReachesNothing) {
    ASSERT_NO_FATAL_FAILURE(connect_tls());
    Bystander bystander;
    ASSERT_NO_FATAL_FAILURE(bystander.listen());

    TlsClient client;
    client.transport() = std::move(pair.client);
    const auto handle  = client.transport().native_handle();
    client.start();
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_FALSE(client.transport().is_open());
    EXPECT_EQ(closing(), 1u) << "the peer is still open: the loop is closing the descriptor after it";
    pair.server.close();
    ASSERT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "the peer closed: the loop must close the number";

    ASSERT_NO_FATAL_FAILURE(bystander.take(handle));
#if !defined(_WIN32)
    ASSERT_TRUE(bystander.got) << "the system did not hand the closed number out again: the case proves nothing";
#endif
    EXPECT_LE(client.transport().write("STRAY", 5), 0);
    EXPECT_EQ(bystander.received().size(), 0u) << "a TLS record of the ended connection was written into an unrelated connection";
}

// The control: the same stray write after a PEER-ended dispose, which closes the transport outright, reached nothing
// before and reaches nothing now.
TEST_F(CloseAfterPeerTls, AWriteOnTheTransportAfterAPeerEndedDisposeReachesNothing) {
    ASSERT_NO_FATAL_FAILURE(connect_tls());
    Bystander bystander;
    ASSERT_NO_FATAL_FAILURE(bystander.listen());

    TlsClient client;
    client.transport() = std::move(pair.client);
    const auto handle  = client.transport().native_handle();
    client.start();
    pair.server.close(); // the peer ends it
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    EXPECT_FALSE(descriptor_open(handle));
    EXPECT_EQ(closing(), 0u);

    ASSERT_NO_FATAL_FAILURE(bystander.take(handle));
    EXPECT_LE(client.transport().write("STRAY", 5), 0);
    EXPECT_EQ(bystander.received().size(), 0u) << "a TLS record of the ended connection was written into an unrelated connection";
}

// A TLS client's own-ended close goes through the loop like a plain one: the peer reads what was sent, the client's
// close_notify (tls/tls-close-notify.cpp), then EOF, and the descriptor closes when the peer closes.
TEST_F(CloseAfterPeerTls, ATlsClientThatEndsItsConnectionIsClosedAfterItsPeer) {
    ASSERT_NO_FATAL_FAILURE(connect_tls());
    TlsClient client;
    client.transport() = std::move(pair.client);
    const auto handle  = client.transport().native_handle();
    client.set_linger(20s);
    client.start();
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    ASSERT_EQ(closing(), 1u);
    EXPECT_TRUE(descriptor_open(handle));
    // The peer writes raw octets into the closing connection: the loop drops them undecrypted and resets nothing.
    const std::string noise(4096, 'n');
    EXPECT_EQ(::send(pair.server.native_handle(), noise.data(), static_cast<int>(noise.size()), 0), static_cast<int>(noise.size()));
    pump(100ms);
    EXPECT_TRUE(descriptor_open(handle)) << "what the peer sent ended the close: only its EOF, an error or the bound may";
    pair.server.close();
    EXPECT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "the peer closed: the descriptor must close";
    EXPECT_EQ(closing(), 0u);
}
