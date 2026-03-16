/**
 * @file qb/io/async/coroutine/stream.h
 * @brief Coroutine stream for message-based I/O
 *
 * Provides a message stream interface for clients that allows
 * co_awaiting on message reception. This is specifically designed
 * for CLIENT-side usage where a single coroutine consumes messages.
 *
 * For server-side usage, the existing callback-based on(message&&)
 * remains the recommended approach.
 *
 * USAGE:
 * ======
 *
 * @code
 * // In a client coroutine
 * task<void> client_session(coro_stream<Protocol>& stream) {
 *     while (true) {
 *         auto msg = co_await stream.receive();  // Suspend until message
 *         if (!msg) break;  // Stream closed
 *
 *         process(*msg);
 *     }
 * }
 * @endcode
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_STREAM_H
#define QB_IO_ASYNC_COROUTINE_STREAM_H

#include <queue>
#include <optional>
#include <mutex>
#include <coroutine>
#include <chrono>

#include <ev/ev++.h>

#include "task.h"
#include "scheduler.h"
#include "../listener.h"

namespace qb::io::async {

/**
 * @brief Message stream for coroutine-based clients
 *
 * A queue-based message stream that allows coroutines to co_await
 * on message reception. Thread-safe for use with coroutine scheduler.
 *
 * @tparam MessageType Type of message delivered by the protocol
 * @ingroup Coroutine
 */
template<typename MessageType>
class coro_stream {
public:
    using message_type = MessageType;
    using optional_message = std::optional<message_type>;

private:
    // Message queue with synchronization
    std::queue<message_type> queue_;
    mutable std::mutex mutex_;

    // Handle of coroutine waiting for message
    std::coroutine_handle<> waiting_handle_;
    bool has_waiter_ = false;

    // Stream state (closed until connection is established or open() is called)
    bool closed_ = true;
    bool error_ = false;

public:
    /**
     * @brief Default constructor
     */
    coro_stream() = default;

    /**
     * @brief Deleted copy (stream is a unique resource)
     */
    coro_stream(const coro_stream&) = delete;
    coro_stream& operator=(const coro_stream&) = delete;

    /**
     * @brief Move constructor
     */
    coro_stream(coro_stream&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        queue_ = std::move(other.queue_);
        waiting_handle_ = other.waiting_handle_;
        has_waiter_ = other.has_waiter_;
        closed_ = other.closed_;
        error_ = other.error_;

        other.has_waiter_ = false;
        other.waiting_handle_ = nullptr;
    }

    /**
     * @brief Deliver a message to the stream
     *
     * Called by the protocol when a message is received.
     * If a coroutine is waiting, it will be resumed.
     *
     * @param msg The message to deliver (moved)
     * @return true if delivered, false if stream closed
     */
    bool deliver(message_type&& msg) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) {
            return false;
        }

        queue_.push(std::move(msg));

        // Resume waiting coroutine if any
        if (has_waiter_ && waiting_handle_) {
            has_waiter_ = false;
            auto handle = waiting_handle_;
            waiting_handle_ = nullptr;

            // Schedule resume via coroutine scheduler
            if (auto* sched = CoroutineScheduler::current_ptr()) {
                sched->schedule_resume(handle);
            }
        }

        return true;
    }

    /**
     * @brief Open the stream (e.g. allow delivery without connection)
     *
     * Resets closed and error state so messages can be delivered.
     */
    void open() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
        error_ = false;
    }

    /**
     * @brief Close the stream
     *
     * Signals that no more messages will be delivered.
     * Any waiting coroutine will be resumed with empty result.
     */
    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);

        closed_ = true;

        // Resume waiting coroutine with empty
        if (has_waiter_ && waiting_handle_) {
            has_waiter_ = false;
            auto handle = waiting_handle_;
            waiting_handle_ = nullptr;

            if (auto* sched = CoroutineScheduler::current_ptr()) {
                sched->schedule_resume(handle);
            }
        }
    }

    /**
     * @brief Mark stream as having error
     */
    void set_error() noexcept {
        std::coroutine_handle<> handle_to_resume;
        bool should_resume = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = true;
            closed_ = true;

            // Capture handle while holding lock
            if (has_waiter_ && waiting_handle_) {
                has_waiter_ = false;
                handle_to_resume = waiting_handle_;
                waiting_handle_ = nullptr;
                should_resume = true;
            }
        }

        // Resume outside the lock
        if (should_resume) {
            if (auto* sched = CoroutineScheduler::current_ptr()) {
                sched->schedule_resume(handle_to_resume);
            }
        }
    }

    /**
     * @brief Cancel a specific waiter (e.g. when receive timeout fires).
     * @param h The coroutine handle that was registered as waiter
     * @return true if this handle was the current waiter and was cancelled
     */
    bool cancel_waiter(std::coroutine_handle<> h) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_waiter_ && waiting_handle_ == h) {
            has_waiter_ = false;
            waiting_handle_ = nullptr;
            return true;
        }
        return false;
    }

    /**
     * @brief Check if stream is closed
     */
    [[nodiscard]] bool is_closed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /**
     * @brief Check if stream has error
     */
    [[nodiscard]] bool has_error() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    /**
     * @brief Get queue size
     */
    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief Awaiter for receive operation
     *
     * Follows the qb coroutine pattern:
     * - await_ready(): check if can complete immediately
     * - await_suspend(): register for wakeup or schedule immediate resume
     * - await_resume(): return result
     */
    struct receive_awaiter {
        coro_stream* stream;
        optional_message result_;

        receive_awaiter(coro_stream* s, optional_message res = std::nullopt)
            : stream(s), result_(std::move(res)) {}

        // Check if we can complete immediately
        bool await_ready() const noexcept {
            std::lock_guard<std::mutex> lock(stream->mutex_);

            // Ready if message available or stream closed
            if (stream->closed_ && stream->queue_.empty()) {
                const_cast<receive_awaiter*>(this)->result_ = std::nullopt;
                return true;
            }

            if (!stream->queue_.empty()) {
                const_cast<receive_awaiter*>(this)->result_ = std::move(
                    const_cast<std::queue<message_type>&>(stream->queue_).front()
                );
                const_cast<std::queue<message_type>&>(stream->queue_).pop();
                return true;
            }

            return false;  // Need to suspend
        }

        // Register for wakeup - always suspend (return void)
        void await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> lock(stream->mutex_);

            // Double-check after acquiring lock (race condition with deliver)
            if (stream->closed_ && stream->queue_.empty()) {
                result_ = std::nullopt;
                // Schedule immediate resume since we know result
                if (auto* sched = CoroutineScheduler::current_ptr()) {
                    sched->schedule_resume(h);
                }
                return;
            }

            if (!stream->queue_.empty()) {
                result_ = std::move(stream->queue_.front());
                stream->queue_.pop();
                // Schedule immediate resume since we got message
                if (auto* sched = CoroutineScheduler::current_ptr()) {
                    sched->schedule_resume(h);
                }
                return;
            }

            // No message available - register as waiter
            // deliver() or close() will call schedule_resume later
            stream->waiting_handle_ = h;
            stream->has_waiter_ = true;
        }

        optional_message await_resume() {
            // If result already set (by await_ready or await_suspend), return it
            if (result_) {
                return std::move(result_);
            }

            // Otherwise, try to get from queue (when woken by deliver/close)
            std::lock_guard<std::mutex> lock(stream->mutex_);

            if (stream->closed_ && stream->queue_.empty()) {
                return std::nullopt;
            }

            if (!stream->queue_.empty()) {
                auto msg = std::move(stream->queue_.front());
                stream->queue_.pop();
                return msg;
            }

            // Should not happen, but return nullopt just in case
            return std::nullopt;
        }
    };

    /**
     * @brief Awaiter for receive with timeout
     *
     * Resumes with message, nullopt if stream closed, or nullopt if timeout expired.
     */
    struct receive_awaiter_with_timeout {
        coro_stream* stream;
        std::chrono::seconds timeout_;
        optional_message result_;
        bool timed_out_ = false;
        std::coroutine_handle<> handle_;
        CoroutineScheduler* scheduler_ = nullptr;
        ev_timer watcher_{};
        ev::loop_ref loop_;
        bool timer_started_ = false;

        receive_awaiter_with_timeout(coro_stream* s, std::chrono::seconds timeout, ev::loop_ref loop)
            : stream(s), timeout_(timeout), loop_(loop) {
            ev_timer_init(&watcher_, timer_callback, 0.0, 0.0);
            watcher_.data = this;
        }

        static void timer_callback(struct ev_loop*, ev_timer* w, int) noexcept {
            auto* self = static_cast<receive_awaiter_with_timeout*>(w->data);
            if (self && !self->timed_out_) {
                self->timed_out_ = true;
                self->stream->cancel_waiter(self->handle_);
                if (self->scheduler_ && self->handle_) {
                    self->scheduler_->schedule_resume(self->handle_);
                }
            }
        }

        bool await_ready() const noexcept {
            std::lock_guard<std::mutex> lock(stream->mutex_);
            if (stream->closed_ && stream->queue_.empty()) {
                const_cast<receive_awaiter_with_timeout*>(this)->result_ = std::nullopt;
                return true;
            }
            if (!stream->queue_.empty()) {
                const_cast<receive_awaiter_with_timeout*>(this)->result_ = std::move(
                    const_cast<std::queue<message_type>&>(stream->queue_).front());
                const_cast<std::queue<message_type>&>(stream->queue_).pop();
                return true;
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) {
            handle_ = h;
            scheduler_ = CoroutineScheduler::current_ptr();
            if (!scheduler_) {
                scheduler_ = &CoroutineScheduler::current();
            }
            {
                std::lock_guard<std::mutex> lock(stream->mutex_);
                if (stream->closed_ && stream->queue_.empty()) {
                    result_ = std::nullopt;
                    scheduler_->schedule_resume(h);
                    return;
                }
                if (!stream->queue_.empty()) {
                    result_ = std::move(stream->queue_.front());
                    stream->queue_.pop();
                    scheduler_->schedule_resume(h);
                    return;
                }
                stream->waiting_handle_ = h;
                stream->has_waiter_ = true;
            }
            double after = (timeout_.count() <= 0) ? 0.001 : static_cast<double>(timeout_.count());
            ev_timer_set(&watcher_, after, 0.0);
            ev_timer_start(loop_, &watcher_);
            timer_started_ = true;
        }

        optional_message await_resume() {
            if (timer_started_ && ev_is_active(&watcher_)) {
                ev_timer_stop(loop_, &watcher_);
                timer_started_ = false;
            }
            if (timed_out_) {
                return std::nullopt;
            }
            if (result_) {
                return std::move(result_);
            }
            std::lock_guard<std::mutex> lock(stream->mutex_);
            if (stream->closed_ && stream->queue_.empty()) {
                return std::nullopt;
            }
            if (!stream->queue_.empty()) {
                auto msg = std::move(stream->queue_.front());
                stream->queue_.pop();
                return msg;
            }
            return std::nullopt;
        }
    };

    /**
     * @brief Receive a message from the stream
     *
     * Suspends until a message is available or the stream is closed.
     *
     * @return optional_message The message, or nullopt if stream closed
     *
     * @example
     * @code
     * while (auto msg = co_await stream.receive()) {
     *     process(*msg);
     * }
     * // Stream closed
     * @endcode
     */
    [[nodiscard]] receive_awaiter receive() noexcept {
        return receive_awaiter{this, std::nullopt};
    }

    /**
     * @brief Receive a message with timeout
     *
     * Suspends until a message is available, the stream is closed, or the timeout expires.
     * @param timeout Maximum time to wait
     * @return optional_message The message, or nullopt if closed or timeout
     */
    [[nodiscard]] receive_awaiter_with_timeout receive(std::chrono::seconds timeout) noexcept {
        return receive_awaiter_with_timeout{this, timeout, listener::current.loop()};
    }

    /**
     * @brief Try to receive without blocking
     * @return optional_message The message if available, nullopt otherwise
     */
    [[nodiscard]] optional_message try_receive() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return std::nullopt;
        }

        auto msg = std::move(queue_.front());
        queue_.pop();
        return msg;
    }
};

/**
 * @brief Helper to create a stream with type deduction
 * @tparam MessageType Type of messages
 * @return coro_stream<MessageType>
 */
template<typename MessageType>
[[nodiscard]] auto make_coro_stream() {
    return coro_stream<MessageType>{};
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_STREAM_H
