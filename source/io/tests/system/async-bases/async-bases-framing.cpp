/**
 * @file qb/source/io/tests/system/test-async-io-bases.cpp
 * @brief Focused tests for async::input and async::output CRTP bases.
 *
 * These tests use local POSIX pipes to drive libev read/write readiness without
 * relying on TCP timing. They exercise the protocol processing and output drain
 * paths that concrete transports inherit from qb-io async bases.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 *
 * @ingroup Tests
 */

#include <gtest/gtest.h>

#include <qb/io/async/io.h>
#include <qb/system/allocator/pipe.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <tuple>
#include <utility>
#include <unistd.h>
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

class UniqueFd {
    int _fd = -1;

public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept
        : _fd(fd) {}
    UniqueFd(UniqueFd const &)            = delete;
    UniqueFd &operator=(UniqueFd const &) = delete;

    UniqueFd(UniqueFd &&other) noexcept
        : _fd(std::exchange(other._fd, -1)) {}

    UniqueFd &
    operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
            reset();
            _fd = std::exchange(other._fd, -1);
        }
        return *this;
    }

    ~UniqueFd() {
        reset();
    }

    [[nodiscard]] int
    get() const noexcept {
        return _fd;
    }
    [[nodiscard]] bool
    valid() const noexcept {
        return _fd >= 0;
    }

    void
    reset(int fd = -1) noexcept {
        if (_fd >= 0)
            ::close(_fd);
        _fd = fd;
    }
};

struct PipePair {
    UniqueFd read;
    UniqueFd write;
};

PipePair
make_pipe_pair() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0)
        throw std::runtime_error(std::strerror(errno));
    return PipePair{UniqueFd{fds[0]}, UniqueFd{fds[1]}};
}

PipePair
make_socket_pair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        throw std::runtime_error(std::strerror(errno));
    return PipePair{UniqueFd{fds[0]}, UniqueFd{fds[1]}};
}

class FdTransport {
    int _fd = -1;

public:
    FdTransport() = default;
    explicit FdTransport(int fd) noexcept
        : _fd(fd) {}

    [[nodiscard]] int
    native_handle() const noexcept {
        return _fd;
    }

    void
    set_nonblocking(bool enabled) const noexcept {
        if (_fd < 0)
            return;
        const int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags < 0)
            return;
        if (enabled)
            std::ignore = ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
        else
            std::ignore = ::fcntl(_fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    void
    close() noexcept {
        _fd = -1;
    }
};

class PipeInputProbe : public qb::io::async::input<PipeInputProbe> {
    FdTransport               _transport;
    qb::allocator::pipe<char> _in;

public:
    using base_io_t = qb::io::async::input<PipeInputProbe>;

    std::vector<std::string> messages;
    std::size_t              pending_read_events    = 0u;
    std::size_t              eof_events             = 0u;
    std::size_t              disconnected_events    = 0u;
    std::size_t              dispose_events         = 0u;
    int                      last_disconnect_reason = 0;

    explicit PipeInputProbe(int fd) noexcept
        : _transport(fd) {}

    base_io_t &
    base() noexcept {
        return *this;
    }
    base_io_t const &
    base() const noexcept {
        return *this;
    }
    FdTransport &
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
        const auto            ret    = ::read(_transport.native_handle(), dst, kChunk);
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

    FdTransport               _transport;
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

    explicit PipeOutputProbe(int fd) noexcept
        : _transport(fd) {}

    base_io_t &
    base() noexcept {
        return *this;
    }
    base_io_t const &
    base() const noexcept {
        return *this;
    }
    FdTransport &
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
            errno = EWOULDBLOCK;
            return -1;
        }
        if (_write_mode == write_mode::hard_error) {
            errno = EPIPE;
            return -1;
        }

        const auto count = std::min(_max_chunk, _out.size());
        const auto ret   = ::write(_transport.native_handle(), _out.begin(), count);
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

    FdTransport               _transport;
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

    explicit PipeDuplexProbe(int fd) noexcept
        : _transport(fd) {}

    base_io_t &
    base() noexcept {
        return *this;
    }
    base_io_t const &
    base() const noexcept {
        return *this;
    }
    FdTransport &
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
        const auto            ret    = ::read(_transport.native_handle(), dst, kChunk);
        if (ret >= 0)
            _in.free_back(kChunk - static_cast<std::size_t>(ret));
        else
            _in.free_back(kChunk);
        return static_cast<int>(ret);
    }

    int
    write() noexcept {
        if (_write_mode == write_mode::would_block) {
            errno = EWOULDBLOCK;
            return -1;
        }
        if (_write_mode == write_mode::hard_error) {
            errno = EPIPE;
            return -1;
        }

        const auto count = std::min(_max_chunk, _out.size());
        const auto ret   = ::write(_transport.native_handle(), _out.begin(), count);
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
    auto           pipes = make_pipe_pair();
    PipeInputProbe input{pipes.read.get()};
    ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);

    input.base().start();
    ASSERT_EQ(::write(pipes.write.get(), "abcdefghZ", 9), 9);

    run_nowait_iterations();

    EXPECT_EQ(input.messages, (std::vector<std::string>{"abcd", "efgh"}));
    EXPECT_EQ(input.pendingRead(), 1u);
    EXPECT_EQ(input.pending_read_events, 1u);
    EXPECT_EQ(input.eof_events, 0u);
    EXPECT_EQ(input.base().bytes_read(), 9u);
    EXPECT_EQ(input.base().messages_processed(), 2u);
    EXPECT_TRUE(input.base().has_pending_data());

    input.flush(input.pendingRead());
    ASSERT_EQ(::write(pipes.write.get(), "wxyz", 4), 4);
    run_nowait_iterations();

    EXPECT_EQ(input.messages.back(), "wxyz");
    EXPECT_EQ(input.pendingRead(), 0u);
    EXPECT_EQ(input.eof_events, 1u);
}

TEST_F(AsyncIoBaseTest, InputDisconnectsOnProtocolErrorAndOversizedFrame) {
    {
        auto           pipes = make_pipe_pair();
        PipeInputProbe input{pipes.read.get()};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input, true), nullptr);

        input.base().start();
        ASSERT_EQ(::write(pipes.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(input.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.dispose_events, 1u);
        EXPECT_EQ(input.last_disconnect_reason, -1);
        EXPECT_FALSE(input.base().is_connected());
    }

    {
        auto           pipes = make_pipe_pair();
        PipeInputProbe input{pipes.read.get()};
        ASSERT_NE(input.base().switch_protocol<OversizedInputProtocol>(input), nullptr);
        input.base().set_max_message_size(4u);

        input.base().start();
        ASSERT_EQ(::write(pipes.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.last_disconnect_reason, -2);
    }
}

TEST_F(AsyncIoBaseTest, InputDisposesWhenProtocolIsInvalidOrClearedBeforeRead) {
    {
        auto           pipes = make_pipe_pair();
        PipeInputProbe input{pipes.read.get()};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);
        ASSERT_NE(input.base().protocol(), qb::io::async::no_protocol()); // a real protocol is set
        input.base().protocol()->not_ok();

        input.base().start();
        ASSERT_EQ(::write(pipes.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.dispose_events, 1u);
        EXPECT_EQ(input.last_disconnect_reason, -1);
        EXPECT_FALSE(input.base().is_connected());
    }

    {
        auto           pipes = make_pipe_pair();
        PipeInputProbe input{pipes.read.get()};
        ASSERT_NE(input.base().switch_protocol<FourByteInputProtocol>(input), nullptr);
        input.base().clear_protocols();
        EXPECT_EQ(input.base().protocol(), qb::io::async::no_protocol());

        input.base().start();
        ASSERT_EQ(::write(pipes.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(input.disconnected_events, 1u);
        EXPECT_EQ(input.dispose_events, 1u);
        EXPECT_FALSE(input.base().is_connected());
    }

    {
        auto           pipes = make_pipe_pair();
        PipeInputProbe input{pipes.read.get()};
        EXPECT_EQ(input.base().switch_protocol<RejectingInputProtocol>(input), nullptr);
        EXPECT_EQ(input.base().protocol(), qb::io::async::no_protocol());
    }
}

TEST_F(AsyncIoBaseTest, OutputDrainsPartialWritesAndPublishesEos) {
    auto            pipes = make_pipe_pair();
    PipeOutputProbe output{pipes.write.get()};
    output.set_max_chunk(3u);

    output.base().start();
    output.base().publish(std::string_view{"abcdef"});

    run_nowait_iterations();
    EXPECT_EQ(output.pending_write_events, 1u);
    ASSERT_EQ(output.eos_events, 1u);
    EXPECT_EQ(output.base().bytes_written(), 6u);
    ASSERT_FALSE(output.base().has_pending_data());

    std::array<char, 8> buffer{};
    const auto          read = ::read(pipes.read.get(), buffer.data(), buffer.size());
    ASSERT_EQ(read, 6);
    EXPECT_EQ(std::string_view(buffer.data(), 6), "abcdef");
}

TEST_F(AsyncIoBaseTest, OutputDisconnectIsIdempotentAndReportsReason) {
    auto            pipes = make_pipe_pair();
    PipeOutputProbe output{pipes.write.get()};

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
    auto            pipes = make_pipe_pair();
    PipeOutputProbe output{pipes.write.get()};
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
        auto            pipes = make_pipe_pair();
        PipeOutputProbe output{pipes.write.get()};
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
        auto            pipes = make_pipe_pair();
        PipeOutputProbe output{pipes.write.get()};
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
    auto            sockets = make_socket_pair();
    PipeDuplexProbe session{sockets.read.get()};
    session.set_max_chunk(2u);
    ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, false, true), nullptr);

    session.base().start();
    ASSERT_EQ(::write(sockets.write.get(), "abcdZ", 5), 5);

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
    const auto          read = ::read(sockets.write.get(), buffer.data(), buffer.size());
    ASSERT_EQ(read, 4);
    EXPECT_EQ(std::string_view(buffer.data(), 4), "pong");
}

TEST_F(AsyncIoBaseTest, DuplexProtocolLifecycleAccessorsAndNoProtocolDisposal) {
    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
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
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_FALSE(session.base().is_connected());
    }
}

TEST_F(AsyncIoBaseTest, DuplexClearedProtocolAndCloseAfterDeliverDisposeCleanly) {
    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session), nullptr);
        session.base().clear_protocols();

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages.size(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_FALSE(session.base().is_connected());
    }

    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, true, true), nullptr);

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.pendingWrite(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(session.base().bytes_written(), 4u);
        EXPECT_FALSE(session.base().is_connected());

        std::array<char, 8> buffer{};
        const auto          read = ::read(sockets.write.get(), buffer.data(), buffer.size());
        ASSERT_EQ(read, 4);
        EXPECT_EQ(std::string_view(buffer.data(), 4), "pong");

        session.reset_state();
        EXPECT_TRUE(session.base().is_connected());
    }
}

TEST_F(AsyncIoBaseTest, DuplexWriteOverflowRollsBackAndBlocksFurtherPublish) {
    auto            sockets = make_socket_pair();
    PipeDuplexProbe session{sockets.read.get()};
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
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, true), nullptr);

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.dispose_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -1);
    }

    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
        ASSERT_EQ(session.base().switch_protocol<RejectingDuplexProtocol>(session), nullptr);
        ASSERT_NE(session.base().switch_protocol<FourByteDuplexProtocol>(session, false, false), nullptr);
        session.force_read_overflow();

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages.size(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -3);
    }
}

TEST_F(AsyncIoBaseTest, DuplexProtocolEdgeCasesPreserveFlushAndDeferredDrainSemantics) {
    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
        ASSERT_NE(session.base().switch_protocol<OversizedDuplexProtocol>(session, 8u), nullptr);
        session.base().set_max_message_size(4u);

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_TRUE(session.messages.empty());
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, -2);
        EXPECT_FALSE(session.base().is_connected());
    }

    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
        ASSERT_NE(session.base().switch_protocol<DisconnectingDuplexProtocol>(session), nullptr);

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 0u);
        EXPECT_EQ(session.pendingWrite(), 0u);
        EXPECT_EQ(session.disconnected_events, 1u);
        EXPECT_EQ(session.last_disconnect_reason, 77);

        std::array<char, 8> buffer{};
        const auto          read = ::read(sockets.write.get(), buffer.data(), buffer.size());
        ASSERT_EQ(read, 4);
        EXPECT_EQ(std::string_view(buffer.data(), 4), "bye!");
    }

    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
        ASSERT_NE(session.base().switch_protocol<NonFlushingDuplexProtocol>(session), nullptr);

        session.base().start();
        ASSERT_EQ(::write(sockets.write.get(), "data", 4), 4);
        run_nowait_iterations();

        EXPECT_EQ(session.messages, (std::vector<std::string>{"data"}));
        EXPECT_EQ(session.pendingRead(), 4u);
        EXPECT_EQ(session.pending_read_events, 1u);
        EXPECT_TRUE(session.base().has_pending_read());
    }
}

TEST_F(AsyncIoBaseTest, DuplexWriteErrorsDistinguishWouldBlockFromHardFailure) {
    {
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
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
        auto            sockets = make_socket_pair();
        PipeDuplexProbe session{sockets.read.get()};
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
