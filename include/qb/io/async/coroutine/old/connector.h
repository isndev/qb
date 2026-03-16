/**
 * @file qb/io/async/coroutine/connector.h
 * @brief Async connector with coroutine support for outbound connections
 *
 * Provides coroutine-based connection establishment for TCP clients.
 * Wraps the existing qb::io::async::tcp::connect() callback API into
 * a clean co_await interface.
 *
 * USAGE:
 * ======
 *
 * @code
 * qb::io::async::coroutine_connector<qb::io::transport::tcp> connector;
 *
 * if (co_await connector.co_connect("tcp://localhost:8080", 5s)) {
 *     // Connection established
 *     auto transport = connector.transport();
 * }
 * @endcode
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_CONNECTOR_H
#define QB_IO_ASYNC_COROUTINE_CONNECTOR_H

#include "task.h"
#include "../tcp/connector.h"
#include "../../transport/tcp.h"
#include <functional>
#include <optional>

namespace qb::io::async {

/**
 * @brief Async connection awaiter
 *
 * Bridges tcp::connect() callback to coroutine.
 * Suspends until connection completes (success or failure).
 */
template<typename Transport>
class connection_awaiter {
    using transport_io_type = typename Transport::transport_io_type;

    // Connection state - using tristate: empty=not_ready, false=failed, true=success
    std::optional<bool> result_;
    std::optional<transport_io_type> transport_result_;
    std::function<void(std::function<void(bool, transport_io_type)>)> async_op_;
    std::coroutine_handle<> handle_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> resumed_{false};
    CoroutineScheduler* scheduler_ = nullptr;

public:
    /**
     * @brief Construct with connection operation
     * @param async_op Function that initiates tcp::connect()
     */
    explicit connection_awaiter(
        std::function<void(std::function<void(bool, transport_io_type)>)> async_op
    ) : async_op_(std::move(async_op)) {}

    bool await_ready() const noexcept {
        return result_.has_value();
    }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        scheduler_ = CoroutineScheduler::current_ptr();
        if (!scheduler_) {
            scheduler_ = &CoroutineScheduler::current();
        }

        // Create callback that stores result and resumes
        auto callback = [this](bool success, transport_io_type transport) {
            result_ = success;
            if (success && transport.is_open()) {
                transport_result_ = std::move(transport);
            }
            ready_.store(true, std::memory_order_release);

            // Schedule resume via scheduler
            if (scheduler_ && handle_) {
                scheduler_->schedule_resume(handle_);
            }
        };

        // Initiate async connection
        async_op_(callback);
    }

    std::pair<bool, std::optional<transport_io_type>> await_resume() {
        resumed_.store(true, std::memory_order_release);
        return {result_.value_or(false), std::move(transport_result_)};
    }
};

/**
 * @brief Coroutine-enabled connector for outbound TCP connections
 *
 * Wraps tcp::connect() with a co_await interface.
 * Compatible with all transport types (tcp, stcp for SSL).
 *
 * @tparam Transport Transport type (transport::tcp, transport::stcp, etc.)
 * @ingroup Coroutine
 */
template<typename Transport>
class coroutine_connector {
    typename Transport::transport_io_type transport_;
    bool connected_ = false;

public:
    using transport_type = Transport;
    using transport_io_type = typename Transport::transport_io_type;

    /**
     * @brief Default constructor
     */
    coroutine_connector() = default;

    /**
     * @brief Connect to remote endpoint with coroutine
     *
     * Suspends until connection completes or timeout expires.
     * Uses the existing tcp::connect() infrastructure.
     *
     * The connection is considered successful ONLY when:
     * 1. The underlying tcp::connect() calls the callback with success=true
     * 2. The transport socket is valid and open
     *
     * @param target URI of target (e.g., "tcp://host:port")
     * @param timeout Connection timeout duration
     * @return task<bool> true if connected successfully
     *
     * @example
     * @code
     * auto connector = coroutine_connector<transport::tcp>{};
     * if (co_await connector.co_connect("tcp://localhost:8080", 5s)) {
     *     // Use connector.transport()
     * }
     * @endcode
     */
    [[nodiscard]] task<bool> co_connect(
        uri const& target,
        std::chrono::seconds timeout = std::chrono::seconds(5)
    ) {
        // Create awaiter that wraps tcp::connect()
        // The tcp::connect() callback contract:
        // - Called with (true, socket) when connection succeeds
        // - Called with (false, empty_socket) when connection fails
        auto awaiter = connection_awaiter<Transport>(
            [&target, timeout](auto cb) {
                tcp::connect<transport_io_type>(
                    target,
                    [cb](auto&& transport) mutable {
                        // The transport validity is the indicator of success
                        // tcp::connector guarantees:
                        // - On success: transport is valid and connected
                        // - On failure: transport is default/empty
                        bool success = transport.is_open();
                        cb(success, std::move(transport));
                    },
                    timeout.count()
                );
            }
        );

        auto [success, received_transport] = co_await awaiter;

        // Double-check: success must be true AND we must have a valid transport
        if (success && received_transport && received_transport->is_open()) {
            transport_ = std::move(*received_transport);
            connected_ = true;
            co_return true;
        }

        connected_ = false;
        co_return false;
    }

    /**
     * @brief Get the established transport
     * @return Reference to transport (valid after successful co_connect)
     */
    [[nodiscard]] transport_io_type& transport() noexcept {
        return transport_;
    }

    /**
     * @brief Check if connected
     * @return true if connection established
     */
    [[nodiscard]] bool is_connected() const noexcept {
        return connected_;
    }

    /**
     * @brief Disconnect and cleanup
     */
    void disconnect() noexcept {
        if (transport_.is_open()) {
            transport_.close();
        }
        connected_ = false;
    }
};

/**
 * @brief Helper to create connector with type deduction
 * @tparam Transport Transport type
 * @return coroutine_connector<Transport>
 */
template<typename Transport>
[[nodiscard]] auto make_coroutine_connector() {
    return coroutine_connector<Transport>{};
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CONNECTOR_H
