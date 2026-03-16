/**
 * @file qb/io/async/coroutine/client.h
 * @brief Complete coroutine client for outbound connections
 *
 * High-level coroutine client that combines connector, stream, and I/O
 * operations into a single easy-to-use interface.
 *
 * This is specifically designed for CLIENT-side usage. For server-side,
 * use the existing callback-based io<> and input<> classes.
 *
 * USAGE:
 * ======
 *
 * Simple request/response client:
 * @code
 * task<void> example() {
 *     coro_client<qb::io::transport::tcp> client;
 *
 *     // Connect
 *     if (!co_await client.connect("tcp://localhost:8080", 5s)) {
 *         std::cerr << "Failed to connect\n";
 *         co_return;
 *     }
 *
 *     // Send raw data
 *     co_await client.send("Hello, Server!\n");
 *
 *     // Receive response (if protocol configured)
 *     auto response = co_await client.receive(30s);
 *     if (response) {
 *         std::cout << "Got: " << response->size() << " bytes\n";
 *     }
 *
 *     client.disconnect();
 * }
 * @endcode
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_CLIENT_H
#define QB_IO_ASYNC_COROUTINE_CLIENT_H

#include <chrono>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "task.h"
#include "connector.h"
#include "stream.h"
#include "protocol_reader.h"
#include <qb/io/protocol/text.h>

namespace qb::io::async {

/**
 * @brief Simple message type for coroutine clients
 *
 * Holds raw data received from the transport.
 */
struct coro_message {
    std::vector<char> data;

    [[nodiscard]] std::size_t size() const noexcept { return data.size(); }
    [[nodiscard]] const char* begin() const noexcept { return data.data(); }
    [[nodiscard]] const char* end() const noexcept { return data.data() + data.size(); }
};

/**
 * @brief Complete coroutine client
 *
 * Combines connection management, message streaming, and I/O operations
 * into a single coroutine-friendly interface.
 *
 * This client operates at the transport level (raw bytes). For protocol-specific
 * clients (JSON, HTTP, etc.), build on top of this class.
 *
 * @tparam Transport Transport type (transport::tcp, transport::stcp, etc.)
 * @ingroup Coroutine
 */
template<typename Transport>
class coro_client {
public:
    using transport_type = Transport;
    using message_type = coro_message;
    using transport_io_type = typename Transport::transport_io_type;

private:
    // Connection management
    coroutine_connector<Transport> connector_;

    // Message stream for receiving
    coro_stream<message_type> stream_;

    // Optional protocol reader (when switch_protocol<Proto>() was called)
    std::unique_ptr<coro_protocol_reader_base<transport_io_type>> reader_;

    // Connection state
    bool connected_ = false;
    bool reading_ = false;

public:
    /**
     * @brief Default constructor
     */
    coro_client() = default;

    /**
     * @brief Destructor - cleanup connection
     */
    ~coro_client() {
        disconnect();
    }

    // Non-copyable
    coro_client(const coro_client&) = delete;
    coro_client& operator=(const coro_client&) = delete;

    // Movable
    coro_client(coro_client&&) = default;
    coro_client& operator=(coro_client&&) = default;

    /**
     * @brief Connect to remote endpoint
     *
     * Establishes connection.
     *
     * @param target URI of target (e.g., "tcp://host:port")
     * @param timeout Connection timeout
     * @return task<bool> true if connected successfully
     */
    [[nodiscard]] task<bool> connect(
        uri const& target,
        std::chrono::seconds timeout = std::chrono::seconds(5)
    ) {
        // Connect transport
        if (!co_await connector_.co_connect(target, timeout)) {
            co_return false;
        }

        stream_.open();
        connected_ = true;
        if (reader_) {
            reader_->start(transport());
        }
        co_return true;
    }

    /**
     * @brief Set the protocol for message parsing (e.g. line, binary).
     * Call before or after connect(); reader is started in connect() when already set.
     * @tparam Proto Protocol template (e.g. qb::protocol::text::line)
     */
    template <template <typename> class Proto>
    void switch_protocol() {
        reader_ = std::make_unique<coro_client_protocol_reader<Transport, Proto>>(
            transport(),
            [this](std::vector<char>&& vec) {
                deliver_message(coro_message{std::move(vec)});
            });
        if (connected_ && transport().is_open()) {
            reader_->start(transport());
        }
    }

    /**
     * @brief Current protocol (if any). Same API as callback client.
     */
    [[nodiscard]] IProtocol* protocol() noexcept {
        return reader_ ? reader_->protocol() : nullptr;
    }

    /**
     * @brief Start the protocol reader (if any). Called automatically from connect().
     * Call explicitly only if you set the protocol after connect().
     */
    void start() noexcept {
        if (reader_ && connected_ && transport().is_open()) {
            reader_->start(transport());
        }
    }

    /**
     * @brief Send data to the server
     *
     * Writes data to the transport. Supports buffer-like types (contiguous bytes).
     *
     * @tparam T Data type (e.g. std::string, std::string_view, std::vector<char>)
     * @param data Data to send
     * @return task<bool> true if write succeeded
     */
    template<typename T>
    [[nodiscard]] task<bool> send(T&& data) {
        if (!connected_) {
            co_return false;
        }

        auto& transport = connector_.transport();

        if (!transport.is_open()) {
            connected_ = false;
            co_return false;
        }

        const void* ptr = nullptr;
        std::size_t sz = 0;
        if constexpr (std::is_convertible_v<T, std::string_view>) {
            std::string_view sv = std::forward<T>(data);
            ptr = sv.data();
            sz = sv.size();
        } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::vector<char>>) {
            const auto& v = data;
            ptr = v.data();
            sz = v.size();
        } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, coro_message>) {
            const auto& m = data;
            ptr = m.data.data();
            sz = m.data.size();
        } else {
            static_assert(sizeof(T) == 0, "send() requires string_view, vector<char>, or coro_message");
        }

        if (sz == 0) {
            co_return true;
        }
        int n = transport.write(ptr, sz);
        co_return n >= 0;
    }

    /**
     * @brief Receive a message with timeout
     *
     * Suspends until a message is received or timeout expires.
     * Uses the message stream.
     *
     * Note: This requires setup of a read mechanism that delivers
     * to the stream. For raw transport, this returns nullopt unless
     * a protocol layer is added.
     *
     * @param timeout Maximum time to wait for message
     * @return task<std::optional<message_type>> The message, or nullopt
     */
    [[nodiscard]] task<std::optional<message_type>> receive(
        std::chrono::seconds timeout = std::chrono::seconds(30)
    ) {
        if (!connected_) {
            co_return std::nullopt;
        }

        // Try to receive immediately first
        if (auto msg = stream_.try_receive()) {
            co_return msg;
        }

        co_return co_await stream_.receive(timeout);
    }

    /**
     * @brief Send request and wait for response
     *
     * Convenience method for request/response patterns.
     *
     * @tparam T Request type
     * @param request Request data
     * @param timeout Response timeout
     * @return task<std::optional<message_type>> Response message
     */
    template<typename T>
    [[nodiscard]] task<std::optional<message_type>> request(
        T&& request,
        std::chrono::seconds timeout = std::chrono::seconds(30)
    ) {
        // Send request
        if (!co_await send(std::forward<T>(request))) {
            co_return std::nullopt;
        }

        // Wait for response
        co_return co_await receive(timeout);
    }

    /**
     * @brief Disconnect and cleanup
     */
    void disconnect() noexcept {
        if (reader_) {
            reader_->stop();
            reader_.reset();
        }
        stream_.close();
        if (connected_) {
            connector_.disconnect();
            connected_ = false;
            reading_ = false;
        }
    }

    /**
     * @brief Check if connected
     */
    [[nodiscard]] bool is_connected() const noexcept {
        return connected_ && connector_.is_connected();
    }

    /**
     * @brief Get the underlying transport
     */
    [[nodiscard]] transport_io_type& transport() noexcept {
        return connector_.transport();
    }

    /**
     * @brief Get the message stream (for advanced usage)
     */
    [[nodiscard]] coro_stream<message_type>& stream() noexcept {
        return stream_;
    }

    /**
     * @brief Deliver a message to the stream (called by read mechanism)
     */
    void deliver_message(message_type&& msg) {
        stream_.deliver(std::move(msg));
    }
};

/**
 * @brief Coroutine client with protocol fixed at compile time (coherent with callback API).
 *
 * Same as coro_client but the protocol is a template parameter; the reader is created
 * at construction and start() is called on connect(). No switch_protocol(); use this
 * when you want one protocol per client type (e.g. line, JSON), aligned with
 * callback tcp::client<Derived, Transport> where Derived::Protocol is fixed.
 *
 * @tparam Transport Transport type (e.g. qb::io::transport::tcp)
 * @tparam ProtocolTemplate Protocol template taking one IO type (e.g. qb::protocol::text::command)
 */
template<typename Transport, template<typename> class ProtocolTemplate>
class coro_client_p {
public:
    using transport_type = Transport;
    using message_type = coro_message;
    using transport_io_type = typename Transport::transport_io_type;
    using reader_type = coro_client_protocol_reader<Transport, ProtocolTemplate>;

private:
    coroutine_connector<Transport> connector_;
    coro_stream<message_type> stream_;
    std::unique_ptr<reader_type> reader_;
    bool connected_ = false;

public:
    /**
     * @brief Construct client with protocol; reader is created and will start on connect().
     */
    coro_client_p()
        : reader_(std::make_unique<reader_type>(
              connector_.transport(),
              [this](std::vector<char>&& vec) {
                  stream_.deliver(coro_message{std::move(vec)});
              })) {}

    ~coro_client_p() {
        disconnect();
    }

    coro_client_p(const coro_client_p&) = delete;
    coro_client_p& operator=(const coro_client_p&) = delete;
    /** Move disabled: reader holds pointer to this->connector_.transport(). */
    coro_client_p(coro_client_p&&) = delete;
    coro_client_p& operator=(coro_client_p&&) = delete;

    [[nodiscard]] task<bool> connect(
        uri const& target,
        std::chrono::seconds timeout = std::chrono::seconds(5))
    {
        if (!co_await connector_.co_connect(target, timeout)) {
            co_return false;
        }
        stream_.open();
        connected_ = true;
        reader_->start(transport());
        co_return true;
    }

    template<typename T>
    [[nodiscard]] task<bool> send(T&& data) {
        if (!connected_) co_return false;
        auto& tr = connector_.transport();
        if (!tr.is_open()) {
            connected_ = false;
            co_return false;
        }
        const void* ptr = nullptr;
        std::size_t sz = 0;
        if constexpr (std::is_convertible_v<T, std::string_view>) {
            std::string_view sv = std::forward<T>(data);
            ptr = sv.data();
            sz = sv.size();
        } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::vector<char>>) {
            ptr = data.data();
            sz = data.size();
        } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, coro_message>) {
            ptr = data.data.data();
            sz = data.data.size();
        } else {
            static_assert(sizeof(T) == 0, "send() requires string_view, vector<char>, or coro_message");
        }
        if (sz == 0) co_return true;
        co_return tr.write(ptr, sz) >= 0;
    }

    [[nodiscard]] task<std::optional<message_type>> receive(
        std::chrono::seconds timeout = std::chrono::seconds(30))
    {
        if (!connected_) co_return std::nullopt;
        if (auto msg = stream_.try_receive()) co_return msg;
        co_return co_await stream_.receive(timeout);
    }

    template<typename T>
    [[nodiscard]] task<std::optional<message_type>> request(
        T&& request,
        std::chrono::seconds timeout = std::chrono::seconds(30))
    {
        if (!co_await send(std::forward<T>(request))) co_return std::nullopt;
        co_return co_await receive(timeout);
    }

    void disconnect() noexcept {
        if (reader_) {
            reader_->stop();
            reader_.reset();
        }
        stream_.close();
        if (connected_) {
            connector_.disconnect();
            connected_ = false;
        }
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return connected_ && connector_.is_connected();
    }

    [[nodiscard]] transport_io_type& transport() noexcept {
        return connector_.transport();
    }

    [[nodiscard]] coro_stream<message_type>& stream() noexcept {
        return stream_;
    }

    /** @brief Current protocol (always set for coro_client_p). */
    [[nodiscard]] IProtocol* protocol() noexcept {
        return reader_ ? reader_->protocol() : nullptr;
    }
};

/**
 * @brief Helper to create client with type deduction
 * @tparam Transport Transport type
 * @return coro_client<Transport>
 */
template<typename Transport>
[[nodiscard]] auto make_coro_client() {
    return coro_client<Transport>{};
}

/** @brief Helper to create protocol-fixed client. */
template<typename Transport, template<typename> class ProtocolTemplate>
[[nodiscard]] auto make_coro_client_p() {
    return coro_client_p<Transport, ProtocolTemplate>{};
}

// Type aliases for common clients
using tcp_client = coro_client<qb::io::transport::tcp>;

/** @brief Line protocol (newline-terminated) TCP client. */
using line_tcp_client = coro_client_p<qb::io::transport::tcp, qb::protocol::text::command>;

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CLIENT_H
