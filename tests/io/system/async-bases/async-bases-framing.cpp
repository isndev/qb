/**
 * @file qb/source/io/tests/system/async-bases/async-bases-framing.cpp
 * @brief Focused tests for async::input, async::output and async::io CRTP bases.
 *
 * These tests drive libev read/write readiness over a local connected socket pair without
 * relying on TCP timing across hosts. They exercise the protocol processing and output
 * drain paths that concrete transports inherit from the qb-io async bases.
 *
 * Portable by construction: the byte-stream endpoints are qb's own cross-platform
 * `qb::io::tcp` sockets on an ephemeral loopback port (replacing the original POSIX
 * `pipe()`/`socketpair()` + raw `::read`/`::write`, which do not exist for winsock
 * SOCKETs). The probe transports route I/O through `qb::io::tcp::socket::read()/write()` —
 * the very methods the production transports use — so the base's read/write/would-block
 * handling is exercised identically on Windows, Linux and macOS. The forced would-block /
 * hard-error paths use `qb::io::socket::set_last_errno()` (WSASetLastError on Windows /
 * errno on POSIX) so the base's `not_send_error()` / `system_error()` verdicts are the same
 * everywhere.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 *
 * @ingroup Tests
 */

#include <gtest/gtest.h>

#include <qb/io/async/io.h>
#include <qb/io/protocol/base.h>
#include <qb/io/system/sys__socket.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/system/allocator/pipe.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../shared/log_writer_drain.h"
#include "../../shared/loopback_fixture.h"

namespace {

class AsyncIoBaseTest : public ::testing::Test {
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

// A connected loopback socket pair. `probe` is the end the async base under test drives;
// `peer` is the far end the test injects bytes into / reads replies back from. The pair
// OWNS both sockets (they close on scope exit); a probe only BORROWS its end via
// SocketTransport, so the pair MUST outlive the probe. This mirrors the original ownership
// split (fds owned by PipePair, borrowed by FdTransport) and matters on Windows/wepoll,
// where the base must stop its watcher BEFORE the underlying SOCKET is closed.
struct StreamPair {
    qb::io::tcp::socket probe;
    qb::io::tcp::socket peer;
};

StreamPair
make_stream_pair() {
    qb::io::tcp::listener listener;
    if (listener.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
        throw std::runtime_error("make_stream_pair: listen_v4 failed");
    const std::uint16_t port = listener.local_endpoint().port();
    if (port == 0)
        throw std::runtime_error("make_stream_pair: ephemeral port is zero");

    qb::io::tcp::socket peer; // the far end (connector) — stays blocking for the test's raw I/O
    if (peer.connect_v4("127.0.0.1", port) != qb::io::SocketStatus::Done)
        throw std::runtime_error("make_stream_pair: connect_v4 failed");

    qb::io::tcp::socket probe; // the base-driven end (accepted) — the base makes it non-blocking
    if (listener.accept(probe) != qb::io::SocketStatus::Done)
        throw std::runtime_error("make_stream_pair: accept failed");

    listener.disconnect();
    return StreamPair{std::move(probe), std::move(peer)};
}

// The native handle type of a qb socket (SOCKET on Windows, int on POSIX) — deduced from the
// API so this test does not depend on the platform typedef's namespace.
using socket_handle_t = decltype(std::declval<qb::io::tcp::socket>().native_handle());

// View of one StreamPair end, exposing exactly the transport surface the async CRTP bases
// require. I/O routes through qb::io::tcp::socket::read()/write() (the production transport
// methods → identical cross-platform semantics). close() closes the socket, as a production
// transport's does: a standalone base's dispose() calls it once its watcher is stopped, and the
// peer then reads EOF (Huly QB-1060). The StreamPair still owns the socket -- closing it again
// at scope exit is a no-op -- and nothing else closes it, so the pair must still outlive the
// probe. native_handle() is the socket's own: invalid once it is closed.
class SocketTransport {
    qb::io::tcp::socket *_sock = nullptr;

public:
    SocketTransport() = default;
    explicit SocketTransport(qb::io::tcp::socket &sock) noexcept
        : _sock(&sock) {}

    [[nodiscard]] socket_handle_t
    native_handle() const noexcept {
        return _sock ? _sock->native_handle() : qb::io::invalid_socket;
    }

    void
    set_nonblocking(bool enabled) const noexcept {
        if (_sock)
            _sock->set_nonblocking(enabled);
    }

    int
    read(void *dst, std::size_t n) const noexcept {
        return _sock ? _sock->read(dst, n) : -1;
    }

    int
    write(const void *src, std::size_t n) const noexcept {
        return _sock ? _sock->write(src, n) : -1;
    }

    void
    close() noexcept {
        if (_sock)
            _sock->close();
    }
};

class PipeInputProbe : public qb::io::async::input<PipeInputProbe> {
    SocketTransport           _transport;
    qb::allocator::pipe<char> _in;

public:
    using base_io_t = qb::io::async::input<PipeInputProbe>;

    std::vector<std::string> messages;
    std::size_t              pending_read_events    = 0u;
    std::size_t              eof_events             = 0u;
    std::size_t              disconnected_events    = 0u;
    std::size_t              dispose_events         = 0u;
    int                      last_disconnect_reason = 0;

    explicit PipeInputProbe(qb::io::tcp::socket &sock) noexcept
        : _transport(sock) {}

    base_io_t &
    base() noexcept {
        return *this;
    }
    base_io_t const &
    base() const noexcept {
        return *this;
    }
    SocketTransport &
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
        constexpr std::size_t kChunk = 64u;
        auto                 *dst    = _in.allocate_back(kChunk);
        const auto            ret    = _transport.read(dst, kChunk);
        if (ret >= 0)
            _in.free_back(kChunk - static_cast<std::size_t>(ret));
        else
            _in.free_back(kChunk);
        return static_cast<int>(ret);
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
    close() noexcept {
        _transport.close();
    }

    void
    on(qb::io::async::event::pending_read &&event) noexcept {
        ++pending_read_events;
        EXPECT_EQ(event.bytes, pendingRead());
    }

    void
    on(qb::io::async::event::eof &&) noexcept {
        ++eof_events;
    }

    // Huly QB-256: both teardown hooks throw, after counting.
    bool throw_in_teardown = false;

    void
    on(qb::io::async::event::disconnected &&event) {
        ++disconnected_events;
        last_disconnect_reason = event.reason;
        if (throw_in_teardown)
            throw std::runtime_error("probe: on(disconnected) threw");
    }

    void
    on(qb::io::async::event::dispose &&) {
        ++dispose_events;
        if (throw_in_teardown)
            throw std::runtime_error("probe: on(dispose) threw");
    }
};

class FourByteInputProtocol : public qb::io::async::AProtocol<PipeInputProbe> {
    bool _invalidate_after_message;

public:
    explicit FourByteInputProtocol(PipeInputProbe &io, bool invalidate_after_message = false) noexcept
        : AProtocol(io)
        , _invalidate_after_message(invalidate_after_message) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        if (_invalidate_after_message)
            not_ok();
    }

    void
    reset() noexcept final {}
};

class OversizedInputProtocol : public qb::io::async::AProtocol<PipeInputProbe> {
public:
    explicit OversizedInputProtocol(PipeInputProbe &io) noexcept
        : AProtocol(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return 8u;
    }
    void
    onMessage(std::size_t) noexcept final {
        ADD_FAILURE();
    }
    void
    reset() noexcept final {}
};

class RejectingInputProtocol : public qb::io::async::AProtocol<PipeInputProbe> {
public:
    explicit RejectingInputProtocol(PipeInputProbe &io) noexcept
        : AProtocol(io) {
        not_ok();
    }

    std::size_t
    getMessageSize() noexcept final {
        return 0u;
    }
    void
    onMessage(std::size_t) noexcept final {}
    void
    reset() noexcept final {}
};

class PipeOutputProbe : public qb::io::async::output<PipeOutputProbe> {
    enum class write_mode { normal, would_block, hard_error };

    SocketTransport           _transport;
    qb::allocator::pipe<char> _out;
    std::size_t               _max_chunk             = 64u;
    std::size_t               _max_write_buffer_size = QB_MAX_WRITE_BUFFER_SIZE;
    write_mode                _write_mode            = write_mode::normal;

public:
    using base_io_t = qb::io::async::output<PipeOutputProbe>;

    std::size_t pending_write_events   = 0u;
    std::size_t eos_events             = 0u;
    std::size_t disconnected_events    = 0u;
    std::size_t dispose_events         = 0u;
    int         last_disconnect_reason = 0;

    explicit PipeOutputProbe(qb::io::tcp::socket &sock) noexcept
        : _transport(sock) {}

    base_io_t &
    base() noexcept {
        return *this;
    }
    base_io_t const &
    base() const noexcept {
        return *this;
    }
    SocketTransport &
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
        return _max_write_buffer_size;
    }

    void
    set_max_chunk(std::size_t max_chunk) noexcept {
        _max_chunk = max_chunk;
    }

    void
    set_max_write_buffer_size(std::size_t size) noexcept {
        _max_write_buffer_size = size;
    }

    void
    force_write_would_block() noexcept {
        _write_mode = write_mode::would_block;
    }

    void
    force_write_hard_error() noexcept {
        _write_mode = write_mode::hard_error;
    }

    int
    write() noexcept {
        if (_write_mode == write_mode::would_block) {
            qb::io::socket::set_last_errno(EWOULDBLOCK);
            return -1;
        }
        if (_write_mode == write_mode::hard_error) {
            qb::io::socket::set_last_errno(EPIPE);
            return -1;
        }

        const auto count = std::min(_max_chunk, _out.size());
        const auto ret   = _transport.write(_out.begin(), count);
        if (ret > 0)
            _out.free_front(static_cast<std::size_t>(ret));
        return static_cast<int>(ret);
    }

    void
    on(qb::io::async::event::pending_write &&event) noexcept {
        ++pending_write_events;
        EXPECT_EQ(event.bytes, pendingWrite());
    }

    void
    on(qb::io::async::event::eos &&) noexcept {
        ++eos_events;
    }

    // Huly QB-256: both teardown hooks throw, after counting.
    bool throw_in_teardown = false;

    void
    on(qb::io::async::event::disconnected &&event) {
        ++disconnected_events;
        last_disconnect_reason = event.reason;
        if (throw_in_teardown)
            throw std::runtime_error("probe: on(disconnected) threw");
    }

    void
    on(qb::io::async::event::dispose &&) {
        ++dispose_events;
        if (throw_in_teardown)
            throw std::runtime_error("probe: on(dispose) threw");
    }
};

class PipeDuplexProbe : public qb::io::async::io<PipeDuplexProbe> {
    enum class write_mode { normal, would_block, hard_error };

    SocketTransport           _transport;
    qb::allocator::pipe<char> _in;
    qb::allocator::pipe<char> _out;
    std::size_t               _max_chunk             = 64u;
    std::size_t               _max_write_buffer_size = QB_MAX_WRITE_BUFFER_SIZE;
    bool                      _force_read_overflow   = false;
    write_mode                _write_mode            = write_mode::normal;

public:
    using base_io_t = qb::io::async::io<PipeDuplexProbe>;

    std::vector<std::string> messages;
    std::size_t              pending_read_events    = 0u;
    std::size_t              pending_write_events   = 0u;
    std::size_t              eof_events             = 0u;
    std::size_t              eos_events             = 0u;
    std::size_t              disconnected_events    = 0u;
    std::size_t              dispose_events         = 0u;
    int                      last_disconnect_reason = 0;

    explicit PipeDuplexProbe(qb::io::tcp::socket &sock) noexcept
        : _transport(sock) {}

    base_io_t &
    base() noexcept {
        return *this;
    }
    base_io_t const &
    base() const noexcept {
        return *this;
    }
    SocketTransport &
    transport() noexcept {
        return _transport;
    }
    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }
    qb::allocator::pipe<char> &
    out() noexcept {
        return _out;
    }
    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _in.size();
    }
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out.size();
    }
    [[nodiscard]] std::size_t
    max_write_buffer_size() const noexcept {
        return _max_write_buffer_size;
    }

    void
    set_max_chunk(std::size_t max_chunk) noexcept {
        _max_chunk = max_chunk;
    }

    void
    set_max_write_buffer_size(std::size_t size) noexcept {
        _max_write_buffer_size = size;
    }

    void
    reset_state() noexcept {
        reset_io_state();
    }

    // reset_for_reconnect() is protected, like reset_io_state(): the client that reuses itself calls it.
    void
    reset_for_next_connection() {
        reset_for_reconnect();
    }

    // disconnect_now() is protected too: the standalone client that needs its teardown done calls it.
    void
    disconnect_now_for_test(int reason = 1) noexcept {
        disconnect_now(reason);
    }

    // When set, on(disconnected) calls start() -- from inside dispose(), the misuse QB-202 asserts.
    bool restart_on_disconnected = false;

    void
    force_read_overflow() noexcept {
        _force_read_overflow = true;
    }

    void
    force_write_would_block() noexcept {
        _write_mode = write_mode::would_block;
    }

    void
    force_write_hard_error() noexcept {
        _write_mode = write_mode::hard_error;
    }

    int
    read() noexcept {
        if (_force_read_overflow)
            return -2;

        constexpr std::size_t kChunk = 64u;
        auto                 *dst    = _in.allocate_back(kChunk);
        const auto            ret    = _transport.read(dst, kChunk);
        if (ret >= 0)
            _in.free_back(kChunk - static_cast<std::size_t>(ret));
        else
            _in.free_back(kChunk);
        return static_cast<int>(ret);
    }

    int
    write() noexcept {
        if (_write_mode == write_mode::would_block) {
            qb::io::socket::set_last_errno(EWOULDBLOCK);
            return -1;
        }
        if (_write_mode == write_mode::hard_error) {
            qb::io::socket::set_last_errno(EPIPE);
            return -1;
        }

        const auto count = std::min(_max_chunk, _out.size());
        const auto ret   = _transport.write(_out.begin(), count);
        if (ret > 0)
            _out.free_front(static_cast<std::size_t>(ret));
        return static_cast<int>(ret);
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
    close() noexcept {
        _transport.close();
    }

    void
    on(qb::io::async::event::pending_read &&event) noexcept {
        ++pending_read_events;
        EXPECT_EQ(event.bytes, pendingRead());
    }

    void
    on(qb::io::async::event::pending_write &&event) noexcept {
        ++pending_write_events;
        EXPECT_EQ(event.bytes, pendingWrite());
    }

    void
    on(qb::io::async::event::eof &&) noexcept {
        ++eof_events;
    }

    // Huly QB-294: the eos hook leaves the thread's last socket error at would-block, as any
    // non-blocking call that found nothing to do in it would (Windows reads WSAGetLastError()).
    bool stale_would_block_in_eos = false;

    void
    on(qb::io::async::event::eos &&) noexcept {
        ++eos_events;
        if (stale_would_block_in_eos) {
#ifdef _WIN32
            qb::io::socket::set_last_errno(QB_WINDOWS_WOULDBLOCK_ERROR);
#else
            qb::io::socket::set_last_errno(EWOULDBLOCK);
#endif
        }
    }

    // Huly QB-256: both teardown hooks throw, after counting.
    bool throw_in_teardown = false;

    void
    on(qb::io::async::event::disconnected &&event) {
        ++disconnected_events;
        last_disconnect_reason = event.reason;
        if (restart_on_disconnected)
            base().start();
        if (throw_in_teardown)
            throw std::runtime_error("probe: on(disconnected) threw");
    }

    void
    on(qb::io::async::event::dispose &&) {
        ++dispose_events;
        if (throw_in_teardown)
            throw std::runtime_error("probe: on(dispose) threw");
    }
};

class FourByteDuplexProtocol : public qb::io::async::AProtocol<PipeDuplexProbe> {
    bool _invalidate_after_message;
    bool _reply;

public:
    explicit FourByteDuplexProtocol(PipeDuplexProbe &io, bool invalidate_after_message = false, bool reply = false) noexcept
        : AProtocol(io)
        , _invalidate_after_message(invalidate_after_message)
        , _reply(reply) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        if (_reply)
            _io.publish(std::string_view{"pong"});
        if (_invalidate_after_message)
            not_ok();
    }

    void
    reset() noexcept final {}
};

class OversizedDuplexProtocol : public qb::io::async::AProtocol<PipeDuplexProbe> {
    std::size_t _reported_size;

public:
    OversizedDuplexProtocol(PipeDuplexProbe &io, std::size_t reported_size) noexcept
        : AProtocol(io)
        , _reported_size(reported_size) {}

    std::size_t
    getMessageSize() noexcept final {
        return _reported_size;
    }

    void
    onMessage(std::size_t) noexcept final {
        ADD_FAILURE();
    }

    void
    reset() noexcept final {}
};

class DisconnectingDuplexProtocol : public qb::io::async::AProtocol<PipeDuplexProbe> {
    bool _now; // disconnect_now() instead of disconnect(): inside the message loop the two must agree

public:
    explicit DisconnectingDuplexProtocol(PipeDuplexProbe &io, bool now = false) noexcept
        : AProtocol(io)
        , _now(now) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        _io.publish(std::string_view{"bye!"});
        if (_now)
            _io.disconnect_now_for_test(77);
        else
            _io.disconnect(77);
    }

    void
    reset() noexcept final {}
};

class NonFlushingDuplexProtocol : public qb::io::async::AProtocol<PipeDuplexProbe> {
    bool _seen = false;

public:
    explicit NonFlushingDuplexProtocol(PipeDuplexProbe &io) noexcept
        : AProtocol(io) {
        set_should_flush(false);
    }

    std::size_t
    getMessageSize() noexcept final {
        if (_seen)
            return 0u;
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        _seen = true;
        _io.messages.emplace_back(_io.in().begin(), size);
    }

    void
    reset() noexcept final {}
};

class RejectingDuplexProtocol : public qb::io::async::AProtocol<PipeDuplexProbe> {
public:
    explicit RejectingDuplexProtocol(PipeDuplexProbe &io) noexcept
        : AProtocol(io) {
        not_ok();
    }

    std::size_t
    getMessageSize() noexcept final {
        return 0u;
    }
    void
    onMessage(std::size_t) noexcept final {}
    void
    reset() noexcept final {}
};

// The real length-prefixed framer of qb (qb/io/protocol/base.h): a zero-length header is a framing
// error it reports by marking itself not-ok and returning 0 from getMessageSize() -- the one way a
// framer can invalidate itself without a message being delivered (Huly QB-293).
class SizedInputProtocol : public qb::protocol::base::size_as_header<PipeInputProbe, std::uint32_t> {
public:
    explicit SizedInputProtocol(PipeInputProbe &io) noexcept
        : size_as_header(io) {}

    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
    }
};

class SizedDuplexProtocol : public qb::protocol::base::size_as_header<PipeDuplexProbe, std::uint32_t> {
    bool _reply;

public:
    explicit SizedDuplexProtocol(PipeDuplexProbe &io, bool reply = false) noexcept
        : size_as_header(io)
        , _reply(reply) {}

    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        if (_reply)
            _io.publish(std::string_view{"pong"});
    }
};

// A frame: a 4-byte network-order length, then the payload.
std::string
sized_frame(std::string_view payload) {
    const auto  header = qb::protocol::base::size_as_header<PipeInputProbe, std::uint32_t>::Header(payload.size());
    std::string frame(reinterpret_cast<char const *>(&header), sizeof(header));
    frame.append(payload);
    return frame;
}

const std::string kZeroHeader(4, '\0');

// Answers its first message with `reply` and then asks to close once that is delivered.
class ReplyThenCloseDuplexProtocol : public qb::io::async::AProtocol<PipeDuplexProbe> {
    std::string _reply;

public:
    ReplyThenCloseDuplexProtocol(PipeDuplexProbe &io, std::string reply) noexcept
        : AProtocol(io)
        , _reply(std::move(reply)) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        _io.publish(std::string_view{_reply});
        _io.close_after_deliver();
    }

    void
    reset() noexcept final {}
};

void
run_nowait_iterations(int count = 16) {
    for (int i = 0; i < count; ++i)
        qb::io::async::run(EVRUN_NOWAIT);
}

/// Run `passes` non-blocking loop passes; return how many events they dispatched.
std::size_t
dispatched_over(int passes) {
    std::size_t events = 0;
    for (int i = 0; i < passes; ++i)
        events += static_cast<std::size_t>(qb::io::async::run(EVRUN_NOWAIT));
    return events;
}

/// What `sock`'s receive buffer holds, or nothing when the socket cannot say (it is closed).
std::optional<std::size_t>
readable_bytes(qb::io::tcp::socket &sock) {
#ifdef _WIN32
    u_long held = 0;
#else
    int held = 0;
#endif
    if (::QB_IOCTLSOCKET(sock.native_handle(), FIONREAD, &held) != 0)
        return std::nullopt;
    return static_cast<std::size_t>(held);
}

/// The peer writes `bytes`, then the case waits -- on a clock, not a pass count -- until they sit in the
/// probe's receive buffer. write() returning does not say they do: they may still be crossing the loopback.
/// On macOS a 4-byte write is usually not readable yet at the far end when write() returns: it is a few
/// microseconds later on an idle host, milliseconds on a loaded one -- longer than the fixed non-blocking
/// passes the cases below assert on last (Huly QB-1233). The count taken first is all the peer had sent:
/// every peer write the probe reads goes through here.
::testing::AssertionResult
deliver(StreamPair &pair, std::string_view bytes) {
    const auto before = readable_bytes(pair.probe);
    if (!before)
        return ::testing::AssertionFailure() << "the probe's receive buffer cannot be read: its socket is closed, nothing can reach it";
    if (pair.peer.write(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
        return ::testing::AssertionFailure() << "the peer could not write its " << bytes.size() << " bytes";
    const auto deadline = std::chrono::steady_clock::now() + qb::io::test::kLoopbackWaitBudget;
    for (;;) {
        const auto held = readable_bytes(pair.probe);
        if (!held)
            return ::testing::AssertionFailure() << "the probe's receive buffer could no longer be read";
        if (*held >= *before + bytes.size())
            return ::testing::AssertionSuccess();
        if (std::chrono::steady_clock::now() >= deadline)
            return ::testing::AssertionFailure() << "the peer's " << bytes.size() << " bytes did not reach the probe within "
                                                 << qb::io::test::kLoopbackWaitBudget.count() << " ms";
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
}

/// What the peer reads of the session's output: `n` bytes, waited for on a clock -- they cross the loopback
/// too, and a blocking read waited for ever when they never came (Huly QB-1233) -- then whatever had already
/// arrived behind them: a reply sent twice shows when its copy is there. A short read is a short string.
std::string
reply_of(qb::io::tcp::socket &peer, std::size_t n) {
    std::string reply(n, '\0');
    reply.resize(qb::io::test::read_exact_within(peer, reply.data(), n));
    if (const auto behind = readable_bytes(peer); behind && *behind > 0) {
        std::string more(*behind, '\0');
        if (const int got = peer.read(more.data(), more.size()); got > 0)
            reply.append(more.data(), static_cast<std::size_t>(got));
    }
    return reply;
}

/// Non-blocking loop passes until `done()` holds, for at most the loopback budget: for an event whose cause
/// crosses the loopback after the call that sent it -- the peer's FIN -- which no number of passes is sure
/// to outlast (Huly QB-1233). Not qb::io::async::run_until(), which has no deadline.
template <typename Done>
::testing::AssertionResult
pass_until(Done &&done, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + qb::io::test::kLoopbackWaitBudget;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return ::testing::AssertionFailure() << what << ": not within " << qb::io::test::kLoopbackWaitBudget.count() << " ms";
        if (qb::io::async::run(EVRUN_NOWAIT) == 0)
            std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return ::testing::AssertionSuccess();
}

} // namespace

TEST_F(AsyncIoBaseTest, InputReadsFramesAndReportsPendingThenEof) {
    auto           pair = make_stream_pair();
    PipeInputProbe input{pair.probe};
    ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);

    input.base().start();
    ASSERT_TRUE(deliver(pair, "abcdefghZ"));

    run_nowait_iterations();

    EXPECT_EQ(input.messages, (std::vector<std::string>{"abcd", "efgh"}));
    EXPECT_EQ(input.pendingRead(), 1u);
    EXPECT_EQ(input.pending_read_events, 1u);
    EXPECT_EQ(input.eof_events, 0u);
    EXPECT_EQ(input.base().bytes_read(), 9u);
    EXPECT_EQ(input.base().messages_processed(), 2u);
    EXPECT_TRUE(input.base().has_pending_data());

    input.flush(input.pendingRead());
    ASSERT_TRUE(deliver(pair, "wxyz"));
    run_nowait_iterations();

    EXPECT_EQ(input.messages, (std::vector<std::string>{"abcd", "efgh", "wxyz"})); // not back(): a miss must fail, not crash the run
    EXPECT_EQ(input.pendingRead(), 0u);
    EXPECT_EQ(input.eof_events, 1u);
}

TEST_F(AsyncIoBaseTest, InputDisconnectsOnProtocolErrorAndOversizedFrame) {
    {
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input, true), nullptr);

        input.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(input.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.dispose_events, 1u);
        EXPECT_EQ(input.last_disconnect_reason, -1);
        EXPECT_FALSE(input.base().is_connected());
    }

    {
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<OversizedInputProtocol>(input), nullptr);
        input.base().set_max_message_size(4u);

        input.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.last_disconnect_reason, -2);
    }
}

TEST_F(AsyncIoBaseTest, InputDisposesWhenProtocolIsInvalidOrClearedBeforeRead) {
    {
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);
        ASSERT_NE(input.base().protocol(), qb::io::async::no_protocol()); // a real protocol is set
        input.base().protocol()->not_ok();

        input.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.dispose_events, 1u);
        EXPECT_EQ(input.last_disconnect_reason, -1);
        EXPECT_FALSE(input.base().is_connected());
    }

    {
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);
        input.base().clear_protocols();
        EXPECT_EQ(input.base().protocol(), qb::io::async::no_protocol());

        input.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.dispose_events, 1u);
        EXPECT_FALSE(input.base().is_connected());
    }

    {
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        EXPECT_EQ(input.base().switch_protocol<RejectingInputProtocol>(input), nullptr);
        EXPECT_EQ(input.base().protocol(), qb::io::async::no_protocol());
    }
}

TEST_F(AsyncIoBaseTest, OutputDrainsPartialWritesAndPublishesEos) {
    auto            pair = make_stream_pair();
    PipeOutputProbe output{pair.probe};
    output.set_max_chunk(3u);

    output.base().start();
    output.base().publish(std::string_view{"abcdef"});

    run_nowait_iterations();
    EXPECT_EQ(output.pending_write_events, 1u);
    ASSERT_EQ(output.eos_events, 1u);
    EXPECT_EQ(output.base().bytes_written(), 6u);
    ASSERT_FALSE(output.base().has_pending_data());

    EXPECT_EQ(reply_of(pair.peer, 6), "abcdef");
}

TEST_F(AsyncIoBaseTest, OutputDisconnectIsIdempotentAndReportsReason) {
    auto            pair = make_stream_pair();
    PipeOutputProbe output{pair.probe};

    output.base().start();
    EXPECT_TRUE(output.base().is_connected());
    output.base().disconnect(0);
    run_nowait_iterations();

    EXPECT_EQ(output.disconnected_events, 1u);
    EXPECT_EQ(output.dispose_events, 1u);
    EXPECT_EQ(output.last_disconnect_reason, static_cast<int>(qb::io::async::event::disconnect_reason::user_initiated));
    EXPECT_FALSE(output.base().is_connected());

    output.base().disconnect(42);
    run_nowait_iterations();
    EXPECT_EQ(output.disconnected_events, 1u);
}

TEST_F(AsyncIoBaseTest, OutputPublishOverflowRollsBackAndDisconnects) {
    auto            pair = make_stream_pair();
    PipeOutputProbe output{pair.probe};
    output.set_max_write_buffer_size(4u);

    output.base().start();
    output.base().publish(std::string_view{"ab"});
    ASSERT_EQ(output.pendingWrite(), 2u);

    output.base().publish(std::string_view{"cdef"});

    EXPECT_EQ(output.pendingWrite(), 4u);
    EXPECT_EQ(std::string_view(output.out().begin(), output.out().size()), "abcd");
    EXPECT_EQ(output.base().disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));

    output.base().publish(std::string_view{"ignored"});
    EXPECT_EQ(std::string_view(output.out().begin(), output.out().size()), "abcd");
}

TEST_F(AsyncIoBaseTest, OutputWriteErrorsDistinguishWouldBlockFromHardFailure) {
    {
        auto            pair = make_stream_pair();
        PipeOutputProbe output{pair.probe};
        output.force_write_would_block();

        output.base().start();
        output.base().publish(std::string_view{"held"});
        run_nowait_iterations();

        EXPECT_EQ(output.pendingWrite(), 4u);
        EXPECT_TRUE(output.base().has_pending_data());
        EXPECT_TRUE(output.base().is_connected());
        EXPECT_EQ(output.disconnected_events, 0u);
        EXPECT_EQ(output.eos_events, 0u);
    }

    {
        auto            pair = make_stream_pair();
        PipeOutputProbe output{pair.probe};
        output.force_write_hard_error();

        output.base().start();
        output.base().publish(std::string_view{"boom"});
        run_nowait_iterations();

        EXPECT_EQ(output.disconnected_events, 1u);
        EXPECT_EQ(output.dispose_events, 1u);
        EXPECT_NE(output.base().system_error(), 0);
        EXPECT_FALSE(output.base().is_connected());
    }
}

TEST_F(AsyncIoBaseTest, DuplexProcessesInputAndDrainsReplyInSameEventCycle) {
    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    session.set_max_chunk(2u);
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, false, true), nullptr);

    session.base().start();
    ASSERT_TRUE(deliver(pair, "abcdZ"));

    run_nowait_iterations();

    EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd"}));
    EXPECT_EQ(session.pendingRead(), 1u);
    EXPECT_EQ(session.pending_read_events, 1u);
    EXPECT_EQ(session.eof_events, 0u);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.pending_write_events, 1u);
    EXPECT_EQ(session.eos_events, 1u);
    EXPECT_EQ(session.base().bytes_read(), 5u);
    EXPECT_EQ(session.base().bytes_written(), 4u);
    EXPECT_EQ(session.base().messages_processed(), 1u);
    EXPECT_TRUE(session.base().has_pending_read());
    EXPECT_FALSE(session.base().has_pending_write());

    EXPECT_EQ(reply_of(pair.peer, 4), "pong");
}

TEST_F(AsyncIoBaseTest, DuplexProtocolLifecycleAccessorsAndNoProtocolDisposal) {
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        const auto     &const_base = session.base();

        EXPECT_EQ(session.base().protocol(), qb::io::async::no_protocol());
        EXPECT_EQ(const_base.protocol(), qb::io::async::no_protocol());
        EXPECT_FALSE(session.base().has_pending_read());
        EXPECT_FALSE(session.base().has_pending_write());
        EXPECT_EQ(session.base().max_message_size(), QB_MAX_MESSAGE_SIZE);

        EXPECT_EQ(session.base().switch_protocol<RejectingDuplexProtocol>(session), nullptr);
        EXPECT_EQ(session.base().protocol(), qb::io::async::no_protocol());

        auto *protocol = session.base().switch_protocol<FourByteDuplexProtocol>(session);
        ASSERT_NE(protocol, nullptr);
        EXPECT_EQ(session.base().protocol(), protocol);

        session.base().start();
        EXPECT_TRUE(session.base().is_reading());
        EXPECT_FALSE(session.base().is_writing());

        session.base().publish(std::string_view{"queued"});
        EXPECT_TRUE(session.base().has_pending_write());
        EXPECT_TRUE(session.base().is_writing());
        EXPECT_EQ(session.pendingWrite(), 6u);

        session.base().clear_protocols();
        EXPECT_EQ(session.base().protocol(), qb::io::async::no_protocol());
        EXPECT_FALSE(session.base().has_pending_read());
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_FALSE(session.base().is_connected());
    }
}

TEST_F(AsyncIoBaseTest, DuplexClearedProtocolAndCloseAfterDeliverDisposeCleanly) {
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.base().clear_protocols();

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.messages.size(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_FALSE(session.base().is_connected());
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, true, true), nullptr);

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.pendingWrite(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(session.base().bytes_written(), 4u);
        EXPECT_FALSE(session.base().is_connected());

        EXPECT_EQ(reply_of(pair.peer, 4), "pong");

        session.reset_state();
        EXPECT_TRUE(session.base().is_connected());
    }
}

TEST_F(AsyncIoBaseTest, DuplexWriteOverflowRollsBackAndBlocksFurtherPublish) {
    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    session.set_max_write_buffer_size(4u);

    session.base().start();
    session.base().publish(std::string_view{"ab"});
    ASSERT_EQ(session.pendingWrite(), 2u);

    session.base().publish(std::string_view{"cdef"});

    EXPECT_EQ(session.pendingWrite(), 4u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd");
    EXPECT_EQ(session.base().disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));

    session.base().publish(std::string_view{"ignored"});
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd");
}

TEST_F(AsyncIoBaseTest, DuplexDisconnectsOnInvalidProtocolAndReadOverflow) {
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, true), nullptr);

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -1);
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_EQ(session.base().switch_protocol<RejectingDuplexProtocol>(session), nullptr);
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, false, false), nullptr);
        session.force_read_overflow();

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.messages.size(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -3);
    }
}

TEST_F(AsyncIoBaseTest, DuplexProtocolEdgeCasesPreserveFlushAndDeferredDrainSemantics) {
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<OversizedDuplexProtocol>(session, 8u), nullptr);
        session.base().set_max_message_size(4u);

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_TRUE(session.messages.empty());
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -2);
        EXPECT_FALSE(session.base().is_connected());
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<DisconnectingDuplexProtocol>(session), nullptr);

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.pendingWrite(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, 77);

        EXPECT_EQ(reply_of(pair.peer, 4), "bye!");
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<NonFlushingDuplexProtocol>(session), nullptr);

        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 4u);
        EXPECT_EQ(session.pending_read_events, 1u);
        EXPECT_TRUE(session.base().has_pending_read());
    }
}

TEST_F(AsyncIoBaseTest, DuplexWriteErrorsDistinguishWouldBlockFromHardFailure) {
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.force_write_would_block();

        session.base().start();
        session.base() << std::string_view{"held"};
        run_nowait_iterations();

        EXPECT_EQ(session.pendingWrite(), 4u);
        EXPECT_TRUE(session.base().has_pending_write());
        EXPECT_TRUE(session.base().is_connected());
        EXPECT_EQ(session.disconnected_events, 0u);
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.force_write_hard_error();

        session.base().start();
        session.base().publish(std::string_view{"boom"});
        run_nowait_iterations();

        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_NE(session.base().system_error(), 0);
        EXPECT_FALSE(session.base().is_connected());
    }
}

// -------------------------------------------------------------------------------------------
// An io object reused for its NEXT connection (Huly QB-202) -- the shape of every client that is
// itself the io of the connections it opens (qbm-http's HTTP/2 client, qbm-pgsql, qbm-redis).
// dispose() and start() never touch the buffers: a reply published and disconnected in the same
// tick stays in out(), and the next start() on the same object sends it first -- on the HTTP/2
// client that put a request ahead of the connection preface. reset_for_reconnect() leaves nothing
// of the previous connection -- buffers, protocol -- and start() clears the dispose latches.
// dispose() ends the transport the connection ended with (Huly QB-1060): the next connection comes
// with its own.
// -------------------------------------------------------------------------------------------
TEST_F(AsyncIoBaseTest, DuplexResetForReconnectLeavesNothingOfThePreviousConnection) {
    auto            pair = make_stream_pair();
    auto            next = make_stream_pair(); // the next connection's; declared first, it outlives the probe
    PipeDuplexProbe session{pair.probe};
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
    session.base().start();

    session.base().publish(std::string_view{"stale"}); // queued, never flushed: the teardown runs first
    auto *partial = session.in().allocate_back(2);     // and half a frame read, never framed
    std::memcpy(partial, "ha", 2);
    session.base().disconnect();
    run_nowait_iterations();
    ASSERT_EQ(session.dispose_events, 1u);
    ASSERT_EQ(session.pendingWrite(), 5u) << "dispose() keeps out(): the defect's precondition";
    ASSERT_EQ(session.pendingRead(), 2u) << "dispose() keeps in(): the defect's precondition";

    session.reset_for_next_connection();
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.base().protocol(), qb::io::async::no_protocol());
    EXPECT_FALSE(session.base().is_connected()) << "the dispose latches are start()'s to clear, not the reset's";

    // The next connection on the same object, over its own transport: only what IT publishes
    // reaches the peer -- "stalenext" was the defect.
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
    session.transport() = SocketTransport{next.probe};
    session.base().start();
    EXPECT_TRUE(session.base().is_connected()) << "start() clears the dispose latches";
    session.base().publish(std::string_view{"next"});
    run_nowait_iterations();
    EXPECT_EQ(reply_of(next.peer, 4), "next");
}

// What was published BEFORE start() leaves with it (Huly QB-202): publish() asks for EV_WRITE on a
// watcher that is not running yet, and start() used to arm EV_READ alone over it -- the bytes then
// waited for the next publish(). That is the shape of a command a reconnecting client re-issued
// during the disconnect, and of a greeting published in a session constructor.
TEST_F(AsyncIoBaseTest, DuplexStartSendsWhatWasPublishedBeforeIt) {
    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
    session.base().publish(std::string_view{"early"});
    ASSERT_EQ(session.pendingWrite(), 5u);

    session.base().start();
    run_nowait_iterations();
    EXPECT_EQ(session.pendingWrite(), 0u) << "the bytes published before start() must have left";

    EXPECT_EQ(reply_of(pair.peer, 5), "early");
}

// disconnect_now() (Huly QB-202): the teardown a standalone client needs done before its disconnect
// returns -- so that a connect() issued right after cannot overtake it. Outside the message loop it
// runs in the call, with no loop pass; from inside a protocol handler it cannot (the io dispatch is
// on the stack) and behaves exactly as disconnect() does there: the reply drains as far as the socket
// takes it at once, and the teardown runs in the same pass, right after that dispatch returns.
TEST_F(AsyncIoBaseTest, DuplexDisconnectNowTearsDownBeforeReturning) {
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.base().start();

        session.disconnect_now_for_test(42);
        EXPECT_EQ(session.disconnected_events, 1u) << "no loop pass ran: the teardown must have happened in the call";
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, 42);
        EXPECT_FALSE(session.base().is_connected());

        run_nowait_iterations();
        EXPECT_EQ(session.disconnected_events, 1u) << "the event the call fed must not tear down a second time";
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<DisconnectingDuplexProtocol>(session, true), nullptr);
        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingWrite(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, 77);
        EXPECT_EQ(reply_of(pair.peer, 4), "bye!") << "from a handler, the reply drains first, as with disconnect()";
    }

    {
        // The drain never holds the teardown back: when the reply cannot leave (the socket would
        // block), the event disconnect() fed is the last one pending, libev runs it right after the
        // handler's dispatch returns, and it tears down in the SAME pass -- nothing a connect() could
        // complete in between.
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<DisconnectingDuplexProtocol>(session, true), nullptr);
        session.force_write_would_block();
        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));

        for (int pass = 0; pass < 100 && session.messages.empty(); ++pass)
            qb::io::async::run(EVRUN_NOWAIT);
        ASSERT_EQ(session.messages, (std::vector<std::string>{"data"}));
        ASSERT_EQ(session.pendingWrite(), 4u) << "the reply could not leave: the case this block is about";
        EXPECT_EQ(session.disconnected_events, 1u) << "the pass that ran the handler must also have run the teardown";
        EXPECT_EQ(session.last_disconnect_reason, 77);
        EXPECT_FALSE(session.base().is_connected());
    }
}

// A teardown hook that throws (Huly QB-256). dispose() runs the derived class's
// on(event::disconnected&&) and then steps nothing may skip -- a standalone object's watcher stopped,
// on(event::dispose&&) fired -- and a hook that threw skipped them. The listener contained the
// exception, so nothing crashed: the watcher stayed armed on a disposed io, and a ready socket then
// dispatched it on every pass while dispose() returned at once. In each case both hooks throw; the
// teardown must complete anyway, and the socket must wake nothing afterwards.

TEST_F(AsyncIoBaseTest, InputTeardownCompletesWhenItsHooksThrow) {
    auto           pair = make_stream_pair();
    PipeInputProbe input{pair.probe};
    ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);
    input.throw_in_teardown = true;
    input.base().start();

    input.base().disconnect(0);
    run_nowait_iterations();
    EXPECT_EQ(input.disconnected_events, 1u);
    EXPECT_EQ(input.dispose_events, 1u) << "on(dispose) must run after a throwing on(disconnected)";
    EXPECT_FALSE(input.base().is_connected());

    ASSERT_TRUE(deliver(pair, "data"));
    EXPECT_EQ(dispatched_over(20), 0u) << "the watcher must be stopped: bytes for a disposed input wake nothing";
}

TEST_F(AsyncIoBaseTest, OutputTeardownCompletesWhenItsHooksThrow) {
    auto            pair = make_stream_pair();
    PipeOutputProbe output{pair.probe};
    output.throw_in_teardown = true;
    output.force_write_would_block(); // the bytes stay queued: the watcher is armed for writing
    output.base().start();
    output.base().publish(std::string_view{"abcd"});

    output.base().disconnect(0);
    run_nowait_iterations();
    EXPECT_EQ(output.disconnected_events, 1u);
    EXPECT_EQ(output.dispose_events, 1u) << "on(dispose) must run after a throwing on(disconnected)";
    EXPECT_FALSE(output.base().is_connected());

    EXPECT_EQ(dispatched_over(20), 0u) << "the watcher must be stopped: a writable socket wakes nothing";
}

TEST_F(AsyncIoBaseTest, DuplexTeardownCompletesWhenItsHooksThrow) {
    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
    session.throw_in_teardown = true;
    session.base().start();

    session.base().disconnect(0);
    run_nowait_iterations();
    EXPECT_EQ(session.disconnected_events, 1u);
    EXPECT_EQ(session.dispose_events, 1u) << "on(dispose) must run after a throwing on(disconnected)";
    EXPECT_FALSE(session.base().is_connected());

    ASSERT_TRUE(deliver(pair, "data"));
    EXPECT_EQ(dispatched_over(20), 0u) << "the watcher must be stopped: bytes for a disposed io wake nothing";
}

// disconnect_now() is noexcept and runs dispose() in the call: a hook that escaped it would end the
// process in std::terminate.
TEST_F(AsyncIoBaseTest, DuplexDisconnectNowCompletesWhenItsHooksThrow) {
    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
    session.throw_in_teardown = true;
    session.base().start();

    session.disconnect_now_for_test(42);
    EXPECT_EQ(session.disconnected_events, 1u);
    EXPECT_EQ(session.dispose_events, 1u) << "on(dispose) must run after a throwing on(disconnected)";
    EXPECT_EQ(session.last_disconnect_reason, 42);
    EXPECT_FALSE(session.base().is_connected());

    ASSERT_TRUE(deliver(pair, "data"));
    EXPECT_EQ(dispatched_over(20), 0u) << "the watcher must be stopped: bytes for a disposed io wake nothing";
}

// ---------------------------------------------------------------------------------------------
// Huly QB-1060 -- a standalone base's dispose() stopped its watcher and left the transport as it
// was, until the object connected again or died: the peer of a disconnect(), or of a framing error,
// never saw EOF, and the end that read its peer's EOF stayed open (CLOSE_WAIT). dispose() now ends
// the transport once the watcher is stopped: closed when the peer's EOF or a socket error ended the
// connection, shut down for sending when this object did -- the peer reads EOF after everything
// queued, and nothing is reset, input left unread included. Each base, each path. (The production
// composition, a tcp client, is in tcp/standalone-dispose-ends-transport.cpp.)
// ---------------------------------------------------------------------------------------------

namespace {

/// What `peer` reads up to a clean EOF within `budget`, or nothing when the connection stays open
/// (a peer never blocks here: it reads "would block" until the budget runs out) or is reset.
std::optional<std::string>
read_to_eof(qb::io::tcp::socket &peer, std::chrono::milliseconds budget = qb::io::test::kLoopbackWaitBudget) {
    peer.set_nonblocking(true);
    const auto            deadline = std::chrono::steady_clock::now() + budget;
    std::string           received;
    std::array<char, 256> buffer{};
    do {
        const int n = peer.read(buffer.data(), buffer.size());
        if (n > 0) {
            received.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        // tcp::socket::read() reports the peer's clean close as -1 with the error cleared.
        const int error = qb::io::socket::get_last_errno();
        if (error == 0)
            return received;
        if (!qb::io::socket::not_recv_error(error))
            return std::nullopt; // a reset, not the clean end a disposed transport sends
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
}

} // namespace

TEST_F(AsyncIoBaseTest, InputDisposeEndsItsTransport) {
    {
        // This object ends it: the sending side is shut down, the peer reads EOF.
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);
        input.base().start();

        input.base().disconnect(0);
        run_nowait_iterations();
        ASSERT_EQ(input.dispose_events, 1u);
        EXPECT_EQ(read_to_eof(pair.peer), std::optional<std::string>{""}) << "the peer of a disconnect() must see EOF";
    }
    {
        // The peer ends it: the end that read its EOF is closed, not left in CLOSE_WAIT.
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);
        input.base().start();

        pair.peer.disconnect();
        ASSERT_TRUE(pass_until([&] { return input.dispose_events != 0u; }, "the dispose the peer's FIN causes"));
        ASSERT_EQ(input.dispose_events, 1u);
        EXPECT_FALSE(pair.probe.is_open()) << "an input whose peer hung up must not keep its end open";
    }
}

TEST_F(AsyncIoBaseTest, OutputDisposeEndsItsTransport) {
    {
        // This object ends it: what was delivered stays delivered, then the peer reads EOF.
        auto            pair = make_stream_pair();
        PipeOutputProbe output{pair.probe};
        output.base().start();
        output.base().publish(std::string_view{"last"});
        run_nowait_iterations();
        ASSERT_EQ(output.pendingWrite(), 0u);

        output.base().disconnect(0);
        run_nowait_iterations();
        ASSERT_EQ(output.dispose_events, 1u);
        EXPECT_EQ(read_to_eof(pair.peer), std::optional<std::string>{"last"}) << "the delivery, then EOF";
    }
    {
        // A write error ends it: the transport is closed.
        auto            pair = make_stream_pair();
        PipeOutputProbe output{pair.probe};
        output.force_write_hard_error();
        output.base().start();
        output.base().publish(std::string_view{"lost"});
        run_nowait_iterations();
        ASSERT_EQ(output.dispose_events, 1u);
        EXPECT_FALSE(pair.probe.is_open()) << "an output whose socket failed must not keep it open";
    }
}

TEST_F(AsyncIoBaseTest, DuplexDisposeEndsItsTransportOnEveryPath) {
    {
        // An explicit disconnect().
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.base().start();

        session.base().disconnect();
        run_nowait_iterations();
        ASSERT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(read_to_eof(pair.peer), std::optional<std::string>{""}) << "the peer of a disconnect() must see EOF";
    }
    {
        // A framing error: the protocol marks itself not ok after the message it delivered.
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, true), nullptr);
        session.base().start();
        ASSERT_TRUE(deliver(pair, "data"));

        for (int pass = 0; pass < 100 && session.dispose_events == 0u; ++pass)
            qb::io::async::run(EVRUN_NOWAIT);
        ASSERT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -1);
        EXPECT_EQ(read_to_eof(pair.peer), std::optional<std::string>{""}) << "the peer of a connection ended by a framing error must see EOF";
    }
    {
        // The peer's own EOF: the end that read it must not stay open (CLOSE_WAIT).
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.base().start();

        pair.peer.disconnect();
        ASSERT_TRUE(pass_until([&] { return session.dispose_events != 0u; }, "the dispose the peer's FIN causes"));
        ASSERT_EQ(session.dispose_events, 1u);
        EXPECT_FALSE(pair.probe.is_open()) << "a client whose peer hung up must not keep its end open";
    }
    {
        // This object ends it with the peer's bytes still unread: closing would reset the peer -- its next
        // send fails, and on Windows it also drops what it has not read yet, the reply below. The end is a
        // shutdown of the sending side: the reply, then a clean EOF, and the peer may still send.
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.base().start();
        session.base().publish(std::string_view{"reply"});
        run_nowait_iterations();
        ASSERT_EQ(session.pendingWrite(), 0u);

        ASSERT_TRUE(deliver(pair, "unread")); // in the probe's buffer, unread: no loop pass reads it, the disconnect is handled first
        session.base().disconnect();
        run_nowait_iterations();
        ASSERT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(read_to_eof(pair.peer), std::optional<std::string>{"reply"}) << "the reply, then EOF -- not a reset that loses it";
        EXPECT_EQ(pair.peer.write("more", 4), 4) << "the connection is half-closed, not reset: the peer may still finish sending";
    }
}

// ---------------------------------------------------------------------------------------------
// Huly QB-292 -- `component << a << b` admitted only `a`: operator<< returned the RAW output
// buffer, so every later operand skipped the write-buffer cap and the disconnection check.
// ---------------------------------------------------------------------------------------------

TEST_F(AsyncIoBaseTest, OutputChainedInsertionAdmitsEveryOperand) {
    EXPECT_TRUE((std::is_same_v<decltype(std::declval<PipeOutputProbe &>() << std::string_view{}), PipeOutputProbe &>) )
        << "operator<< must return the component, so the next operand of the chain is admitted too";

    auto            pair = make_stream_pair();
    PipeOutputProbe output{pair.probe};
    output.set_max_write_buffer_size(4u);
    output.base().start();

    output << std::string_view{"ab"} << std::string_view{"cdef"}; // the SECOND operand crosses the cap
    EXPECT_EQ(std::string_view(output.out().begin(), output.out().size()), "abcd") << "rolled back to the cap";
    EXPECT_EQ(output.base().disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));

    output << std::string_view{"x"} << std::string_view{"y"};
    EXPECT_EQ(std::string_view(output.out().begin(), output.out().size()), "abcd") << "nothing is appended once refused";
}

TEST_F(AsyncIoBaseTest, DuplexChainedInsertionAdmitsEveryOperand) {
    EXPECT_TRUE((std::is_same_v<decltype(std::declval<PipeDuplexProbe &>() << std::string_view{}), PipeDuplexProbe &>) )
        << "operator<< must return the component, so the next operand of the chain is admitted too";

    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    session.set_max_write_buffer_size(4u);
    session.base().start();

    session << std::string_view{"ab"} << std::string_view{"cdef"};
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd") << "rolled back to the cap";
    EXPECT_EQ(session.base().disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));

    session << std::string_view{"x"} << std::string_view{"y"};
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "abcd") << "nothing is appended once refused";
}

// ---------------------------------------------------------------------------------------------
// Huly QB-293 -- a framer that rejects its input invalidates itself and answers 0 (qb's own
// size_as_header on a zero-length header). The framing loop took that 0 for "need more bytes" and
// reported success: the connection outlived the protocol error until another event -- never, from a
// silent peer. Each case below ends with the peer silent, so only the event that carried the bad
// bytes can deliver the verdict.
// ---------------------------------------------------------------------------------------------

TEST_F(AsyncIoBaseTest, InputDisconnectsOnAFramingErrorInTheEventThatCarriedIt) {
    auto           pair = make_stream_pair();
    PipeInputProbe input{pair.probe};
    ASSERT_NE(input.base().switch_protocol<SizedInputProtocol>(input), nullptr);
    input.base().start();

    const auto bytes = sized_frame("abc") + kZeroHeader; // a good frame, then a zero-length header
    ASSERT_TRUE(deliver(pair, bytes));
    run_nowait_iterations();

    EXPECT_EQ(input.messages, (std::vector<std::string>{"abc"})) << "the frame before the error is delivered";
    EXPECT_EQ(input.disconnected_events, 1u);
    EXPECT_EQ(input.last_disconnect_reason, -1) << "a protocol error";
    EXPECT_FALSE(input.base().is_connected());
}

TEST_F(AsyncIoBaseTest, DuplexDisconnectsOnAFramingErrorInTheEventThatCarriedIt) {
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<SizedDuplexProtocol>(session), nullptr);
        session.base().start();

        ASSERT_TRUE(deliver(pair, kZeroHeader));
        run_nowait_iterations();

        EXPECT_TRUE(session.messages.empty());
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -1) << "a protocol error, with nothing left to deliver";
        EXPECT_FALSE(session.base().is_connected());
    }
    {
        // Output published for the frames before the error is delivered first, then the connection
        // ends -- the verdict the in-loop protocol check already gave (close-after-deliver).
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<SizedDuplexProtocol>(session, true), nullptr);
        session.base().start();

        const auto bytes = sized_frame("abc") + kZeroHeader;
        ASSERT_TRUE(deliver(pair, bytes));
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"abc"}));
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_FALSE(session.base().is_connected());

        EXPECT_EQ(reply_of(pair.peer, 4), "pong");
    }
}

// ---------------------------------------------------------------------------------------------
// Huly QB-294 -- the graceful close of close_after_deliver() is the one way the io base reaches its
// error label with no reason. On Windows that label returned without disposing whenever the thread's
// LAST socket error read would-block -- a stale value any earlier call leaves (here: the eos hook) --
// and the connection never closed. POSIX never had the exemption: there this case is the control.
// ---------------------------------------------------------------------------------------------

TEST_F(AsyncIoBaseTest, DuplexCloseAfterDeliverDisposesWhateverTheLastSocketErrorSays) {
    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    session.stale_would_block_in_eos = true;
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, true, true), nullptr);
    session.base().start();

    ASSERT_TRUE(deliver(pair, "data"));
    run_nowait_iterations();

    EXPECT_EQ(session.eos_events, 1u) << "the final delivery is reported first";
    EXPECT_EQ(session.disconnected_events, 1u) << "then the connection ends, whatever the last socket error";
    EXPECT_EQ(session.dispose_events, 1u);
    EXPECT_FALSE(session.base().is_connected());

    EXPECT_EQ(reply_of(pair.peer, 4), "pong");
}

// The same graceful close, with an output larger than the socket takes at once and a peer that keeps
// writing: a read readiness then arrives ALONE (the socket is full, so no EV_WRITE comes with it). The
// read is skipped (the protocol is closing), nothing is written, and the event fell to the error label
// with no reason: the connection was disposed at once and the undelivered output lost. Before QB-294
// a stale would-block masked this on Windows; on POSIX it always happened.
TEST_F(AsyncIoBaseTest, DuplexCloseAfterDeliverDeliversEverythingWhileThePeerKeepsWriting) {
    constexpr std::size_t kReply = std::size_t{64} << 20; // far more than both socket buffers hold
    std::string           reply(kReply, 'r');
    reply.back() = '!';

    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    session.set_max_chunk(std::size_t{1} << 20);
    ASSERT_NE(session.base().switch_protocol<ReplyThenCloseDuplexProtocol>(session, reply), nullptr);
    session.base().start();
    ASSERT_TRUE(deliver(pair, "data"));

    // Write until the socket is full: the pending output, and what the peer holds, stop moving -- unchanged
    // across passes for 100 ms, a clock and not a pass count: what frees room in the probe's socket crosses
    // the loopback (Huly QB-1233).
    auto       last      = std::make_pair(session.pendingWrite(), readable_bytes(pair.peer).value_or(0u));
    auto       still     = std::chrono::steady_clock::now();
    const auto settle_by = still + qb::io::test::kLoopbackWaitBudget;
    bool       settled   = false;
    while (!settled && std::chrono::steady_clock::now() < settle_by) {
        qb::io::async::run(EVRUN_NOWAIT);
        const auto now = std::make_pair(session.pendingWrite(), readable_bytes(pair.peer).value_or(0u));
        if (now != last) {
            last  = now;
            still = std::chrono::steady_clock::now();
        } else {
            settled = std::chrono::steady_clock::now() - still >= std::chrono::milliseconds{100};
        }
    }
    ASSERT_TRUE(settled) << "the pending output and the peer's buffer never stopped moving";
    ASSERT_EQ(session.messages, (std::vector<std::string>{"data"}));
    ASSERT_GT(session.pendingWrite(), 0u) << "the socket buffers must be full for this test to prove anything";
    ASSERT_EQ(session.disconnected_events, 0u);

    ASSERT_TRUE(deliver(pair, "more")); // a read readiness, alone: the probe's socket is full
    for (int i = 0; i < 50; ++i)
        qb::io::async::run(EVRUN_NOWAIT);
    EXPECT_EQ(session.disconnected_events, 0u) << "a read event during the delivery must not end the connection";
    EXPECT_FALSE(session.base().is_reading()) << "the read readiness came alone: only that path stops the reads";

    // The peer drains: every byte arrives, then the connection ends -- on a clock too: every byte crosses
    // the loopback.
    pair.peer.set_nonblocking(true);
    std::vector<char> buffer(std::size_t{1} << 20);
    std::size_t       received   = 0;
    char              last_byte  = 0;
    const auto        drained_by = std::chrono::steady_clock::now() + 10 * qb::io::test::kLoopbackWaitBudget;
    while (received < kReply && std::chrono::steady_clock::now() < drained_by) {
        const bool moved = qb::io::async::run(EVRUN_NOWAIT) != 0;
        const auto n     = pair.peer.read(buffer.data(), buffer.size());
        if (n > 0) {
            received += static_cast<std::size_t>(n);
            last_byte = buffer[static_cast<std::size_t>(n) - 1];
        } else if (qb::io::socket::get_last_errno() == 0) {
            break; // the peer's EOF (tcp::socket::read() reports it as -1 with the error cleared): nothing more comes
        } else if (!moved) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }
    EXPECT_EQ(received, kReply) << "the whole reply must be delivered before the close";
    EXPECT_EQ(last_byte, '!');
    run_nowait_iterations();
    EXPECT_EQ(session.disconnected_events, 1u) << "and the connection ends once it is";
    EXPECT_EQ(session.pendingWrite(), 0u);
}

#ifndef NDEBUG
// The reuse contract is asserted in debug (Huly QB-202). Each statement below is a caller error a
// release build turns into a lost disconnection, in silence: a start() or a reset_io_state() under a
// pending disconnect() zeroes `_reason` before its deferred dispose() runs, and a start() from
// inside on(disconnected) is undone by the stop() dispose() runs right after it. The child is
// re-executed ("threadsafe"); the patterns are substrings of the assertions' own text.
TEST(AsyncIoBaseDeathTest, StartWhileADisconnectIsPendingIsRefused) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    qb::io::test::drain_log_writer_before_fork(); // a --gtest_repeat pass follows this binary's logging cases (Huly QB-1206)
    EXPECT_DEATH(
        {
            qb::io::async::init();
            auto            pair = make_stream_pair();
            PipeDuplexProbe session{pair.probe};
            session.base().start();
            session.base().disconnect();
            session.base().start();
        },
        "io::start.. while a disconnect");
}

TEST(AsyncIoBaseDeathTest, ResetIoStateWhileADisconnectIsPendingIsRefused) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    qb::io::test::drain_log_writer_before_fork(); // a --gtest_repeat pass follows this binary's logging cases (Huly QB-1206)
    EXPECT_DEATH(
        {
            qb::io::async::init();
            auto            pair = make_stream_pair();
            PipeDuplexProbe session{pair.probe};
            session.base().start();
            session.base().disconnect();
            session.reset_state();
        },
        "io::reset_io_state.. while a disconnect");
}

TEST(AsyncIoBaseDeathTest, StartFromInsideDisposeIsRefused) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    qb::io::test::drain_log_writer_before_fork(); // a --gtest_repeat pass follows this binary's logging cases (Huly QB-1206)
    EXPECT_DEATH(
        {
            qb::io::async::init();
            auto            pair = make_stream_pair();
            PipeDuplexProbe session{pair.probe};
            session.restart_on_disconnected = true;
            session.base().start();
            session.base().disconnect();
            run_nowait_iterations();
        },
        "io::start.. from inside dispose");
}
#endif

// =====================================================================================================
// clear_protocols() from inside a dispatch (Huly QB-1278). The read loop calls the protocol's onMessage();
// the protocol hands the frame to its io -- the session's handler -- and touches itself after the handler
// returned, as the WebSocket decoder writes its frame state. A handler that called clear_protocols() ended
// that protocol under its own onMessage(): it then wrote freed memory.
// =====================================================================================================
namespace {

/// What a DispatchingProtocol saw of its own lifetime. Lives outside it, so it can be read once it is gone.
struct ProtocolLifetime {
    bool        in_dispatch           = false; ///< the protocol's onMessage() is on the stack
    std::size_t destroyed             = 0u;    ///< its destructor ran
    std::size_t destroyed_in_dispatch = 0u;    ///< its destructor ran while its onMessage() was still running
    std::size_t frames_after_handler  = 0u;    ///< the protocol's own count, written after its handler returned
    bool        destructor_armed      = true;  ///< the destructor's callback may run (never into a component being destroyed)
};

/// Disarms a DispatchingProtocol's destructor callback when the test's scope ends, before the session it would call into.
struct DisarmOnExit {
    ProtocolLifetime &life;
    ~DisarmOnExit() {
        life.destructor_armed = false;
    }
};

/// Four-byte frames, each handed to `handler(io)`; the protocol writes its own state after the handler.
template <typename Io>
class DispatchingProtocol : public qb::io::async::AProtocol<Io> {
public:
    using handler_t = void (*)(Io &);

private:
    ProtocolLifetime &_life;
    handler_t         _handler;
    handler_t         _on_destroy;
    std::size_t       _frames = 0u;

public:
    DispatchingProtocol(Io &io, ProtocolLifetime &life, handler_t handler, handler_t on_destroy = nullptr) noexcept
        : qb::io::async::AProtocol<Io>(io)
        , _life(life)
        , _handler(handler)
        , _on_destroy(on_destroy) {}

    ~DispatchingProtocol() override {
        ++_life.destroyed;
        if (_life.in_dispatch)
            ++_life.destroyed_in_dispatch;
        if (_on_destroy && _life.destructor_armed)
            _on_destroy(this->_io); // a destructor that calls back into its io, as the read loop ends it
    }

    std::size_t
    getMessageSize() noexcept final {
        return this->_io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        auto &life       = _life;
        life.in_dispatch = true;
        this->_io.messages.emplace_back(this->_io.in().begin(), size);
        _handler(this->_io);
        ++_frames; // the protocol's own state, after its handler: freed memory if the handler ended it
        life.frames_after_handler = _frames;
        life.in_dispatch          = false;
    }

    void
    reset() noexcept final {
        _frames = 0u;
    }
};

template <typename Io>
void
clear_in_handler(Io &io) {
    io.base().clear_protocols();
}

template <typename Io>
void
disconnect_and_clear_in_handler(Io &io) {
    io.base().disconnect();
    io.base().clear_protocols();
}

void
clear_then_switch_in_handler(PipeDuplexProbe &io) {
    io.base().clear_protocols();
    (void) io.base().switch_protocol<FourByteDuplexProtocol>(io);
}

void
switch_in_handler(PipeDuplexProbe &io) {
    (void) io.base().switch_protocol<FourByteDuplexProtocol>(io);
}

template <typename Io>
void
no_handler(Io &) {}

ProtocolLifetime g_switched_in_life; // the protocol clear_then_switch_counted_in_handler() switches in

void
clear_then_switch_counted_in_handler(PipeDuplexProbe &io) {
    io.base().clear_protocols();
    (void) io.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(io, g_switched_in_life, &no_handler<PipeDuplexProbe>);
}

std::size_t g_disconnects_seen_by_the_destructor = ~std::size_t{0}; // what disconnect_now_in_destructor() saw once its call returned

void
disconnect_now_in_destructor(PipeDuplexProbe &io) {
    io.disconnect_now_for_test(77);
    g_disconnects_seen_by_the_destructor = io.disconnected_events;
}

} // namespace

TEST_F(AsyncIoBaseTest, DuplexClearProtocolsFromAHandlerEndsTheProtocolOnceItsDispatchReturned) {
    ProtocolLifetime life;
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &clear_in_handler<PipeDuplexProbe>),
                  nullptr);
        session.base().start();
        ASSERT_TRUE(deliver(pair, "abcd"));
        ASSERT_TRUE(pass_until([&] { return session.disconnected_events > 0u; }, "the session ends once it has no protocol"));

        EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd"}));
        EXPECT_EQ(life.frames_after_handler, 1u) << "the protocol finished its onMessage() after the handler cleared it";
        EXPECT_EQ(life.destroyed, 1u) << "the cleared protocol is destroyed once the dispatch has returned";
        EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
        EXPECT_EQ(session.base().protocol(), qb::io::async::no_protocol());
        EXPECT_EQ(session.last_disconnect_reason, -1) << "no protocol left: a protocol error, as before";
    }
    EXPECT_EQ(life.destroyed, 1u) << "destroyed once";
}

TEST_F(AsyncIoBaseTest, DuplexDisconnectThenClearFromAHandlerEndsTheProtocolOnceItsDispatchReturned) {
    ProtocolLifetime life;
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life,
                                                                                       &disconnect_and_clear_in_handler<PipeDuplexProbe>),
                  nullptr);
        session.base().start();
        ASSERT_TRUE(deliver(pair, "abcd"));
        ASSERT_TRUE(pass_until([&] { return session.disconnected_events > 0u; }, "the session ends on its handler's disconnect()"));

        EXPECT_EQ(life.frames_after_handler, 1u);
        EXPECT_EQ(life.destroyed, 1u);
        EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
        EXPECT_EQ(session.last_disconnect_reason, 1) << "the handler's disconnect() names the reason";
    }
    EXPECT_EQ(life.destroyed, 1u);
}

TEST_F(AsyncIoBaseTest, DuplexClearThenSwitchFromAHandlerKeepsTheNewProtocolAndEndsTheOld) {
    ProtocolLifetime life;
    auto             pair = make_stream_pair();
    PipeDuplexProbe  session{pair.probe};
    ASSERT_NE(session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &clear_then_switch_in_handler), nullptr);
    session.base().start();
    ASSERT_TRUE(deliver(pair, "abcdefgh")); // the second frame is the new protocol's, in this read or the next
    ASSERT_TRUE(pass_until([&] { return session.messages.size() >= 2u; }, "the new protocol reads the second frame"));
    ASSERT_TRUE(pass_until([&] { return life.destroyed > 0u; }, "the cleared protocol ends once its dispatch has returned"));
    ASSERT_TRUE(deliver(pair, "ijkl")); // a read after the pass that ended the old protocol: the new one is still there
    ASSERT_TRUE(pass_until([&] { return session.messages.size() >= 3u; }, "the new protocol reads the frame of a later read"));

    EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd", "efgh", "ijkl"}));
    EXPECT_EQ(life.frames_after_handler, 1u);
    EXPECT_EQ(life.destroyed, 1u) << "the cleared protocol is destroyed once its dispatch has returned";
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_NE(session.base().protocol(), qb::io::async::no_protocol()) << "the protocol switched in after the clear is kept";
    EXPECT_EQ(session.disconnected_events, 0u);
    EXPECT_TRUE(session.base().is_connected());
}

TEST_F(AsyncIoBaseTest, DuplexARetiredProtocolsDestructorMaySwitchTheNextProtocolIn) {
    // The read loop ends the retired protocol after its dispatch returned, while the protocol the handler switched
    // in is live beside it: the destructor's own switch_protocol() must find the list consistent, and is kept.
    ProtocolLifetime life;
    auto             pair = make_stream_pair();
    PipeDuplexProbe  session{pair.probe};
    DisarmOnExit     disarm{life}; // a failed ASSERT returns: the session must not run the callback while it dies
    ASSERT_NE(
        session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &clear_then_switch_in_handler, &switch_in_handler),
        nullptr);
    session.base().start();
    ASSERT_TRUE(deliver(pair, "abcd"));
    ASSERT_TRUE(pass_until([&] { return life.destroyed > 0u; }, "the cleared protocol ends once its dispatch has returned"));
    auto *const switched_in_by_the_destructor = session.base().protocol();
    ASSERT_TRUE(deliver(pair, "efgh"));
    ASSERT_TRUE(pass_until([&] { return session.messages.size() >= 2u; }, "the protocol the destructor switched in reads the next frame"));

    EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd", "efgh"}));
    EXPECT_EQ(life.destroyed, 1u);
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_NE(switched_in_by_the_destructor, qb::io::async::no_protocol());
    EXPECT_EQ(session.base().protocol(), switched_in_by_the_destructor) << "the destructor's switch_protocol() is the active one";
    EXPECT_EQ(session.disconnected_events, 0u);
    EXPECT_TRUE(session.base().is_connected());
}

TEST_F(AsyncIoBaseTest, DuplexARetiredProtocolsDestructorThatClearsEndsTheProtocolSwitchedInAfterTheClear) {
    // The retired protocol ends with the dispatch flag set: its clear_protocols() joins the clear under way and
    // ends the protocol the handler switched in, in the same pass.
    ProtocolLifetime life;
    g_switched_in_life   = {};
    auto            pair = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    DisarmOnExit    disarm{life}; // a failed ASSERT returns: the session must not run the callback while it dies
    ASSERT_NE(session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &clear_then_switch_counted_in_handler,
                                                                                   &clear_in_handler<PipeDuplexProbe>),
              nullptr);
    session.base().start();
    ASSERT_TRUE(deliver(pair, "abcd"));
    ASSERT_TRUE(pass_until([&] { return life.destroyed > 0u; }, "the cleared protocol ends once its dispatch has returned"));

    EXPECT_EQ(life.destroyed, 1u);
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_EQ(g_switched_in_life.destroyed, 1u) << "the destructor's clear ended the switched-in protocol in the same pass";
    EXPECT_EQ(g_switched_in_life.destroyed_in_dispatch, 0u);
    EXPECT_EQ(session.base().protocol(), qb::io::async::no_protocol());
}

TEST_F(AsyncIoBaseTest, DuplexARetiredProtocolsDestructorThatDisconnectsNowLeavesTheTeardownToTheEventItFed) {
    // The retired protocol ends under the io's dispatch flag, as it did when the clear destroyed it inside the
    // handler: its disconnect_now() feeds the event and leaves the teardown to it, never dispose() under the pass.
    ProtocolLifetime life;
    g_disconnects_seen_by_the_destructor = ~std::size_t{0};
    auto            pair                 = make_stream_pair();
    PipeDuplexProbe session{pair.probe};
    DisarmOnExit    disarm{life}; // a failed ASSERT returns: the session must not run the callback while it dies
    ASSERT_NE(session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &clear_in_handler<PipeDuplexProbe>,
                                                                                   &disconnect_now_in_destructor),
              nullptr);
    session.base().start();
    ASSERT_TRUE(deliver(pair, "abcd"));
    ASSERT_TRUE(pass_until([&] { return session.disconnected_events > 0u; }, "the session ends once its protocol is gone"));

    EXPECT_EQ(life.destroyed, 1u);
    EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
    EXPECT_EQ(g_disconnects_seen_by_the_destructor, 0u) << "disconnect_now() ran dispose() inside the destructor, under the read pass";
    EXPECT_EQ(session.disconnected_events, 1u);
    EXPECT_EQ(session.last_disconnect_reason, 77) << "the destructor's disconnect_now() is the reason the teardown reports";
}

TEST_F(AsyncIoBaseTest, DuplexADestructorRunByAClearOutsideADispatchMaySwitchTheNextProtocolIn) {
    // Outside a dispatch the clear ends its protocols at once, one at a time, the sentinel already active: the
    // destructor's switch_protocol() is the active protocol when the clear returns.
    ProtocolLifetime life;
    auto             pair = make_stream_pair();
    PipeDuplexProbe  session{pair.probe};
    DisarmOnExit     disarm{life}; // a failed ASSERT returns: the session must not run the callback while it dies
    ASSERT_NE(
        session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &no_handler<PipeDuplexProbe>, &switch_in_handler),
        nullptr);
    session.base().clear_protocols();

    EXPECT_EQ(life.destroyed, 1u) << "outside a dispatch, destroyed before clear_protocols() returns";
    EXPECT_NE(session.base().protocol(), qb::io::async::no_protocol()) << "the destructor's switch_protocol() is kept";
}

TEST_F(AsyncIoBaseTest, DuplexSwitchFromAHandlerKeepsTheOldProtocolUntilTheSessionEnds) {
    ProtocolLifetime life;
    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &switch_in_handler), nullptr);
        session.base().start();
        ASSERT_TRUE(deliver(pair, "abcdefgh"));
        ASSERT_TRUE(pass_until([&] { return session.messages.size() >= 2u; }, "the new protocol reads the second frame"));

        EXPECT_EQ(session.messages, (std::vector<std::string>{"abcd", "efgh"}));
        EXPECT_EQ(life.destroyed, 0u) << "switch_protocol() ends no protocol: the old one is owned until a clear or the end";
        EXPECT_TRUE(session.base().is_connected());
    }
    EXPECT_EQ(life.destroyed, 1u) << "the session's end destroys it";
    EXPECT_EQ(life.destroyed_in_dispatch, 0u);
}

TEST_F(AsyncIoBaseTest, DuplexClearProtocolsOutsideADispatchEndsTheProtocolsAtOnce) {
    ProtocolLifetime life;
    auto             pair = make_stream_pair();
    PipeDuplexProbe  session{pair.probe};
    ASSERT_NE(session.base().switch_protocol<DispatchingProtocol<PipeDuplexProbe>>(session, life, &no_handler<PipeDuplexProbe>), nullptr);
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
    session.base().clear_protocols();
    EXPECT_EQ(life.destroyed, 1u) << "outside a dispatch, clear_protocols() destroys what it ends before it returns";
    EXPECT_EQ(session.base().protocol(), qb::io::async::no_protocol());
}

TEST_F(AsyncIoBaseTest, InputClearProtocolsFromAHandlerEndsTheProtocolOnceItsDispatchReturned) {
    ProtocolLifetime life;
    {
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<DispatchingProtocol<PipeInputProbe>>(input, life, &clear_in_handler<PipeInputProbe>), nullptr);
        input.base().start();
        ASSERT_TRUE(deliver(pair, "abcd"));
        ASSERT_TRUE(pass_until([&] { return input.disconnected_events > 0u; }, "the input ends once it has no protocol"));

        EXPECT_EQ(input.messages, (std::vector<std::string>{"abcd"}));
        EXPECT_EQ(life.frames_after_handler, 1u);
        EXPECT_EQ(life.destroyed, 1u);
        EXPECT_EQ(life.destroyed_in_dispatch, 0u) << "destroyed under its own onMessage()";
        EXPECT_EQ(input.base().protocol(), qb::io::async::no_protocol());
    }
    EXPECT_EQ(life.destroyed, 1u);
}

TEST_F(AsyncIoBaseTest, InputClearProtocolsOutsideADispatchEndsTheProtocolsAtOnce) {
    ProtocolLifetime life;
    auto             pair = make_stream_pair();
    PipeInputProbe   input{pair.probe};
    ASSERT_NE(input.base().switch_protocol<DispatchingProtocol<PipeInputProbe>>(input, life, &no_handler<PipeInputProbe>), nullptr);
    input.base().clear_protocols();
    EXPECT_EQ(life.destroyed, 1u) << "outside a dispatch, clear_protocols() destroys what it ends before it returns";
}
