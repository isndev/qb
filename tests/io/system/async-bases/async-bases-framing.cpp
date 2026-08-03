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
#include <qb/io/system/sys__socket.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/system/allocator/pipe.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Non-owning view of one StreamPair end, exposing exactly the transport surface the async
// CRTP bases require. I/O routes through qb::io::tcp::socket::read()/write() (the production
// transport methods → identical cross-platform semantics). close() only DETACHES: the
// StreamPair still owns and closes the socket, matching the original FdTransport whose fd
// was owned by PipePair. native_handle() keeps returning the captured handle after detach —
// the base never touches it post-close, and the socket stays open until the pair dies.
class SocketTransport {
    qb::io::tcp::socket *_sock   = nullptr;
    socket_handle_t      _handle = static_cast<socket_handle_t>(-1);

public:
    SocketTransport() = default;
    explicit SocketTransport(qb::io::tcp::socket &sock) noexcept
        : _sock(&sock)
        , _handle(sock.native_handle()) {}

    [[nodiscard]] socket_handle_t
    native_handle() const noexcept {
        return _handle;
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
        _sock = nullptr;
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

    void
    on(qb::io::async::event::disconnected &&event) noexcept {
        ++disconnected_events;
        last_disconnect_reason = event.reason;
    }

    void
    on(qb::io::async::event::dispose &&) noexcept {
        ++dispose_events;
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

    void
    on(qb::io::async::event::disconnected &&event) noexcept {
        ++disconnected_events;
        last_disconnect_reason = event.reason;
    }

    void
    on(qb::io::async::event::dispose &&) noexcept {
        ++dispose_events;
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

    void
    on(qb::io::async::event::eos &&) noexcept {
        ++eos_events;
    }

    void
    on(qb::io::async::event::disconnected &&event) noexcept {
        ++disconnected_events;
        last_disconnect_reason = event.reason;
    }

    void
    on(qb::io::async::event::dispose &&) noexcept {
        ++dispose_events;
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
public:
    explicit DisconnectingDuplexProtocol(PipeDuplexProbe &io) noexcept
        : AProtocol(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return _io.pendingRead() >= 4u ? 4u : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        _io.messages.emplace_back(_io.in().begin(), size);
        _io.publish(std::string_view{"bye!"});
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

void
run_nowait_iterations(int count = 16) {
    for (int i = 0; i < count; ++i)
        qb::io::async::run(EVRUN_NOWAIT);
}

} // namespace

TEST_F(AsyncIoBaseTest, InputReadsFramesAndReportsPendingThenEof) {
    auto           pair = make_stream_pair();
    PipeInputProbe input{pair.probe};
    ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);

    input.base().start();
    ASSERT_EQ(pair.peer.write("abcdefghZ", 9), 9);

    run_nowait_iterations();

    EXPECT_EQ(input.messages, (std::vector<std::string>{"abcd", "efgh"}));
    EXPECT_EQ(input.pendingRead(), 1u);
    EXPECT_EQ(input.pending_read_events, 1u);
    EXPECT_EQ(input.eof_events, 0u);
    EXPECT_EQ(input.base().bytes_read(), 9u);
    EXPECT_EQ(input.base().messages_processed(), 2u);
    EXPECT_TRUE(input.base().has_pending_data());

    input.flush(input.pendingRead());
    ASSERT_EQ(pair.peer.write("wxyz", 4), 4);
    run_nowait_iterations();

    EXPECT_EQ(input.messages.back(), "wxyz");
    EXPECT_EQ(input.pendingRead(), 0u);
    EXPECT_EQ(input.eof_events, 1u);
}

TEST_F(AsyncIoBaseTest, InputDisconnectsOnProtocolErrorAndOversizedFrame) {
    {
        auto           pair = make_stream_pair();
        PipeInputProbe input{pair.probe};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input, true), nullptr);

        input.base().start();
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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

    std::array<char, 8> buffer{};
    const auto          read = pair.peer.read(buffer.data(), buffer.size());
    ASSERT_EQ(read, 6);
    EXPECT_EQ(std::string_view(buffer.data(), 6), "abcdef");
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
    ASSERT_EQ(pair.peer.write("abcdZ", 5), 5);

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

    std::array<char, 8> buffer{};
    const auto          read = pair.peer.read(buffer.data(), buffer.size());
    ASSERT_EQ(read, 4);
    EXPECT_EQ(std::string_view(buffer.data(), 4), "pong");
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.pendingWrite(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(session.base().bytes_written(), 4u);
        EXPECT_FALSE(session.base().is_connected());

        std::array<char, 8> buffer{};
        const auto          read = pair.peer.read(buffer.data(), buffer.size());
        ASSERT_EQ(read, 4);
        EXPECT_EQ(std::string_view(buffer.data(), 4), "pong");

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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
        ASSERT_EQ(pair.peer.write("data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.pendingWrite(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, 77);

        std::array<char, 8> buffer{};
        const auto          read = pair.peer.read(buffer.data(), buffer.size());
        ASSERT_EQ(read, 4);
        EXPECT_EQ(std::string_view(buffer.data(), 4), "bye!");
    }

    {
        auto            pair = make_stream_pair();
        PipeDuplexProbe session{pair.probe};
        ASSERT_NE(session.base().switch_protocol<NonFlushingDuplexProtocol>(session), nullptr);

        session.base().start();
        ASSERT_EQ(pair.peer.write("data", 4), 4);
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
