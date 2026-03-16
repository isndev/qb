/**
 * @file qb/io/async/coroutine/protocol_reader.h
 * @brief Protocol-driven reader for coroutine client (same protocol API as input<>)
 *
 * Bridges existing AProtocol<> (e.g. qb::protocol::text::line) to the coroutine
 * client stream: reads from transport into a buffer, runs process_messages(),
 * and delivers parsed messages via a callback (e.g. into client.stream()).
 *
 * API aligned with existing input<>: in(), read(), flush(), start(), protocol().
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_PROTOCOL_READER_H
#define QB_IO_ASYNC_COROUTINE_PROTOCOL_READER_H

#include <qb/io/config.h>
#include <qb/io/async/io.h>
#include <qb/io/async/protocol.h>
#include <qb/system/allocator/pipe.h>
#include <functional>
#include <vector>

namespace qb::io::async {

/**
 * @brief Type-erased base for protocol readers (used by coro_client)
 * @tparam _TransportIO Transport I/O type (e.g. qb::io::tcp::socket)
 */
template <typename _TransportIO>
struct coro_protocol_reader_base {
    virtual ~coro_protocol_reader_base() = default;
    virtual void start(_TransportIO&) noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual IProtocol* protocol() noexcept = 0;
};

/**
 * @brief Protocol reader for coroutine client (I/O facade for AProtocol<>)
 *
 * Implements the _IO_ interface expected by AProtocol: in(), read(), flush(),
 * and on(Protocol::message). Uses the same process_messages() loop as input<>.
 * Delivers parsed messages as std::vector<char> via callback (client builds
 * coro_message from that).
 *
 * Supported protocols: only those whose message type has contiguous raw bytes
 * (e.g. .data and .size). This includes qb::protocol::text::command, text::string,
 * protocol::base::size_as_header, etc. It does *not* include protocols whose
 * message is a parsed object (e.g. qb::protocol::redis, whose message is
 * redisReply*); those remain callback-only unless a dedicated integration is added.
 *
 * @tparam _Transport Transport type (e.g. qb::io::transport::tcp)
 * @tparam _ProtocolTemplate Protocol template taking one IO type (e.g. qb::protocol::text::command)
 */
template <typename _Transport, template <typename> class _ProtocolTemplate>
class coro_client_protocol_reader
    : public base<coro_client_protocol_reader<_Transport, _ProtocolTemplate>, event::io>
    , public coro_protocol_reader_base<typename _Transport::transport_io_type> {
public:
    using transport_io_type = typename _Transport::transport_io_type;
    using protocol_type = _ProtocolTemplate<coro_client_protocol_reader<_Transport, _ProtocolTemplate>>;
    using deliver_fn = std::function<void(std::vector<char>&&)>;
    /** @brief Used by AProtocol<> friend declaration (same as reader type) */
    using base_io_t = coro_client_protocol_reader<_Transport, _ProtocolTemplate>;

private:
    using reader_t = coro_client_protocol_reader<_Transport, _ProtocolTemplate>;
    transport_io_type* transport_ = nullptr;
    qb::allocator::pipe<char> in_buffer_;
    deliver_fn deliver_;
    protocol_type protocol_;
    bool on_message_ = false;
    std::size_t max_message_size_ = QB_MAX_MESSAGE_SIZE;
    std::size_t max_read_buffer_size_ = static_cast<std::size_t>(-1);

public:
    /**
     * @brief Construct reader with transport and deliver callback
     * @param transport Reference to the connected transport (must stay valid)
     * @param deliver Callback invoked with message bytes (e.g. push to client stream)
     */
    coro_client_protocol_reader(transport_io_type& transport, deliver_fn deliver)
        : base<reader_t, event::io>()
        , transport_(&transport)
        , deliver_(std::move(deliver))
        , protocol_(*static_cast<reader_t*>(this)) {}

    coro_client_protocol_reader(const coro_client_protocol_reader&) = delete;
    coro_client_protocol_reader& operator=(const coro_client_protocol_reader&) = delete;

    /** @brief Get input buffer (for protocol getMessageSize / onMessage) */
    qb::allocator::pipe<char>& in() noexcept { return in_buffer_; }

    /** @brief Get input buffer (const) */
    const qb::allocator::pipe<char>& in() const noexcept { return in_buffer_; }

    /** @brief Read from transport into buffer; returns bytes read or negative error */
    int read() noexcept {
        if (!transport_ || !transport_->is_open()) {
            return -1;
        }
        constexpr std::size_t bucket_read = QB_DEFAULT_READ_BUFFER_SIZE;
        if (in_buffer_.size() + bucket_read > max_read_buffer_size_) {
            return -2;  // DoS buffer limit
        }
        std::size_t read_size = (bucket_read > QB_MAX_IO_SIZE) ? QB_MAX_IO_SIZE : bucket_read;
        char* ptr = in_buffer_.allocate_back(read_size);
        int ret = transport_->read(ptr, read_size);
        if (ret >= 0) {
            in_buffer_.free_back(read_size - static_cast<std::size_t>(ret));
        } else {
            in_buffer_.free_back(read_size);
        }
        return ret;
    }

    /** @brief Consume size bytes from front of input buffer */
    void flush(std::size_t size) noexcept {
        in_buffer_.free_front(size);
    }

    /** @brief Bytes available in input buffer */
    std::size_t pendingRead() const noexcept {
        return in_buffer_.size();
    }

    /** @brief Transport reference (for start() fd and protocol if needed) */
    transport_io_type& transport() noexcept {
        return *transport_;
    }

    /** @brief Protocol dispatch: copy message bytes and invoke deliver callback */
    void on(typename protocol_type::message&& msg) noexcept {
        if (deliver_ && msg.data && msg.size) {
            deliver_(std::vector<char>(msg.data, msg.data + msg.size));
        }
    }

    /** @brief Current protocol (IProtocol*) for API coherence with input<> */
    IProtocol* protocol() noexcept override {
        return &protocol_;
    }

    /** @brief Start reading: set non-blocking and register EV_READ (type-erased API) */
    void start(transport_io_type&) noexcept override {
        if (!transport_ || !transport_->is_open()) {
            return;
        }
        transport_->set_nonblocking(true);
        this->_async_event.start(transport_->native_handle(), EV_READ);
    }

    /** @brief Stop reading (stops the io watcher) */
    void stop() noexcept override {
        this->_async_event.stop();
    }

    /** @brief EOF: reorder or reset buffer (same as istream) */
    void eof() noexcept {
        if (!in_buffer_.size()) {
            in_buffer_.reset();
        } else {
            in_buffer_.reorder();
        }
    }

    /** @brief Handle EV_READ: read then process_messages (same as input<>) */
    void on(event::io const& event) {
        constexpr std::size_t invalid_ret = static_cast<std::size_t>(-1);
        if (on_message_) {
            return;
        }
        if (!protocol_.ok()) {
            return;
        }
        if ((event._revents & EV_READ) == 0) {
            return;
        }
        auto ret = static_cast<std::size_t>(read());
        if (ret == invalid_ret) {
            stop();
            return;
        }
        if (ret == static_cast<std::size_t>(-2)) {
            protocol_.not_ok();
            stop();
            return;
        }
        if (!process_messages()) {
            stop();
            return;
        }
        eof();
    }

private:
    bool process_messages() {
        std::size_t ret = 0u;
        on_message_ = true;
        while ((ret = protocol_.getMessageSize()) > 0) {
            if (ret > max_message_size_) {
                protocol_.not_ok();
                on_message_ = false;
                return false;
            }
            auto* prot = &protocol_;
            prot->onMessage(ret);
            if (!protocol_.ok()) {
                on_message_ = false;
                return false;
            }
            if (prot->should_flush()) {
                flush(ret);
            }
        }
        on_message_ = false;
        return true;
    }
};

}  // namespace qb::io::async

#endif
