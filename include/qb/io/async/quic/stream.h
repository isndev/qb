/**
 * @file qb/io/async/quic/stream.h
 * @brief QUIC stream facade for qb async sessions.
 */

#ifndef QB_IO_ASYNC_QUIC_STREAM_H_
#define QB_IO_ASYNC_QUIC_STREAM_H_

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <qb/system/allocator/pipe.h>
#include <qb/utility/type_traits.h>
#include "../buffered_io.h"
#include "../event/io.h"
#include "../protocol.h"
#include "../../quic/types.h"

namespace qb::io::async::quic {

class stream {
    std::uint64_t _connection_id = 0;
    std::uint64_t _id = 0;
    qb::io::quic::stream_direction _direction = qb::io::quic::stream_direction::bidirectional;
    qb::io::quic::stream_origin _origin = qb::io::quic::stream_origin::local;
    bool _open = false;

public:
    stream() = default;

    stream(std::uint64_t id, qb::io::quic::stream_direction direction,
           qb::io::quic::stream_origin origin) noexcept
        : stream(0, id, direction, origin) {}

    stream(std::uint64_t connection_id, std::uint64_t id,
           qb::io::quic::stream_direction direction,
           qb::io::quic::stream_origin origin) noexcept
        : _connection_id(connection_id)
        , _id(id)
        , _direction(direction)
        , _origin(origin)
        , _open(true) {}

    [[nodiscard]] std::uint64_t connection_id() const noexcept { return _connection_id; }
    [[nodiscard]] std::uint64_t id() const noexcept { return _id; }
    [[nodiscard]] bool is_open() const noexcept { return _open; }
    [[nodiscard]] qb::io::quic::stream_direction direction() const noexcept { return _direction; }
    [[nodiscard]] qb::io::quic::stream_origin origin() const noexcept { return _origin; }

    void reset(std::uint64_t /*application_error_code*/ = 0) {
        close();
    }

    void close() noexcept {
        _open = false;
    }
};

template <typename Derived, typename Server = void>
class client;

namespace detail {

template <typename Derived>
class session_base : public qb::io::async::buffered_io<Derived> {
    using base_t = qb::io::async::buffered_io<Derived>;
    qb::allocator::pipe<char> _in;
    qb::allocator::pipe<char> _out;
    std::uint64_t _connection_id = 0;
    std::uint64_t _stream_id = 0;
    std::size_t _max_read_buffer_size = QB_MAX_READ_BUFFER_SIZE;
    std::size_t _max_write_buffer_size = QB_MAX_WRITE_BUFFER_SIZE;
    bool _open = false;

public:
    using base_io_t = base_t;
    using base_t::operator<<;
    using base_t::publish;

    session_base() {
        attach_protocol_if_declared();
    }

    explicit session_base(std::uint64_t stream_id)
        : _stream_id(stream_id)
        , _open(true) {
        attach_protocol_if_declared();
    }

    session_base(std::uint64_t connection_id, std::uint64_t stream_id)
        : _connection_id(connection_id)
        , _stream_id(stream_id)
        , _open(true) {
        attach_protocol_if_declared();
    }

    session_base(session_base const&) = delete;
    session_base& operator=(session_base const&) = delete;
    session_base(session_base&&) = delete;
    session_base& operator=(session_base&&) = delete;
    ~session_base() noexcept = default;

    [[nodiscard]] std::uint64_t id() const noexcept { return _stream_id; }
    [[nodiscard]] std::uint64_t connection_id() const noexcept { return _connection_id; }
    [[nodiscard]] bool is_open() const noexcept { return _open; }
    [[nodiscard]] qb::allocator::pipe<char>& in() noexcept { return _in; }
    [[nodiscard]] qb::allocator::pipe<char>& out() noexcept { return _out; }
    [[nodiscard]] std::size_t pendingRead() const noexcept { return _in.size(); }
    [[nodiscard]] std::size_t pendingWrite() const noexcept { return _out.size(); }
    [[nodiscard]] std::size_t max_read_buffer_size() const noexcept { return _max_read_buffer_size; }
    [[nodiscard]] std::size_t max_write_buffer_size() const noexcept { return _max_write_buffer_size; }

    void set_max_read_buffer_size(std::size_t value) noexcept {
        _max_read_buffer_size = value;
    }

    void set_max_write_buffer_size(std::size_t value) noexcept {
        _max_write_buffer_size = value;
    }

    void assign_stream_id(std::uint64_t stream_id) noexcept {
        _stream_id = stream_id;
        _open = true;
    }

    void assign_connection_id(std::uint64_t connection_id) noexcept {
        _connection_id = connection_id;
    }

    void assign_quic_ids(std::uint64_t connection_id, std::uint64_t stream_id) noexcept {
        _connection_id = connection_id;
        _stream_id = stream_id;
        _open = true;
    }

    bool append(std::string_view data) {
        if (data.empty())
            return true;
        const auto max_read = max_read_buffer_size();
        if (max_read != static_cast<std::size_t>(-1) &&
            pendingRead() + data.size() > max_read) {
            this->disconnect(qb::io::async::event::disconnect_reason::buffer_overflow);
            return false;
        }
        std::memcpy(_in.allocate_back(data.size()), data.data(), data.size());
        this->account_read(data.size());
        return true;
    }

    bool append(std::span<const std::byte> data) {
        return append(std::string_view{
            reinterpret_cast<const char *>(data.data()),
            data.size()
        });
    }

    char *publish(char const *data, std::size_t size) {
        if (!_open)
            return nullptr;
        const auto max_write = max_write_buffer_size();
        if (max_write < pendingWrite() || size > max_write - pendingWrite()) {
            this->disconnect(qb::io::async::event::disconnect_reason::buffer_overflow);
            return nullptr;
        }
        return static_cast<char *>(std::memcpy(_out.allocate_back(size), data, size));
    }

    bool process() noexcept {
        return base_t::process_input();
    }

    void flush(std::size_t size) noexcept {
        _in.free_front(size);
    }

    void close() noexcept {
        _open = false;
        _in.reset();
        _out.reset();
    }

private:
    void attach_protocol_if_declared() {
        if constexpr (qb::has_type_Protocol<Derived>) {
            if constexpr (!std::is_void_v<typename Derived::Protocol>) {
                this->template switch_protocol<typename Derived::Protocol>(
                    static_cast<Derived&>(*this));
            }
        }
    }
};

} // namespace detail

template <typename Derived, typename Server>
class client : public detail::session_base<Derived> {
    using base_t = detail::session_base<Derived>;
    Server *_server = nullptr;

public:
    using base_io_t = base_t;
    using IOServer = Server;
    constexpr static const bool has_server = true;

    client() = delete;

    explicit client(Server& server)
        : _server(&server) {}

    client(Server& server, std::uint64_t stream_id)
        : base_t(stream_id)
        , _server(&server) {}

    client(client const&) = delete;
    client& operator=(client const&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;
    ~client() noexcept = default;

    [[nodiscard]] Server& server() noexcept { return *_server; }
    [[nodiscard]] Server const& server() const noexcept { return *_server; }
};

template <typename Derived>
class client<Derived, void> : public detail::session_base<Derived> {
    using base_t = detail::session_base<Derived>;

public:
    using base_io_t = base_t;
    using base_t::base_t;
    constexpr static const bool has_server = false;

    client() = default;
    client(client const&) = delete;
    client& operator=(client const&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;
    ~client() noexcept = default;
};

template <typename Derived>
using stream_session = client<Derived, void>;

} // namespace qb::io::async::quic

#endif // QB_IO_ASYNC_QUIC_STREAM_H_
