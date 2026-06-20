/**
 * @file qb/io/async/io.h
 * @brief Core asynchronous I/O class templates for event-driven operations.
 *
 * This file defines the fundamental class templates for asynchronous I/O operations
 * in the QB IO library. It includes base classes for handling events, timeouts,
 * file watching, and bidirectional I/O with protocol-based message processing.
 *
 * The classes in this file use the Curiously Recurring Template Pattern (CRTP)
 * to provide static polymorphism, avoiding the overhead of virtual function calls
 * while still allowing customization in derived classes.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#ifndef QB_IO_ASYNC_IO_H
#define QB_IO_ASYNC_IO_H

#include <memory>
#include <qb/system/time.h>
#include <qb/utility/type_traits.h>
#include "../config.h"
#include "../system/sys__socket.h"
#include "event/all.h"
#include "listener.h"
#include "protocol.h"

// Modern C++: member check macros replaced by traits in qb/utility/type_traits.h
// - qb::has_type_Protocol<T>
// - qb::has_flush<C, Args...> / qb::has_flush_r<C, Ret, Args...>

#define Derived static_cast<_Derived &>(*this)

namespace qb::io::async {

/**
 * @class base
 * @ingroup Async
 * @brief Base class for all qb-io asynchronous components that interact with the event listener.
 *
 * This template class provides the foundational mechanism for registering and unregistering
 * a specific libev event watcher (represented by `_EV_EVENT`) with the thread-local
 * `qb::io::async::listener::current`. Derived classes (using CRTP via `_Derived`)
 * will typically inherit from this to manage their primary event watcher.
 *
 * @note **Thread Safety:** Instances of classes derived from `base` are designed to be used
 *       exclusively within a single thread (VirtualCore). They must not be shared between threads.
 *       This is enforced by the thread-local nature of `listener::current`. All event handlers
 *       (`on()` methods) are called sequentially within the same thread, eliminating the need
 *       for synchronization primitives. On Windows, libev’s epoll path uses wepoll (IOCP); keep
 *       the whole loop lifecycle on that thread (see `listener` documentation).
 *
 * @tparam _Derived The derived class type (CRTP pattern).
 * @tparam _EV_EVENT The specific `qb::io::async::event::*` type (which wraps a libev watcher, e.g., `event::io`, `event::timer`).
 */
template <typename _Derived, typename _EV_EVENT>
class base {
protected:
    _EV_EVENT &_async_event; /**< Reference to the registered libev-based event watcher. */

    /**
     * @brief Constructor that registers the event watcher with the current listener.
     * @details The `_async_event` member is initialized by calling `listener::current.registerEvent`,
     *          associating the derived class instance (`Derived`) as the handler for this event type.
     */
    base()
        : _async_event(listener::current.registerEvent<_EV_EVENT>(Derived)) {}

    /**
     * @brief Destructor that unregisters the event watcher.
     * @details Stops the event watcher and unregisters it from the listener to prevent dangling references
     *          and ensure proper cleanup of libev resources.
     *          Specifically, it calls `_async_event.stop()` and then `listener::current.unregisterEvent(_async_event._interface)`.
     */
    ~base() {
        // std::cout << "handle=" << _async_event.fd
        //           << " disposed async.stop()" << std::endl;
        _async_event.stop();
        listener::current.unregisterEvent(_async_event._interface);
    }
};

/**
 * @class with_timeout
 * @ingroup Async
 * @brief CRTP base class that adds timeout functionality to derived asynchronous components.
 *
 * This template class extends `qb::io::async::base` by incorporating an `event::timer`.
 * It allows the derived class to set a timeout, after which an `on(event::timer&)` method
 * in the derived class is triggered if no activity (signaled by `updateTimeout()`) is detected.
 *
 * @tparam _Derived The derived class type (CRTP pattern) that will handle the timeout event.
 */
template <typename _Derived>
class with_timeout : public base<with_timeout<_Derived>, event::timer> {
    ev_tstamp _timeout;       /**< Timeout value in seconds. If 0, timeout is disabled. */
    ev_tstamp _last_activity; /**< Timestamp of the last recorded activity, used to check against timeout. */

public:
    /**
     * @brief Constructor that initializes the timeout.
     * @param timeout Initial timeout value in seconds. A value of `0.0` or less disables the timeout initially.
     *                The timer is started if `timeout` is greater than `0.0`.
     */
    explicit with_timeout(qb::duration timeout = std::chrono::seconds(3))
        : _timeout(qb::detail::to_ev_seconds(timeout))
        , _last_activity(0) {
        if (_timeout > 0.) {
            ev_now_update(static_cast<struct ev_loop *>(listener::current.loop()));
            this->_async_event.start(_timeout);
        }
    }

    /**
     * @brief Updates the last activity timestamp to the current event loop time.
     * @details This method should be called by the derived class whenever an activity occurs
     *          that should reset the timeout countdown (e.g., receiving data, user input).
     *          It sets `_last_activity` to `this->_async_event.loop.now()`.
     */
    void
    updateTimeout() noexcept {
        ev_now_update(static_cast<struct ev_loop *>(listener::current.loop()));
        _last_activity = this->_async_event.loop.now();
    }

    /**
     * @brief Sets a new timeout value and restarts the timer.
     * @param timeout New timeout duration. If zero or negative, the timer is stopped (disabled).
     *                Otherwise, the timer is configured with the new timeout and started.
     *                The `_last_activity` timestamp is also updated to the current event loop time.
     */
    void
    setTimeout(qb::duration timeout) noexcept {
        _timeout = qb::detail::to_ev_seconds(timeout);
        if (_timeout > 0.) { // Check against 0, not just if(_timeout)
            ev_now_update(static_cast<struct ev_loop *>(listener::current.loop()));
            _last_activity = this->_async_event.loop.now();
            this->_async_event.set(_timeout);
            this->_async_event.start();
        } else
            this->_async_event.stop();
    }

    /**
     * @brief Gets the current configured timeout duration.
     * @return Current timeout as a `qb::duration` (zero if disabled).
     */
    [[nodiscard]] qb::duration
    getTimeout() const noexcept {
        return qb::detail::from_ev_seconds(_timeout);
    }

private:
    friend class listener::RegisteredKernelEvent<event::timer, with_timeout>;

    /**
     * @brief Internal timer event handler, called by the listener when the `event::timer` expires.
     * @param event The `event::timer` that was triggered.
     * @details This method checks if the actual timeout duration (`_timeout`) has passed since
     *          `_last_activity`. If it has, it calls the `on(event::timer&)` method of the `_Derived`
     *          class. Otherwise (if activity occurred more recently than `_timeout` ago), it reschedules
     *          the timer for the remaining duration.
     */
    void
    on(event::timer &event) noexcept {
        const ev_tstamp after = _last_activity - event.loop.now() + _timeout;

        if (after <= 0.)
            Derived.on(event);
        else {
            this->_async_event.set(after);
            this->_async_event.start();
        }
    }
};

/**
 * @class Timeout
 * @ingroup Async
 * @brief Utility class to execute a function after a specified timeout using the event loop.
 *
 * This class inherits from `with_timeout` to schedule a one-shot execution of a
 * provided callable (function, lambda, functor). It automatically manages its own lifetime,
 * deleting itself after the function is executed or if the timeout is explicitly cancelled.
 *
 * **Allocation strategy (QB_IO_PLAN 2.15):** every `Timeout<F>` instantiation
 * keeps a thread-local LIFO freelist so that successive `async::callback()`
 * invocations reuse the same storage. Combined with the `RegisteredKernelEvent`
 * slab (QB_IO_PLAN 2.13), a steady-state `callback()` burns exactly zero
 * `malloc`/`free` calls.
 *
 * @tparam _Func The function type (or callable object type) to execute after the timeout.
 */
template <typename _Func>
class Timeout : public with_timeout<Timeout<_Func>> {
    _Func _func; /**< The callable (function, lambda, functor) to execute upon timeout. */
    bool  _delete_only = false;

    // ---- Thread-local freelist (QB_IO_PLAN 2.15) ---------------------------
    // `Timeout<_Func>` is self-deleting (`delete this` on fire), so we can
    // safely intercept `operator new` / `operator delete` on a per-
    // instantiation basis. The listener is thread-local and all
    // `async::callback` traffic flows through it, so this pool sees zero
    // contention. The pool is deliberately not drained at thread exit — the
    // OS reclaims the thread's memory, and releasing the chain from a TLS
    // destructor would race with late `delete this` calls from already-fired
    // timers whose loop iteration outlives the listener.
    struct FreeList {
        void *head = nullptr;
    };

    static FreeList &
    _freelist() noexcept {
        thread_local FreeList fl;
        return fl;
    }

public:
    static void *
    operator new(std::size_t sz) {
        auto &fl = _freelist();
        if (fl.head) {
            void *p = fl.head;
            fl.head = *static_cast<void **>(p);
            return p;
        }
        return ::operator new(sz);
    }

    static void
    operator delete(void *p) noexcept {
        if (!p)
            return;
        auto &fl                 = _freelist();
        *static_cast<void **>(p) = fl.head;
        fl.head                  = p;
    }

    // C++14 sized-delete forward.
    static void
    operator delete(void *p, std::size_t) noexcept {
        Timeout::operator delete(p);
    }

    /**
     * @brief Constructor that schedules a function to be called after a timeout.
     * @param func The function to execute. It will be moved into the Timeout object.
     * @param timeout Timeout duration in seconds before execution. If `0.0` or less,
     *                the function is executed immediately (or in the next loop iteration,
     *                depending on `with_timeout` behavior) and this `Timeout` object is deleted.
     */
    Timeout(_Func &&func, qb::duration timeout)
        : with_timeout<Timeout<_Func>>(timeout > qb::duration::zero() ? timeout : qb::duration::zero())
        , _func(std::forward<_Func>(func)) {
        if (timeout <= qb::duration::zero()) {
            try {
                _func();
            } catch (...) {
            }
            _delete_only = true;
            this->_async_event.start(0.);
        }
    }

    /**
     * @brief Timer event handler called when the timeout expires.
     * @param event The `event::timer` that triggered (marked as unused here as specific timer details are not needed).
     * @details Executes the stored function `_func` and then deletes this `Timeout` object,
     *          ensuring one-shot execution and automatic cleanup.
     */
    void
    on(event::timer const & /*event*/) {
        if (!_delete_only) {
            try {
                _func();
            } catch (...) {
            }
        }
        delete this;
    }
};

/**
 * @brief Utility function to schedule a callable for execution after a timeout.
 * @ingroup Async
 * @tparam _Func The type of the callable (function, lambda, functor).
 * @param func The callable to execute. It is moved into the internal `Timeout` object.
 * @param timeout Timeout duration in seconds before execution. A timeout of `0.0` (or less)
 *                typically means the callback will be scheduled for the next iteration of the event loop,
 *                or executed immediately if the `Timeout` class logic handles it that way.
 * @details This function creates a `Timeout<_Func>` object on the heap (`new Timeout`), which
 *          then manages its own lifetime, deleting itself after `func` is called.
 *          It provides a convenient way to achieve delayed execution without manual timer management.
 */
template <typename _Func>
void
callback(_Func &&func) {
    func();
}

template <typename _Func, typename Rep, typename Period>
void
callback(_Func &&func, std::chrono::duration<Rep, Period> timeout) {
    const qb::duration d = std::chrono::duration_cast<qb::duration>(timeout);
    if (d <= qb::duration::zero()) {
        func();
        return;
    }
    // libev caches the monotonic "now" at the start of each loop iteration.
    // When the caller schedules a timer from a context that has been idle for
    // a long time (e.g. a user thread that just returned from a blocking
    // `sleep_for`), that cache can be arbitrarily stale, which makes the new
    // timer expire far earlier than requested — `expire = ev_mn_now + timeout`
    // becomes `(t_now - delta) + timeout`, triggering on the very next
    // `ev_run`. Refreshing the cache here guarantees the requested delay is
    // honoured regardless of how long the owning thread slept outside the loop.
    ev_now_update(static_cast<struct ev_loop *>(listener::current.loop()));
    new Timeout<_Func>(std::forward<_Func>(func), d);
}

/**
 * @class ScopedTimeout
 * @ingroup Async
 * @brief RAII one-shot timer whose lifetime is owned by the caller.
 *
 * Unlike `Timeout<_Func>` (which `delete`s itself after firing), `ScopedTimeout`
 * is owned by the caller via `std::unique_ptr` (or direct stack placement as a
 * member variable). This avoids the heap allocation + self-delete dance on hot
 * paths where the caller already keeps the timer handle around (retry loops,
 * watchdogs, per-connection deadlines, …) and makes cancellation trivial:
 * destroying the object stops the watcher and releases its registration.
 *
 * If `timeout <= 0`, the callback is executed inline at construction time (to
 * match `callback()`'s "fire immediately" semantics).
 *
 * @tparam _Func Callable invoked when the timer expires.
 */
template <typename _Func>
class ScopedTimeout : public with_timeout<ScopedTimeout<_Func>> {
    _Func _func;
    bool  _fired = false;

public:
    ScopedTimeout(_Func &&func, qb::duration timeout)
        : with_timeout<ScopedTimeout<_Func>>(timeout > qb::duration::zero() ? timeout : qb::duration::zero())
        , _func(std::forward<_Func>(func)) {
        if (timeout <= qb::duration::zero()) {
            _fired = true;
            try {
                _func();
            } catch (...) {
            }
        }
    }

    /** @brief Whether the scheduled callback has already executed. */
    [[nodiscard]] bool
    fired() const noexcept {
        return _fired;
    }

    /** @brief Cancel the scheduled callback if it has not fired yet. */
    void
    cancel() noexcept {
        this->setTimeout(qb::duration::zero());
    }

    /**
     * @brief Internal libev timer callback. Users should not call this directly.
     * @private
     */
    void
    on(event::timer const & /*event*/) {
        if (_fired)
            return;
        _fired = true;
        try {
            _func();
        } catch (...) {
        }
    }
};

/**
 * @brief Create an owned one-shot timer without relying on the self-deleting
 *        `Timeout<_Func>` pattern used by `callback()`.
 * @ingroup Async
 *
 * @tparam _Func Callable invoked when the timer expires.
 * @param  func  The callable to execute.
 * @param  timeout Timer duration in seconds (0 or less → fires immediately).
 * @return `std::unique_ptr<ScopedTimeout<std::decay_t<_Func>>>` owning the timer.
 *
 * Prefer this over `async::callback()` on hot paths: destroying (or reusing) the
 * returned pointer cancels the pending callback without touching the heap twice.
 */
template <typename _Func>
[[nodiscard]] auto
scoped_callback(_Func &&func) {
    using Timer = ScopedTimeout<std::decay_t<_Func>>;
    return std::make_unique<Timer>(std::forward<_Func>(func), qb::duration::zero());
}

/**
 * @brief `std::chrono` / `qb::duration` overload of `scoped_callback`.
 * @ingroup Async
 */
template <typename _Func, typename Rep, typename Period>
[[nodiscard]] auto
scoped_callback(_Func &&func, std::chrono::duration<Rep, Period> timeout) {
    using Timer = ScopedTimeout<std::decay_t<_Func>>;
    return std::make_unique<Timer>(std::forward<_Func>(func), std::chrono::duration_cast<qb::duration>(timeout));
}

/**
 * @class file_watcher
 * @ingroup FileSystem
 * @brief CRTP base class for watching a single file for attribute changes and processing its contents.
 *
 * This template class uses an `event::file` (which wraps `ev::stat`) to monitor
 * a specified file path for changes in its attributes (e.g., size, modification time).
 * When changes are detected, it can read the file content and process it using a `AProtocol`.
 *
 * @tparam _Derived The derived class type (CRTP pattern) that implements transport-specific
 *                  read operations and handles protocol messages and file events.
 */
template <typename _Derived>
class file_watcher : public base<file_watcher<_Derived>, event::file> {
    using base_t                                      = base<file_watcher<_Derived>, event::file>;
    IProtocol                              *_protocol = nullptr; /**< Active protocol (non-owning view into _protocol_list). */
    std::vector<std::unique_ptr<IProtocol>> _protocol_list;      /**< Owned protocol instances (RAII). */
    std::size_t                             _max_message_size = QB_MAX_MESSAGE_SIZE; /**< Maximum allowed message size. */

public:
    using base_io_t                     = file_watcher<_Derived>; /**< Base I/O type alias for CRTP. */
    constexpr static const bool do_read = true;                   /**< Flag indicating this watcher type reads file content. */

    /**
     * @brief Default constructor.
     * Initializes the file_watcher without a specific protocol.
     * A protocol can be set later using `switch_protocol`.
     */
    file_watcher() = default;

    /**
     * @brief Constructor with an initial protocol instance.
     * @param protocol Pointer to an existing protocol instance. The watcher takes
     *                 ownership and will delete it on destruction — the same
     *                 contract as `input(IProtocol*)` / `io(IProtocol*)` (the
     *                 previous non-owning behavior here was the lone exception
     *                 and leaked the instance unless callers tracked it manually).
     */
    file_watcher(IProtocol *protocol)
        : _protocol(protocol) {
        _protocol_list.push_back(std::unique_ptr<IProtocol>(protocol));
    }

    /**
     * @brief Deleted copy constructor to prevent unintended copying of watcher state and resources.
     */
    file_watcher(file_watcher const &) = delete;

    /**
     * @brief Destructor.
     * @details Cleans up all protocol instances created and owned by this `file_watcher`
     *          (i.e., those added to `_protocol_list` via `switch_protocol`).
     */
    ~file_watcher() noexcept = default;

    /**
     * @brief Switches to a new protocol for processing file contents, taking ownership.
     * @tparam _Protocol The concrete `AProtocol` type to instantiate.
     * @tparam _Args Argument types for the `_Protocol` constructor.
     * @param args Arguments to forward to the `_Protocol` constructor.
     * @return Pointer to the newly created and activated protocol instance if successful (protocol's `ok()` returns true),
     *         otherwise `nullptr` (and the created protocol is deleted).
     * @details Any previously owned protocol is not deleted by this method. The new protocol instance
     *          is added to an internal list and will be cleaned up by the `file_watcher` destructor.
     *          The current active protocol is set to this new instance.
     *
     * @note **Error Handling:** If the protocol constructor throws an exception, it will propagate to the caller.
     *       If `ok()` returns false, the protocol is immediately deleted and `nullptr` is returned.
     *
     * @note **Memory Management:** The protocol instance is owned by this component and will be deleted when
     *       the component is destroyed.
     */
    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&...args) {
        auto up = std::make_unique<_Protocol>(std::forward<_Args>(args)...);
        if (up->ok()) {
            auto *raw = up.get();
            _protocol_list.push_back(std::move(up));
            _protocol = raw;
            return raw;
        }
        return nullptr;
    }

    /**
     * @brief Starts watching a file for attribute changes.
     * @param fpath Path to the file to watch.
     * @param ts Polling interval in seconds. Libev uses this to check for changes.
     *           A smaller interval means more responsive but higher CPU usage. Default is 0.1 seconds.
     * @details Initializes and starts the underlying `ev::stat` watcher for the specified file.
     *          The `on(event::file&)` handler will be called when changes are detected.
     */
    void
    start(std::string const &fpath, qb::duration interval = std::chrono::milliseconds(100)) noexcept {
        this->_async_event.start(fpath.c_str(), qb::detail::to_ev_seconds(interval));
    }

    /**
     * @brief Stops watching the file.
     * @details Stops the underlying `ev::stat` watcher. No more file events will be generated for this path.
     *          The associated file descriptor in the transport is typically not closed by this call alone.
     */
    void
    disconnect() noexcept {
        this->_async_event.stop();
    }

    /**
     * @brief Reads all available data from the file and processes it using the current protocol.
     * @return `0` on success (all data read and processed, or file unchanged and no data to read).
     *         `-1` if a read error occurs, or if the protocol becomes invalid, or if `_Derived::read()` fails.
     * @details This method is typically called from the `on(event::file&)` handler when a change
     *          is detected (e.g. file size increased). It repeatedly calls `_Derived::read()` to fill the input buffer,
     *          then processes messages via `_protocol->getMessageSize()` and `_protocol->onMessage()`.
     *          It also invokes `_Derived::eof()` and potentially `_Derived::on(event::pending_read&)`
     *          or `_Derived::on(event::eof&)` based on the read outcome and buffer state.
     *
     * @note **Error Handling:** If this method returns `-1`, the `on(event::file&)` handler will stop
     *       the watcher and close the file. The error is not propagated via an event, but the watcher
     *       will stop monitoring the file. If you need error notification, implement `on(event::file&)`
     *       in your derived class to handle the case where `read_all()` fails.
     *
     * @note Requires `_Derived::do_read` to be true (which it is for `file_watcher` by default).
     *       Assumes `_protocol` is not null if messages are expected.
     *
     * @note **Protocol Validation:** If `_protocol` is null or becomes invalid during processing,
     *       the method returns `-1` immediately. This ensures that invalid protocol states are
     *       detected and handled appropriately.
     */
    int
    read_all() {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        std::size_t          ret         = 0u;
        do {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                return -1;
            // Validate protocol before processing messages
            if (unlikely(!this->_protocol))
                return -1;
            // Check protocol validity before and during message processing
            if (unlikely(!this->_protocol->ok()))
                return -1;
            // Use a separate variable for the framing loop: reusing `ret` here
            // (previous code) left it at 0 after the inner loop, so the outer
            // do/while never iterated and only one read() chunk was processed
            // per file event — files larger than one read buffer stalled.
            std::size_t msg_size = 0u;
            while ((msg_size = this->_protocol->getMessageSize()) > 0) {
                if (unlikely(msg_size > _max_message_size)) {
                    this->_protocol->not_ok();
                    return -1;
                }
                this->_protocol->onMessage(msg_size);
                if (unlikely(!this->_protocol->ok()))
                    return -1;
                Derived.flush(msg_size);
            }
            Derived.eof();
            if constexpr (qb::has_on<_Derived, event::pending_read> || qb::has_on<_Derived, event::eof>) {
                const auto pendingRead = Derived.pendingRead();
                if (pendingRead) {
                    if constexpr (qb::has_on<_Derived, event::pending_read>) {
                        auto evt__pending_read = event::pending_read{pendingRead};
                        Derived.on(std::move(evt__pending_read));
                    }
                } else {
                    if constexpr (qb::has_on<_Derived, event::eof>) {
                        auto evt__eof = event::eof{};
                        Derived.on(std::move(evt__eof));
                    }
                }
            }
        } while (ret);
        return 0;
    }

private:
    friend class listener::RegisteredKernelEvent<event::file, file_watcher>;

    /**
     * @brief Internal file event handler, called by the listener when `event::file` (for the watched file) triggers.
     * @param event The `event::file` containing current (`event.attr`) and previous (`event.prev`) `stat` data.
     * @details
     * This handler is invoked by the event loop when libev detects a change in the watched file's attributes.
     * It performs several checks:
     *  - Forwards the raw `event::file` to `_Derived::on(event::file&)` if the derived class implements it.
     *  - Checks if the file link count (`event.attr.st_nlink`) is zero, indicating deletion, and if so, stops watching and closes.
     *  - Checks if the protocol is valid (`_protocol->ok()`).
     *  - If `_Derived::do_read` is true and the file size has increased (`diff_read > 0`), it calls `read_all()`.
     *  - If any critical error occurs (e.g., `lseek` fails after size decrease, `read_all()` returns < 0), it stops the watcher and closes.
     */
    void
    on(event::file const &event) {
        int ret = 0;

        // forward event to Derived if desired
        if constexpr (qb::has_on<_Derived, event::file>) {
            Derived.on(event);
        }

        auto diff_read = event.attr.st_size - event.prev.st_size;
        if (!_protocol || !_protocol->ok() || !event.attr.st_nlink
            || (diff_read < 0 && lseek(Derived.transport().native_handle(), 0, SEEK_SET)))
            ret = -1;
        else if (diff_read) {
            if constexpr (_Derived::do_read) {
                ret = read_all();
            }
        }

        if (ret < 0) {
            this->_async_event.stop();
            Derived.close();
        }
    }
};

/**
 * @class directory_watcher
 * @ingroup FileSystem
 * @brief CRTP base class for watching a directory for attribute changes.
 *
 * This template class uses an `event::file` (wrapping `ev::stat`) to monitor
 * a specified directory path for changes in its attributes. Unlike `file_watcher`,
 * it generally does not read directory contents itself but notifies the derived class of changes.
 *
 * @tparam _Derived The derived class type (CRTP pattern) that handles the `on(event::file&)` notification.
 */
template <typename _Derived>
class directory_watcher : public base<directory_watcher<_Derived>, event::file> {
    using base_t = base<directory_watcher<_Derived>, event::file>;

public:
    using base_io_t                     = directory_watcher<_Derived>; /**< Base I/O type alias for CRTP. */
    constexpr static const bool do_read = false; /**< Flag indicating this watcher type does not read directory content directly. */

    /**
     * @brief Default constructor.
     */
    directory_watcher() = default;

    /**
     * @brief Destructor.
     */
    ~directory_watcher() = default;

    /**
     * @brief Starts watching a directory for attribute changes.
     * @param fpath Path to the directory to watch.
     * @param ts Polling interval in seconds for checking changes. Default is 0.1 seconds.
     * @details Initializes and starts the underlying `ev::stat` watcher for the specified directory path.
     *          The `on(event::file&)` handler will be called when changes to the directory's attributes are detected.
     */
    void
    start(std::string const &fpath, qb::duration interval = std::chrono::milliseconds(100)) noexcept {
        this->_async_event.start(fpath.c_str(), qb::detail::to_ev_seconds(interval));
    }

    /**
     * @brief Stops watching the directory.
     * @details Stops the underlying `ev::stat` watcher. No more file events for this directory will be generated.
     */
    void
    disconnect() noexcept {
        this->_async_event.stop();
    }

private:
    friend class listener::RegisteredKernelEvent<event::file, directory_watcher>;

    /**
     * @brief Internal directory event handler, called by the listener when `event::file` (for the directory) triggers.
     * @param event The `event::file` containing current and previous `stat` data for the directory.
     * @details This method primarily forwards the `event::file` to the `_Derived::on(event::file&)`
     *          handler if the derived class has implemented it. It does not attempt to read directory contents itself,
     *          as `_Derived::do_read` is false for `directory_watcher`.
     *          Any further action based on directory attribute changes is the responsibility of the derived class.
     */
    void
    on(event::file const &event) {
        // forward event to Derived if desired
        if constexpr (qb::has_on<_Derived, event::file>) {
            Derived.on(event);
        }
    }
};

/**
 * @class input
 * @ingroup Async
 * @brief CRTP base class for managing asynchronous input operations with protocol processing.
 *
 * This template class provides functionality for asynchronous input. It uses an `event::io`
 * watcher to monitor a file descriptor for read readiness. Data read from the transport
 * (provided by `_Derived::transport()`) is processed using an `AProtocol` instance.
 *
 * @note **Thread Safety:** This class and all its derived instances are designed for
 *       single-threaded use within a VirtualCore. All methods, including event handlers,
 *       are called sequentially in the same thread. No synchronization is required for
 *       internal state (`_protocol`, `_on_message`, `_is_disposed`, etc.) as long as
 *       the object is not shared between threads.
 *
 * @tparam _Derived The derived class type (CRTP pattern) that provides the transport
 *                  (e.g., a socket wrapper) and handles protocol messages and I/O events.
 */
template <typename _Derived>
class input : public base<input<_Derived>, event::io> {
    using base_t                                      = base<input<_Derived>, event::io>;
    IProtocol                              *_protocol = nullptr; /**< Active protocol (non-owning view into _protocol_list). */
    std::vector<std::unique_ptr<IProtocol>> _protocol_list;      /**< Owned protocol instances (RAII). */
    bool        _on_message         = false; /**< Internal flag to prevent re-entrant calls to `on(event::io&)` during message processing. */
    bool        _is_disposed        = false; /**< Internal flag to ensure `dispose()` is called only once. */
    int         _reason             = 0;     /**< Stores the reason for disconnection if initiated by `disconnect()`. */
    int         _system_error       = 0;     /**< Stores the system error code (errno) if an I/O error occurred. */
    std::size_t _max_message_size   = QB_MAX_MESSAGE_SIZE; /**< Maximum allowed message size for DoS protection. Configurable at runtime. */
    std::size_t _bytes_read         = 0;                   /**< Total number of bytes read from the transport. */
    std::size_t _messages_processed = 0;                   /**< Total number of messages successfully processed. */

public:
    using base_io_t                        = input<_Derived>; /**< Base I/O type alias for CRTP. */
    constexpr static const bool has_server = false;           /**< Indicates this component is not inherently a server (e.g., an acceptor). */

    /**
     * @brief Default constructor.
     * Initializes the input component without a specific protocol.
     * A protocol should be set using `switch_protocol()` before starting operations that require one.
     */
    input() = default;

    /**
     * @brief Constructor with an initial protocol instance.
     * @param protocol Pointer to an existing protocol instance. This `input` component will use it and
     *                 take ownership by adding it to its internal list of protocols for cleanup.
     * @note If the protocol is managed externally, consider setting it via `set_protocol_no_ownership` (if such a method existed)
     *       or ensure `clear_protocols` is not called if it would delete an externally managed protocol.
     *       Currently, this constructor implies ownership.
     */
    input(IProtocol *protocol)
        : _protocol(protocol) {
        _protocol_list.push_back(std::unique_ptr<IProtocol>(protocol));
    }

    /**
     * @brief Deleted copy constructor to prevent unintended copying of I/O state and resources.
     */
    input(input const &) = delete;

    /**
     * @brief Destructor.
     * @details RAII: `_protocol_list` (unique_ptr vector) cleans up all owned protocols.
     *          The base class destructor handles unregistering the `event::io` watcher.
     */
    ~input() noexcept = default;

    /**
     * @brief Switches to a new protocol for processing input, taking ownership of the new protocol.
     * @tparam _Protocol The concrete `AProtocol` type to instantiate.
     * @tparam _Args Argument types for the `_Protocol` constructor.
     * @param args Arguments to forward to the `_Protocol` constructor.
     * @return Pointer to the newly created and activated protocol instance if successful (protocol's `ok()` returns true),
     *         otherwise `nullptr` (and the created protocol instance is deleted).
     * @details The new protocol instance is added to an internal list and will be cleaned up by this `input` component's destructor.
     *          The current active protocol is set to this new instance. Previous protocols in the list are not deleted by this call.
     *
     * @note **Usage:** This method is typically called during initialization or when switching between different message formats.
     *       For example, you might switch from a handshake protocol to a main protocol after authentication.
     *
     * @note **Error Handling:** If the protocol constructor throws an exception, it will propagate to the caller.
     *       If `ok()` returns false, the protocol is immediately deleted and `nullptr` is returned.
     *
     * @note **Memory Management:** The protocol instance is owned by this component and will be deleted when
     *       `clear_protocols()` is called or when the component is destroyed.
     */
    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&...args) {
        auto up = std::make_unique<_Protocol>(std::forward<_Args>(args)...);
        if (up->ok()) {
            auto *raw = up.get();
            _protocol_list.push_back(std::move(up));
            _protocol = raw;
            return raw;
        }
        return nullptr;
    }

    /**
     * @brief Clears all owned protocol instances.
     * @details Clears `_protocol_list` (unique_ptrs auto-delete) and resets `_protocol` to nullptr.
     */
    void
    clear_protocols() {
        _protocol_list.clear();
        _protocol_list.shrink_to_fit();
        _protocol = nullptr;
    }

    [[nodiscard]] IProtocol *
    protocol() noexcept {
        return _protocol;
    }

    [[nodiscard]] IProtocol const *
    protocol() const noexcept {
        return _protocol;
    }

    /**
     * @brief Starts asynchronous input operations.
     * @details Sets the underlying transport (obtained via `_Derived::transport()`) to non-blocking mode
     *          and starts the `event::io` watcher to listen for read events (`EV_READ`).
     *          Resets any previous disconnection reason (`_reason = 0`) and system error (`_system_error = 0`).
     *
     * @note **Usage:** This method should be called after setting up the transport (e.g., after `connect()` or `accept()`)
     *       and optionally setting a protocol via `switch_protocol()`. Once started, the component will automatically
     *       read data from the transport and process messages through the active protocol.
     *
     * @note **Actor Integration:** When used within a `qb::Actor`, this is typically called in `onInit()` or after
     *       establishing a connection. The component will then automatically trigger `on(event::io&)` events
     *       which are handled by the derived class's event handlers.
     *
     * @note **Example Usage:**
     * @code
     * class MyClient : public qb::Actor, public qb::io::use<MyClient>::tcp::client<> {
     * public:
     *   bool onInit() override {
     *     // Set up protocol before starting
     *     this->template switch_protocol<MyProtocol>(*this);
     *
     *     // Connect to server
     *     if (this->transport().connect_v4("127.0.0.1", 8080) < 0) {
     *       return false; // Connection failed
     *     }
     *
     *     // Start reading data
     *     this->start();
     *     return true;
     *   }
     *
     *   void on(MyProtocol::message &&msg) {
     *     // Handle incoming messages
     *   }
     * };
     * @endcode
     */
    void
    start() noexcept {
        _is_disposed  = false;
        _on_message   = false;
        _reason       = 0;
        _system_error = 0;
        Derived.transport().set_nonblocking(true);
        this->_async_event.start(Derived.transport().native_handle(), EV_READ);
    }

    /**
     * @brief Ensures the I/O watcher is listening for read events (`EV_READ`).
     * @details If the internal `event::io` watcher (from the `base` class) is not currently set to listen for `EV_READ`,
     *          this method reconfigures and restarts it to include `EV_READ` in its watched events.
     *          This is useful if read operations were temporarily paused.
     */
    void
    ready_to_read() noexcept {
        if (!(this->_async_event.events & EV_READ))
            this->_async_event.start(Derived.transport().native_handle(), EV_READ);
    }

    /**
     * @brief Checks if the input component is currently listening for read events.
     * @return true if `EV_READ` is set in the event watcher, false otherwise.
     *
     * @note **Usage:** This method can be used to check the current state of the input component
     *       without modifying it. It's useful for debugging, logging, or conditional logic.
     *
     * @note **Example Usage:**
     * @code
     * if (this->is_reading()) {
     *   LOG_DEBUG("Input component is actively reading");
     * } else {
     *   LOG_DEBUG("Input component is not reading (may be stopped or disconnected)");
     * }
     * @endcode
     */
    [[nodiscard]] bool
    is_reading() const noexcept {
        return (this->_async_event.events & EV_READ) != 0;
    }

    /**
     * @brief Checks if the input component is connected and ready for operations.
     * @return true if not disposed and no disconnection reason is set, false otherwise.
     *
     * @note **Usage:** This method indicates whether the component is in a valid operational state.
     *       A component is considered connected if it hasn't been disposed and no disconnection
     *       has been initiated (either explicitly via `disconnect()` or due to an error).
     *
     * @note **Example Usage:**
     * @code
     * if (this->is_connected()) {
     *   // Safe to send data or perform operations
     *   this->switch_protocol<MyProtocol>(*this);
     * } else {
     *   LOG_WARN("Component is disconnected, cannot perform operations");
     * }
     * @endcode
     *
     * @note This method does not check the underlying transport state (e.g., socket validity).
     *       It only checks the internal state flags. The component may still be in the process
     *       of disconnecting even if this returns true momentarily.
     */
    [[nodiscard]] bool
    is_connected() const noexcept {
        return !_is_disposed && _reason == 0;
    }

    /**
     * @brief Checks if there is pending data to be read from the input buffer.
     * @return true if the protocol indicates there is data available in the input buffer, false otherwise.
     *
     * @note **Usage:** This method checks if there is unprocessed data in the input buffer that
     *       the protocol has not yet consumed. This is useful for determining if more data needs
     *       to be read or if the buffer is empty.
     *
     * @note **Example Usage:**
     * @code
     * if (this->has_pending_data()) {
     *   LOG_DEBUG("Input buffer contains " << this->pendingRead() << " bytes of unprocessed data");
     *   // The protocol may need more data to complete a message, or there may be multiple messages
     * } else {
     *   LOG_DEBUG("Input buffer is empty or all data has been processed");
     * }
     * @endcode
     *
     * @note This method requires a valid protocol (`_protocol != nullptr` and `_protocol->ok() == true`).
     *       If no protocol is set or the protocol is invalid, this method returns false.
     */
    [[nodiscard]] bool
    has_pending_data() const noexcept {
        return _protocol && _protocol->ok() && static_cast<_Derived const &>(*this).pendingRead() > 0;
    }

    /**
     * @brief Gets the maximum allowed message size for DoS protection.
     * @return The current maximum message size in bytes.
     *
     * @note **Usage:** This method returns the configured maximum message size limit.
     *       Messages exceeding this size will cause the protocol to be marked as invalid
     *       and trigger a disconnection with `reason = -2` (Message too large).
     *
     * @note **Default:** The default value is `QB_MAX_MESSAGE_SIZE` (10MB by default).
     *       This can be changed at runtime via `set_max_message_size()`.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t current_limit = this->max_message_size();
     * LOG_INFO("Current message size limit: " << current_limit << " bytes");
     * @endcode
     */
    [[nodiscard]] std::size_t
    max_message_size() const noexcept {
        return _max_message_size;
    }

    /**
     * @brief Sets the maximum allowed message size for DoS protection.
     * @param size Maximum message size in bytes. Must be greater than 0.
     *
     * @note **Usage:** This method allows configuring the maximum message size limit at runtime.
     *       Messages exceeding this size will cause the protocol to be marked as invalid
     *       and trigger a disconnection with `reason = -2` (Message too large).
     *
     * @note **Security:** Setting a very large value may expose the application to DoS attacks
     *       via oversized messages. Setting it too small may cause legitimate large messages
     *       to be rejected. Choose a value appropriate for your application's needs.
     *
     * @note **Example Usage:**
     * @code
     * // Set a custom limit of 5MB
     * this->set_max_message_size(5 * 1024 * 1024);
     *
     * // Or use a smaller limit for a specific use case
     * this->set_max_message_size(1024 * 1024); // 1MB
     * @endcode
     */
    void
    set_max_message_size(std::size_t size) noexcept {
        _max_message_size = size;
    }

    /**
     * @brief Gets the disconnection reason code.
     * @return The reason code for disconnection. Returns `0` if no disconnection has been initiated.
     *
     * @note **Usage:** This method returns the reason code that was set via `disconnect()` or
     *       automatically set by the framework when an error occurs. Common values:
     *       - `0`: No disconnection initiated (normal state)
     *       - `1`: User-initiated disconnect
     *       - `> 0`: Reserved for application-specific use (e.g., `qbm-http` uses 0-5)
     *       - `-1`: Protocol error (automatic, from qb-io)
     *       - `-2`: Message too large (DoS protection, automatic)
     *       - `-3`: Buffer size limit exceeded (DoS protection, automatic)
     *
     * @note **Example Usage:**
     * @code
     * if (this->disconnection_reason() != 0) {
     *   LOG_WARN("Disconnection reason: " << this->disconnection_reason());
     * }
     * @endcode
     */
    [[nodiscard]] int
    disconnection_reason() const noexcept {
        return _reason;
    }

    /**
     * @brief Gets the system error code from the last I/O operation.
     * @return The system error code (errno) if an I/O error occurred, `0` otherwise.
     *
     * @note **Usage:** This method returns the system error code that was captured during
     *       the last I/O error. This is useful for detailed error diagnostics and logging.
     *       The error code can be converted to a human-readable message using `std::system_category().message()`.
     *
     * @note **Example Usage:**
     * @code
     * if (this->system_error() != 0) {
     *   std::error_code ec(this->system_error(), std::system_category());
     *   LOG_ERROR("System error: " << ec.message());
     * }
     * @endcode
     */
    [[nodiscard]] int
    system_error() const noexcept {
        return _system_error;
    }

    /**
     * @brief Stops I/O operations without triggering disconnection cleanup.
     * @details Stops the event watcher and pauses all I/O operations, but does not call `dispose()`
     *          or trigger `event::disconnected`. This allows the component to be restarted later
     *          via `start()` without going through the full disconnection/reconnection cycle.
     *
     * @note **Usage:** This method is useful for temporarily pausing I/O operations (e.g., during
     *       maintenance, rate limiting, or when waiting for external conditions). Unlike `disconnect()`,
     *       this does not mark the component as disposed, so it can be resumed by calling `start()` again.
     *
     * @note **Example Usage:**
     * @code
     * // Temporarily pause reading
     * this->stop();
     *
     * // Later, resume operations
     * this->start();
     * @endcode
     *
     * @note **Difference from `disconnect()`:** `disconnect()` initiates a full cleanup cycle
     *       and triggers `event::disconnected`, while `stop()` only pauses operations and can be resumed.
     */
    void
    stop() noexcept {
        this->_async_event.stop();
    }

    /**
     * @brief Gets the total number of bytes read from the transport.
     * @return The cumulative count of bytes read since the component was created.
     *
     * @note **Usage:** This counter is incremented each time data is successfully read from the transport.
     *       It's useful for monitoring, statistics, and debugging purposes.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t total_read = this->bytes_read();
     * LOG_INFO("Total bytes read: " << total_read);
     * @endcode
     */
    [[nodiscard]] std::size_t
    bytes_read() const noexcept {
        return _bytes_read;
    }

    /**
     * @brief Gets the total number of messages successfully processed.
     * @return The cumulative count of messages that have been successfully parsed and processed.
     *
     * @note **Usage:** This counter is incremented each time a complete message is successfully
     *       processed by the protocol. It's useful for monitoring message throughput and statistics.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t msg_count = this->messages_processed();
     * LOG_INFO("Messages processed: " << msg_count);
     * @endcode
     */
    [[nodiscard]] std::size_t
    messages_processed() const noexcept {
        return _messages_processed;
    }

    /**
     * @brief Initiates a graceful disconnection of the input component.
     * @param reason An optional integer code indicating the reason for disconnection. Common values:
     *               - `0`: Normal shutdown (peer closed connection)
     *               - `1`: User-initiated disconnect (default)
     *               - `> 0`: Reserved for application-specific use (e.g., `qbm-http` uses 0-5)
     *               - `-1`: Protocol error (automatic, from qb-io)
     *               - `-2`: Message too large (DoS protection, automatic)
     *               - `-3`: Buffer size limit exceeded (DoS protection, automatic)
     *               This value will be passed in the `event::disconnected` if that event is handled by the derived class.
     * @details Sets an internal flag (`_reason`) with the provided reason and feeds an `EV_UNDEF` event to the listener.
     *          This typically causes the `on(event::io&)` handler to enter its error path during the next event loop cycle,
     *          leading to the invocation of the `dispose()` method for cleanup.
     *
     * @note **Actor Integration:** When used within a `qb::Actor`, calling `disconnect()` will trigger
     *       `on(event::disconnected&)` if implemented, allowing the actor to handle the disconnection
     *       gracefully (e.g., attempt reconnection, notify other actors, or call `kill()`).
     *
     * @note **Example Usage:**
     * @code
     * class MyClient : public qb::Actor, public qb::io::use<MyClient>::tcp::client<> {
     * public:
     *   void on(MyProtocol::message &&msg) {
     *     if (msg.type == "quit") {
     *       // Gracefully disconnect with reason code 0 (normal shutdown)
     *       this->disconnect(0);
     *     }
     *   }
     *
     *   void on(qb::io::async::event::disconnected const& event) {
     *     if (event.reason == 0) {
     *       LOG_INFO("Disconnected normally");
     *     } else {
     *       LOG_WARN("Disconnected with reason: " << event.reason);
     *     }
     *     // Optionally attempt reconnection or call kill()
     *   }
     * };
     * @endcode
     *
     * @note This method is safe to call multiple times; subsequent calls will update the reason code
     *       but the disconnection process will only occur once.
     *
     * @note **Reason `0` caveat.** Internally, `_reason == 0` is the sentinel meaning
     *       "no disconnect pending". An explicit user call of `disconnect(0)` is therefore
     *       remapped to `disconnect_reason::user_initiated` (`1`), which is semantically
     *       closer to what the caller meant (the `0` / `peer_closed` code is generated
     *       automatically when the kernel reports EOF on the socket). Use the
     *       `disconnect(disconnect_reason)` overload for the strongly-typed path.
     */
    void
    disconnect(int reason = 1) noexcept {
        _reason = reason ? reason : static_cast<int>(event::disconnect_reason::user_initiated);
        this->_async_event.feed_event(EV_UNDEF);
    }

    /**
     * @brief Strongly-typed overload accepting a `disconnect_reason` enum value.
     * @param reason Typed reason code. Equivalent to calling the `int` overload with
     *               `static_cast<int>(reason)`, with the same `peer_closed`→`user_initiated`
     *               remap when the caller explicitly chose `peer_closed`.
     */
    void
    disconnect(event::disconnect_reason reason) noexcept {
        disconnect(static_cast<int>(reason));
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, input>;

    /**
     * @brief Processes messages from the input buffer using the protocol.
     * @return true if processing succeeded, false if an error occurred.
     * @details Optimized hot path: caches protocol pointer to reduce member access overhead.
     */
    bool
    process_messages() noexcept {
        std::size_t ret = 0u;

        _on_message = true;
        while ((ret = this->_protocol->getMessageSize()) > 0) {
            const auto frame_exceeds_pending = [&]() noexcept {
                if (!this->_protocol->should_flush())
                    return false;
                if constexpr (requires {
                                  Derived.in();
                                  Derived.pendingRead();
                              })
                    return ret > Derived.pendingRead();
                else
                    return false;
            }();
            // Security check: prevent DoS via oversized messages
            if (unlikely(ret > _max_message_size || frame_exceeds_pending)) {
                this->_protocol->not_ok();
                _system_error = 0;
                _reason       = -2; // Message too large (DoS protection)
                _on_message   = false;
                return false;
            }
            // Capture protocol pointer before onMessage() — onMessage() may call
            // switch_protocol() internally (e.g. handshake → HTTP/2 upgrade).
            // The OLD protocol's should_flush() must be used for flush(), not the new one.
            // Snapshot should_flush() here as well: onMessage() may not only switch the
            // protocol but also drop the old one from the protocol list (clear_protocols()),
            // which would leave `protocol` dangling. should_flush() is a per-protocol
            // invariant (no setter is ever called after construction), so reading it now
            // is equivalent to reading it after and removes every post-onMessage deref.
            auto      *protocol         = this->_protocol;
            const bool old_should_flush = protocol->should_flush();
            protocol->onMessage(ret);
            // Update statistics: message successfully processed
            ++_messages_processed;
            if (unlikely(_reason)) {
                if (likely(old_should_flush))
                    Derived.flush(ret);
                _on_message = false;
                return false;
            }
            // Check if the CURRENT (potentially new) protocol became invalid
            if (unlikely(!this->_protocol->ok())) {
                _system_error = 0;
                _reason       = -1; // Protocol error
                _on_message   = false;
                return false;
            }
            // Use the OLD protocol's should_flush() to preserve protocol-switching semantics
            if (likely(old_should_flush))
                Derived.flush(ret);
        }
        _on_message = false;
        return true;
    }

    /**
     * @brief Handles post-read processing (eof, pending_read events).
     */
    void
    handle_post_read() {
        if constexpr (qb::has_on<_Derived, event::pending_read> || qb::has_on<_Derived, event::eof>) {
            const auto pendingRead = Derived.pendingRead();
            if (pendingRead) {
                if constexpr (qb::has_on<_Derived, event::pending_read>) {
                    auto evt__pending_read = event::pending_read{pendingRead};
                    Derived.on(std::move(evt__pending_read));
                }
            } else {
                if constexpr (qb::has_on<_Derived, event::eof>) {
                    auto evt__eof = event::eof{};
                    Derived.on(std::move(evt__eof));
                }
            }
        }
    }

    /**
     * @brief Internal I/O event handler, called by the listener when `event::io` triggers for read readiness.
     * @param event The `event::io` that was triggered. `event._revents` contains the actual events (e.g., `EV_READ`).
     * @details
     * This is the core logic for handling incoming data.
     * If not currently processing a message (`_on_message` is false to prevent re-entrance), no disconnection is pending (`_reason` is 0),
     * and the protocol is valid (`_protocol->ok()`):
     * 1. If `EV_READ` is set in `event._revents`, it attempts to read data from `_Derived::read()` into the input buffer.
     * 2. If the read is successful (returns >= 0 bytes), it processes messages via `process_messages()`.
     * 3. After processing, it calls `_Derived::eof()` and handles pending_read/eof events.
     * If any OS-level read error occurs (read returns < 0) or if `_reason` is set (due to `disconnect()` call), it calls `dispose()`.
     */
    void
    on(event::io const &event) {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        // Keep a strong reference to prevent UAF for the entire duration of
        // this handler:
        //   - When process_messages() triggers extractSession() (e.g. WebSocket
        //     upgrade), the io_handler drops the last shared_ptr to *this.
        //   - When dispose() invokes Derived.on(event::disconnected) and the
        //     user handler decides to release the last shared_ptr (typical
        //     "self-managing" actor pattern), *this would be destroyed before
        //     dispose() reaches `_async_event.stop()` further down the call
        //     stack.
        // The guard MUST be acquired before any branch that can reach
        // dispose() — including the goto-error path triggered by an external
        // disconnect() — not only the EV_READ branch.
        // Stored as shared_ptr<void> so it works when _Derived inherits from
        // enable_shared_from_this<Base> where Base != _Derived (e.g. HTTP CRTP sessions).
        // Declaration is at function scope (above all goto targets) to satisfy
        // C++ goto-past-initialization rules.
        std::shared_ptr<void> _self_guard;

        if (_on_message)
            return;

        if constexpr (qb::has_shared_from_this<_Derived>) {
            try {
                _self_guard = Derived.shared_from_this();
            } catch (std::bad_weak_ptr const &) {
            }
        } else if constexpr (requires { Derived.shared(); }) {
            _self_guard = Derived.shared();
        }

        if (_reason || !_protocol || !_protocol->ok()) {
            // Protocol error: capture it before disposing
            if (_protocol && !_protocol->ok()) {
                _system_error = 0;  // Protocol error, not system error
                _reason       = -1; // Use reason code -1 for protocol errors (reserved for qb-io)
            }
            goto error;
        }

        if (likely(event._revents & EV_READ)) {
            auto ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret)) {
                if (qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
                    return;
                goto error;
            }

            // Check for buffer size limit exceeded (DoS protection)
            if (unlikely(ret == static_cast<std::size_t>(-2))) {
                _system_error = 0;
                _reason       = -3; // Buffer size limit exceeded (DoS protection)
                goto error;
            }

            // Update statistics
            _bytes_read += ret;

            if (!process_messages())
                goto error;

            Derived.eof();
            handle_post_read();
            return;
        }
    error:
#ifdef _WIN32
        // Ignore a spurious would-block only when no explicit reason is pending.
        // When _reason is set (user disconnect(), protocol error, DoS guard), the
        // last WSA error is unrelated/stale — letting it suppress dispose() would
        // silently drop the disconnection and leave a zombie session.
        if (!_reason && qb::io::socket::get_last_errno() == QB_WINDOWS_WOULDBLOCK_ERROR)
            return;
#endif
        if (!_reason)
            _system_error = qb::io::socket::get_last_errno();
        dispose();
    }

protected:
    /**
     * @brief Disposes of resources and finalizes disconnection for the input component.
     * @details This method is called internally when an I/O error occurs or when `disconnect()`
     *          is explicitly initiated. It ensures cleanup happens only once by checking the `_is_disposed` flag.
     *          If `_Derived` implements `on(event::disconnected&)`, this method is called with the stored `_reason`.
     *          If `_Derived::has_server` is true (typically for server-side sessions), it notifies the server of the disconnection.
     *          Otherwise, if `_Derived` implements `on(event::dispose&)`, that method is called as a final cleanup hook.
     *          The base class (`async::base`) destructor will handle unregistering the `event::io` watcher.
     * @note **Actor Lifecycle Integration:** When used within a `qb::Actor`, this method is called
     *       during the I/O component's cleanup phase. Actors should handle `event::disconnected` to
     *       perform cleanup and potentially call `kill()` if the connection loss requires actor termination.
     *       The `event::dispose` hook is useful for final resource cleanup before the I/O component is destroyed.
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;

        if constexpr (qb::has_on<_Derived, event::disconnected>) {
            if (_system_error != 0) {
                auto evt = event::disconnected::with_error(_reason, _system_error);
                Derived.on(std::move(evt));
            } else {
                auto evt = event::disconnected{_reason};
                Derived.on(std::move(evt));
            }
        }

        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else {
            this->_async_event.stop();
            if constexpr (qb::has_on<_Derived, event::dispose>) {
                auto evt__dispose = event::dispose{};
                Derived.on(std::move(evt__dispose));
            }
        }
    }

    /**
     * @brief Resets internal disconnection state so the component can be reused.
     *
     * After `dispose()` sets `_is_disposed = true`, subsequent calls to `dispose()`
     * are no-ops, preventing `on(disconnected)` from firing on the next connection drop.
     * Call this before restarting I/O on the same object (e.g. for auto-reconnect) to
     * restore a clean slate identical to the freshly-constructed state.
     *
     * @pre The old async event must be stopped and the old transport socket closed
     *      before calling this — typically done from within the reconnect setup path.
     */
    void
    reset_io_state() noexcept {
        _is_disposed  = false;
        _on_message   = false;
        _reason       = 0;
        _system_error = 0;
    }
};

/**
 * @class output
 * @ingroup Async
 * @brief CRTP base class for managing asynchronous output operations.
 *
 * This template class provides functionality for asynchronous output. It uses an `event::io`
 * watcher to monitor a file descriptor for write readiness. Data to be sent is buffered
 * and then written to the transport (provided by `_Derived::transport()`) when ready.
 *
 * @note **Thread Safety:** This class and all its derived instances are designed for
 *       single-threaded use within a VirtualCore. All methods are called sequentially
 *       in the same thread. No synchronization is required for internal state as long
 *       as the object is not shared between threads.
 *
 * @tparam _Derived The derived class type (CRTP pattern) that provides the transport
 *                  and handles I/O events like `eos` (end of stream) or `pending_write`.
 */
template <typename _Derived>
class output : public base<output<_Derived>, event::io> {
    using base_t               = base<output<_Derived>, event::io>;
    bool        _is_disposed   = false; /**< Internal flag to ensure `dispose()` is called only once. */
    int         _reason        = 0;     /**< Stores the reason for disconnection if initiated by `disconnect()`. */
    int         _system_error  = 0;     /**< Stores the system error code (errno) if an I/O error occurred. */
    std::size_t _bytes_written = 0;     /**< Total number of bytes written to the transport. */

public:
    using base_io_t                        = output<_Derived>; /**< Base I/O type alias for CRTP. */
    constexpr static const bool has_server = false;            /**< Indicates this component is not inherently a server. */

    /**
     * @brief Default constructor.
     */
    output() = default;

    /**
     * @brief Deleted copy constructor to prevent unintended copying of I/O state and resources.
     */
    output(output const &) = delete;

    /**
     * @brief Destructor.
     * @details The base class destructor will handle unregistering the `event::io` watcher.
     */
    ~output() = default;

    /**
     * @brief Starts asynchronous output operations.
     * @details Sets the underlying transport (obtained via `_Derived::transport()`) to non-blocking mode
     *          and starts the `event::io` watcher to listen for write events (`EV_WRITE`).
     *          Resets any previous disconnection reason (`_reason = 0`) and system error (`_system_error = 0`).
     *
     * @note **Usage:** This method should be called after setting up the transport (e.g., after `connect()` or `accept()`).
     *       Once started, the component will automatically write buffered data (added via `publish()` or `operator<<`)
     *       to the transport when it becomes writable.
     *
     * @note **Actor Integration:** When used within a `qb::Actor`, this is typically called in `onInit()` or after
     *       establishing a connection. The component will then automatically trigger `on(event::io&)` events
     *       for write readiness, which are handled by the derived class's event handlers.
     *
     * @note **Example Usage:**
     * @code
     * class MyClient : public qb::Actor, public qb::io::use<MyClient>::tcp::client<> {
     * public:
     *   bool onInit() override {
     *     // Connect to server
     *     if (this->transport().connect_v4("127.0.0.1", 8080) < 0) {
     *       return false;
     *     }
     *
     *     // Start writing data
     *     this->start();
     *
     *     // Send initial message
     *     *this << "Hello, server!" << Protocol::end;
     *     return true;
     *   }
     * };
     * @endcode
     */
    void
    start() noexcept {
        _is_disposed  = false;
        _reason       = 0;
        _system_error = 0;
        Derived.transport().set_nonblocking(true);
        this->_async_event.start(Derived.transport().native_handle(), EV_WRITE);
    }

    /**
     * @brief Ensures the I/O watcher is listening for write events (`EV_WRITE`).
     * @details If the internal `event::io` watcher (from the `base` class) is not currently set to listen for `EV_WRITE`,
     *          this method reconfigures and restarts it to include `EV_WRITE` in its watched events.
     */
    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE))
            this->_async_event.start(Derived.transport().native_handle(), this->_async_event.events | EV_WRITE);
    }

    /**
     * @brief Checks if the output component is currently listening for write events.
     * @return true if `EV_WRITE` is set in the event watcher, false otherwise.
     *
     * @note **Usage:** This method can be used to check the current state of the output component
     *       without modifying it. It's useful for debugging, logging, or conditional logic.
     *
     * @note **Example Usage:**
     * @code
     * if (this->is_writing()) {
     *   LOG_DEBUG("Output component is actively writing");
     * } else {
     *   LOG_DEBUG("Output component is not writing (may be stopped or disconnected)");
     * }
     * @endcode
     */
    [[nodiscard]] bool
    is_writing() const noexcept {
        return (this->_async_event.events & EV_WRITE) != 0;
    }

    /**
     * @brief Checks if the output component is connected and ready for operations.
     * @return true if not disposed and no disconnection reason is set, false otherwise.
     *
     * @note **Usage:** This method indicates whether the component is in a valid operational state.
     *       A component is considered connected if it hasn't been disposed and no disconnection
     *       has been initiated (either explicitly via `disconnect()` or due to an error).
     *
     * @note **Example Usage:**
     * @code
     * if (this->is_connected()) {
     *   // Safe to publish data
     *   *this << "Hello, server!" << Protocol::end;
     * } else {
     *   LOG_WARN("Component is disconnected, cannot send data");
     * }
     * @endcode
     *
     * @note This method does not check the underlying transport state (e.g., socket validity).
     *       It only checks the internal state flags. The component may still be in the process
     *       of disconnecting even if this returns true momentarily.
     */
    [[nodiscard]] bool
    is_connected() const noexcept {
        return !_is_disposed && _reason == 0;
    }

    /**
     * @brief Checks if there is pending data to be written to the output buffer.
     * @return true if there is data in the output buffer waiting to be sent, false otherwise.
     *
     * @note **Usage:** This method checks if there is buffered data that has not yet been written
     *       to the underlying transport. This is useful for determining if the output buffer
     *       is empty or if there is data waiting to be flushed.
     *
     * @note **Example Usage:**
     * @code
     * *this << "Message 1" << Protocol::end;
     * *this << "Message 2" << Protocol::end;
     *
     * if (this->has_pending_data()) {
     *   LOG_DEBUG("Output buffer contains " << this->pendingWrite() << " bytes waiting to be sent");
     *   // The data will be written automatically when the transport becomes writable
     * } else {
     *   LOG_DEBUG("All data has been written to the transport");
     * }
     * @endcode
     */
    [[nodiscard]] bool
    has_pending_data() const noexcept {
        return static_cast<_Derived const &>(*this).pendingWrite() > 0;
    }

    /**
     * @brief Gets the disconnection reason code.
     * @return The reason code for disconnection. Returns `0` if no disconnection has been initiated.
     *
     * @note **Usage:** This method returns the reason code that was set via `disconnect()` or
     *       automatically set by the framework when an error occurs. Common values:
     *       - `0`: No disconnection initiated (normal state)
     *       - `1`: User-initiated disconnect
     *       - `> 0`: Reserved for application-specific use (e.g., `qbm-http` uses 0-5)
     *       - `-1`: Protocol error (automatic, from qb-io)
     *       - `-2`: Message too large (DoS protection, automatic)
     *       - `-3`: Buffer size limit exceeded (DoS protection, automatic)
     *
     * @note **Example Usage:**
     * @code
     * if (this->disconnection_reason() != 0) {
     *   LOG_WARN("Disconnection reason: " << this->disconnection_reason());
     * }
     * @endcode
     */
    [[nodiscard]] int
    disconnection_reason() const noexcept {
        return _reason;
    }

    /**
     * @brief Gets the system error code from the last I/O operation.
     * @return The system error code (errno) if an I/O error occurred, `0` otherwise.
     *
     * @note **Usage:** This method returns the system error code that was captured during
     *       the last I/O error. This is useful for detailed error diagnostics and logging.
     *       The error code can be converted to a human-readable message using `std::system_category().message()`.
     *
     * @note **Example Usage:**
     * @code
     * if (this->system_error() != 0) {
     *   std::error_code ec(this->system_error(), std::system_category());
     *   LOG_ERROR("System error: " << ec.message());
     * }
     * @endcode
     */
    [[nodiscard]] int
    system_error() const noexcept {
        return _system_error;
    }

    /**
     * @brief Stops I/O operations without triggering disconnection cleanup.
     * @details Stops the event watcher and pauses all I/O operations, but does not call `dispose()`
     *          or trigger `event::disconnected`. This allows the component to be restarted later
     *          via `start()` without going through the full disconnection/reconnection cycle.
     *
     * @note **Usage:** This method is useful for temporarily pausing I/O operations (e.g., during
     *       maintenance, rate limiting, or when waiting for external conditions). Unlike `disconnect()`,
     *       this does not mark the component as disposed, so it can be resumed by calling `start()` again.
     *
     * @note **Example Usage:**
     * @code
     * // Temporarily pause writing
     * this->stop();
     *
     * // Later, resume operations
     * this->start();
     * @endcode
     *
     * @note **Difference from `disconnect()`:** `disconnect()` initiates a full cleanup cycle
     *       and triggers `event::disconnected`, while `stop()` only pauses operations and can be resumed.
     */
    void
    stop() noexcept {
        this->_async_event.stop();
    }

    /**
     * @brief Gets the total number of bytes written to the transport.
     * @return The cumulative count of bytes written since the component was created.
     *
     * @note **Usage:** This counter is incremented each time data is successfully written to the transport.
     *       It's useful for monitoring, statistics, and debugging purposes.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t total_written = this->bytes_written();
     * LOG_INFO("Total bytes written: " << total_written);
     * @endcode
     */
    [[nodiscard]] std::size_t
    bytes_written() const noexcept {
        return _bytes_written;
    }

    /**
     * @brief Publishes data to the output buffer and ensures write readiness.
     * @tparam _Args Variadic template arguments for the data to be published.
     * @param args Data arguments to stream into `_Derived::out()` buffer (typically a `qb::allocator::pipe<char>`).
     * @return A reference to the `_Derived::out()` buffer after the data has been added.
     * @details Calls `ready_to_write()` to ensure the event loop is monitoring for write readiness,
     *          then streams all `args` into the output buffer provided by `_Derived::out()`.
     */
    template <typename... _Args>
    inline auto &
    publish(_Args &&...args) {
        if (unlikely(_is_disposed || _reason))
            return Derived.out();
        const auto max_write = Derived.max_write_buffer_size();
        if (unlikely(max_write != static_cast<std::size_t>(-1) && Derived.pendingWrite() >= max_write)) {
            _system_error = 0;
            disconnect(event::disconnect_reason::buffer_overflow);
            return Derived.out();
        }

        ready_to_write();
        if constexpr (sizeof...(_Args)) {
            const auto before = Derived.pendingWrite();
            (Derived.out() << ... << std::forward<_Args>(args));
            const auto after = Derived.pendingWrite();
            if (unlikely(max_write != static_cast<std::size_t>(-1) && after > max_write)) {
                // Best-effort rollback of the just-appended tail to keep the
                // write buffer bounded even when callers stream without
                // checking publish(char*, size) return codes.
                const auto added    = after - before;
                const auto overflow = after - max_write;
                if constexpr (requires { Derived.out().free_back(std::size_t{}); }) {
                    if (overflow <= added)
                        Derived.out().free_back(overflow);
                }
                _system_error = 0;
                disconnect(event::disconnect_reason::buffer_overflow);
            }
        }
        return Derived.out();
    }

    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    /**
     * @brief Initiates a graceful disconnection of the output component.
     * @param reason An optional integer code indicating the reason for disconnection.
     *                Defaults to `1` for user-initiated disconnection. Common values:
     *                - `0`: Normal shutdown (peer closed connection)
     *                - `1`: User-initiated disconnect
     *                - `2`: Protocol error
     *                - `3`: Message too large (DoS protection)
     * @details Sets an internal flag (`_reason`) and feeds an `EV_UNDEF` event to the listener.
     *          This typically causes the `on(event::io&)` handler to enter its error path,
     *          leading to the invocation of `dispose()` for cleanup.
     *
     * @note **Actor Integration:** When used within a `qb::Actor`, calling `disconnect()` will trigger
     *       `on(event::disconnected&)` if implemented, allowing the actor to handle the disconnection
     *       gracefully (e.g., attempt reconnection, notify other actors, or call `kill()`).
     *
     * @note This method is safe to call multiple times; subsequent calls will update the reason code
     *       but the disconnection process will only occur once.
     *
     * @note **Reason `0` caveat.** Explicit `disconnect(0)` is remapped to
     *       `disconnect_reason::user_initiated` because `_reason == 0` is the internal
     *       sentinel for "not disconnecting". Use the `disconnect(disconnect_reason)`
     *       overload for the strongly-typed path.
     */
    void
    disconnect(int reason = 1) noexcept {
        _reason = reason ? reason : static_cast<int>(event::disconnect_reason::user_initiated);
        this->_async_event.feed_event(EV_UNDEF);
    }

    /**
     * @brief Strongly-typed overload accepting a `disconnect_reason` enum value.
     */
    void
    disconnect(event::disconnect_reason reason) noexcept {
        disconnect(static_cast<int>(reason));
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, output>;

    /**
     * @brief Internal I/O event handler, called by the listener when `event::io` triggers for write readiness.
     * @param event The `event::io` that was triggered. `event._revents` indicates if `EV_WRITE` is set.
     * @details
     * This is the core logic for sending buffered data.
     * If not disconnected (`_reason` is 0) and `EV_WRITE` is set in `event._revents`:
     * 1. Attempts to write data from the output buffer via `_Derived::write()`.
     * 2. If an OS-level write error occurs (write returns < 0), calls `dispose()`.
     * 3. If all pending data is written (`_Derived::pendingWrite()` returns 0), the `event::io` watcher is
     *    typically set to `EV_NONE` (to stop listening for write readiness until more data is published),
     *    and `_Derived::on(event::eos&)` is triggered if implemented by the derived class.
     * 4. If data is still pending in the output buffer after the write attempt, `_Derived::on(event::pending_write&)`
     *    is triggered if implemented, indicating how many bytes remain.
     * If `_reason` is set (due to a `disconnect()` call) or other unhandled conditions occur, it calls `dispose()`.
     */
    void
    on(event::io const &event) {
        // Keep a strong reference for the entire handler: dispose() invokes
        // Derived.on(event::disconnected) which may release the last
        // shared_ptr to *this; without this guard, the post-on(disconnected)
        // statements in dispose() (`_async_event.stop()`, `Derived.on(dispose)`)
        // would dereference freed memory. See the equivalent guard in io::on.
        std::shared_ptr<void> _self_guard;
        if constexpr (qb::has_shared_from_this<_Derived>) {
            try {
                _self_guard = Derived.shared_from_this();
            } catch (std::bad_weak_ptr const &) {
            }
        } else if constexpr (requires { Derived.shared(); }) {
            _self_guard = Derived.shared();
        }

        if (_reason)
            goto error;

        if (likely(event._revents & EV_WRITE)) {
            {
                auto ret = Derived.write();
                if (unlikely(ret < 0)) {
                    if (qb::io::socket::not_send_error(qb::io::socket::get_last_errno())) {
                        ready_to_write();
                        return;
                    }
                    goto error;
                }
                if (likely(ret > 0))
                    _bytes_written += static_cast<std::size_t>(ret);
            }

            if (!Derived.pendingWrite()) {
                this->_async_event.set(EV_NONE);
                if constexpr (qb::has_on<_Derived, event::eos>) {
                    auto evt__eos = event::eos{};
                    Derived.on(std::move(evt__eos));
                }
            } else if constexpr (qb::has_on<_Derived, event::pending_write>) {
                auto evt__pending_write = event::pending_write{Derived.pendingWrite()};
                Derived.on(std::move(evt__pending_write));
            }
            return;
        }
    error:
#ifdef _WIN32
        // Ignore a spurious would-block only when no explicit reason is pending.
        // When _reason is set (user disconnect(), protocol error, DoS guard), the
        // last WSA error is unrelated/stale — letting it suppress dispose() would
        // silently drop the disconnection and leave a zombie session.
        if (!_reason && qb::io::socket::get_last_errno() == QB_WINDOWS_WOULDBLOCK_ERROR)
            return;
#endif
        if (!_reason)
            _system_error = qb::io::socket::get_last_errno();
        dispose();
    }

protected:
    /**
     * @brief Disposes of resources and finalizes disconnection for the output component.
     * @details This method is called internally when an I/O error occurs or when `disconnect()`
     *          is explicitly initiated. It ensures cleanup happens only once by checking `_is_disposed`.
     *          If `_Derived` implements `on(event::disconnected&)`, this method is called with the stored `_reason`.
     *          If `_Derived::has_server` is true, it notifies the server. Otherwise, if `_Derived` implements
     *          `on(event::dispose&)`, that method is called for final cleanup.
     *          The base class (`async::base`) destructor handles unregistering the `event::io` watcher.
     * @note **Actor Lifecycle Integration:** When used within a `qb::Actor`, this method is called
     *       during the I/O component's cleanup phase. Actors should handle `event::disconnected` to
     *       perform cleanup and potentially call `kill()` if the connection loss requires actor termination.
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;

        if constexpr (qb::has_on<_Derived, event::disconnected>) {
            if (_system_error != 0) {
                auto evt = event::disconnected::with_error(_reason, _system_error);
                Derived.on(std::move(evt));
            } else {
                auto evt = event::disconnected{_reason};
                Derived.on(std::move(evt));
            }
        }

        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else {
            this->_async_event.stop();
            if constexpr (qb::has_on<_Derived, event::dispose>) {
                auto evt__dispose = event::dispose{};
                Derived.on(std::move(evt__dispose));
            }
        }
    }
};

/**
 * @class io
 * @ingroup Async
 * @brief CRTP base class for managing bidirectional asynchronous I/O operations with protocol processing.
 *
 * This template class combines input and output capabilities. It uses a single `event::io`
 * watcher to monitor a file descriptor for both read and write readiness. Data is read into
 * an input buffer and processed by a protocol, while outgoing data is buffered and written
 * to the transport when ready.
 *
 * @note **Thread Safety:** This class and all its derived instances are designed for
 *       single-threaded use within a VirtualCore. All event handlers (`on(event::io&)`)
 *       are called sequentially in the same thread. The `_on_message` flag provides
 *       re-entrance protection within the same thread context, not cross-thread protection.
 *       Objects must not be shared between threads.
 *
 * @tparam _Derived The derived class type (CRTP pattern) that provides the transport
 *                  and handles protocol messages and I/O events.
 */
template <typename _Derived>
class io : public base<io<_Derived>, event::io> {
    using base_t                                      = base<io<_Derived>, event::io>;
    IProtocol                              *_protocol = nullptr;   /**< Active protocol (non-owning view into _protocol_list). */
    std::vector<std::unique_ptr<IProtocol>> _protocol_list;        /**< Owned protocol instances (RAII). */
    bool                                    _on_message   = false; /**< Internal flag for re-entrance protection in `on(event::io&)`. */
    bool                                    _is_disposed  = false; /**< Internal flag for `dispose()` idempotency. */
    int                                     _reason       = 0;     /**< Disconnection reason code. */
    int                                     _system_error = 0;     /**< Stores the system error code (errno) if an I/O error occurred. */
    std::size_t _max_message_size   = QB_MAX_MESSAGE_SIZE; /**< Maximum allowed message size for DoS protection. Configurable at runtime. */
    std::size_t _bytes_read         = 0;                   /**< Total number of bytes read from the transport. */
    std::size_t _bytes_written      = 0;                   /**< Total number of bytes written to the transport. */
    std::size_t _messages_processed = 0;                   /**< Total number of messages successfully processed. */

public:
    typedef io<_Derived>        base_io_t;          /**< Base I/O type alias for CRTP. */
    constexpr static const bool has_server = false; /**< Indicates this component is not inherently a server. */

    /**
     * @brief Default constructor.
     * Initializes the I/O component without a specific protocol.
     */
    io() = default;

    /**
     * @brief Constructor with an initial protocol instance.
     * @param protocol Pointer to an existing protocol instance. This `io` component will use it and
     *                 take ownership by adding it to its internal list of protocols.
     */
    io(IProtocol *protocol)
        : _protocol(protocol) {
        _protocol_list.push_back(std::unique_ptr<IProtocol>(protocol));
    }

    /**
     * @brief Deleted copy constructor.
     */
    io(io const &) = delete;

    /**
     * @brief Destructor.
     * @details RAII: `_protocol_list` (unique_ptr vector) cleans up all owned protocols.
     */
    ~io() noexcept = default;

    /**
     * @brief Switches to a new protocol for I/O processing, taking ownership.
     * @tparam _Protocol The concrete `AProtocol` type to instantiate.
     * @tparam _Args Argument types for the `_Protocol` constructor.
     * @param args Arguments to forward to the `_Protocol` constructor.
     * @return Pointer to the newly created and activated protocol instance if successful (protocol's `ok()` returns true),
     *         otherwise `nullptr` (and the created protocol instance is deleted).
     * @details The new protocol instance is added to an internal list and will be cleaned up by this `io` component's destructor.
     *          The current active protocol is set to this new instance. Previous protocols in the list are not deleted by this call.
     *
     * @note **Usage:** This method is typically called during initialization or when switching between different message formats.
     *       For example, you might switch from a handshake protocol to a main protocol after authentication.
     *
     * @note **Error Handling:** If the protocol constructor throws an exception, it will propagate to the caller.
     *       If `ok()` returns false, the protocol is immediately deleted and `nullptr` is returned.
     *
     * @note **Memory Management:** The protocol instance is owned by this component and will be deleted when
     *       `clear_protocols()` is called or when the component is destroyed.
     */
    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&...args) {
        auto up = std::make_unique<_Protocol>(std::forward<_Args>(args)...);
        if (up->ok()) {
            auto *raw = up.get();
            _protocol_list.push_back(std::move(up));
            _protocol = raw;
            return raw;
        }
        return nullptr;
    }

    /**
     * @brief Clears all owned protocol instances.
     * @details Clears `_protocol_list` (unique_ptrs auto-delete) and resets `_protocol` to nullptr.
     */
    void
    clear_protocols() {
        _protocol_list.clear();
        _protocol_list.shrink_to_fit();
        _protocol = nullptr;
    }

    [[nodiscard]] IProtocol *
    protocol() noexcept {
        return _protocol;
    }

    [[nodiscard]] IProtocol const *
    protocol() const noexcept {
        return _protocol;
    }

    /**
     * @brief Starts bidirectional asynchronous I/O operations.
     * @details Sets the transport to non-blocking mode and starts listening for read events (`EV_READ`).
     *          Resets any previous disconnection reason (`_reason = 0`) and system error (`_system_error = 0`).
     *          Write events (`EV_WRITE`) are automatically enabled when data is published via `publish()` or `operator<<`.
     *
     * @note **Usage:** This method should be called after setting up the transport (e.g., after `connect()` or `accept()`)
     *       and optionally setting a protocol via `switch_protocol()`. Once started, the component will automatically
     *       read data from the transport and process messages through the active protocol, and write buffered data
     *       when the transport becomes writable.
     *
     * @note **Actor Integration:** When used within a `qb::Actor`, this is typically called in `onInit()` or after
     *       establishing a connection. The component will then automatically trigger `on(event::io&)` events
     *       for both read and write readiness, which are handled by the derived class's event handlers.
     *
     * @note **Example Usage:**
     * @code
     * class MyClient : public qb::Actor, public qb::io::use<MyClient>::tcp::client<> {
     * public:
     *   bool onInit() override {
     *     // Set up protocol before starting
     *     this->template switch_protocol<MyProtocol>(*this);
     *
     *     // Connect to server
     *     if (this->transport().connect_v4("127.0.0.1", 8080) < 0) {
     *       return false;
     *     }
     *
     *     // Start bidirectional I/O
     *     this->start();
     *
     *     // Send initial message
     *     *this << "Hello!" << Protocol::end;
     *     return true;
     *   }
     *
     *   void on(MyProtocol::message &&msg) {
     *     // Handle incoming messages
     *   }
     * };
     * @endcode
     */
    void
    start() noexcept {
        _is_disposed  = false;
        _on_message   = false;
        _reason       = 0;
        _system_error = 0;
        Derived.transport().set_nonblocking(true);
        this->_async_event.start(Derived.transport().native_handle(), EV_READ);
    }

    /**
     * @brief Ensures the I/O watcher is listening for read events (`EV_READ`).
     * @details Modifies the event watcher flags to include `EV_READ` if not already set.
     */
    void
    ready_to_read() noexcept {
        if (!(this->_async_event.events & EV_READ)) {
            this->_async_event.set(this->_async_event.events | EV_READ);
        }
    }

    /**
     * @brief Checks if the I/O component is currently listening for read events.
     * @return true if `EV_READ` is set in the event watcher, false otherwise.
     *
     * @note **Usage:** This method can be used to check the current state of the I/O component
     *       without modifying it. It's useful for debugging, logging, or conditional logic.
     *
     * @note **Example Usage:**
     * @code
     * if (this->is_reading()) {
     *   LOG_DEBUG("I/O component is actively reading");
     * } else {
     *   LOG_DEBUG("I/O component is not reading (may be stopped or disconnected)");
     * }
     * @endcode
     */
    [[nodiscard]] bool
    is_reading() const noexcept {
        return (this->_async_event.events & EV_READ) != 0;
    }

    /**
     * @brief Ensures the I/O watcher is listening for write events (`EV_WRITE`).
     * @details Modifies the event watcher flags to include `EV_WRITE` if not already set.
     * @note Despite the name suggesting a query, this method modifies state by enabling write event monitoring.
     */
    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE)) {
            this->_async_event.set(this->_async_event.events | EV_WRITE);
        }
    }

    /**
     * @brief Checks if the I/O component is currently listening for write events.
     * @return true if `EV_WRITE` is set in the event watcher, false otherwise.
     *
     * @note **Usage:** This method can be used to check the current state of the I/O component
     *       without modifying it. It's useful for debugging, logging, or conditional logic.
     *
     * @note **Example Usage:**
     * @code
     * if (this->is_writing()) {
     *   LOG_DEBUG("I/O component is actively writing");
     * } else {
     *   LOG_DEBUG("I/O component is not writing (may be stopped or disconnected)");
     * }
     * @endcode
     */
    [[nodiscard]] bool
    is_writing() const noexcept {
        return (this->_async_event.events & EV_WRITE) != 0;
    }

    /**
     * @brief Checks if the I/O component is connected and ready for operations.
     * @return true if not disposed and no disconnection reason is set, false otherwise.
     *
     * @note **Usage:** This method indicates whether the component is in a valid operational state.
     *       A component is considered connected if it hasn't been disposed and no disconnection
     *       has been initiated (either explicitly via `disconnect()` or due to an error).
     *
     * @note **Example Usage:**
     * @code
     * if (this->is_connected()) {
     *   // Safe to perform operations
     *   this->switch_protocol<MyProtocol>(*this);
     *   *this << "Hello!" << Protocol::end;
     * } else {
     *   LOG_WARN("Component is disconnected, cannot perform operations");
     * }
     * @endcode
     *
     * @note This method does not check the underlying transport state (e.g., socket validity).
     *       It only checks the internal state flags. The component may still be in the process
     *       of disconnecting even if this returns true momentarily.
     */
    [[nodiscard]] bool
    is_connected() const noexcept {
        return !_is_disposed && _reason == 0;
    }

    /**
     * @brief Checks if there is pending data to be read from the input buffer.
     * @return true if the protocol indicates there is data available in the input buffer, false otherwise.
     *
     * @note **Usage:** This method checks if there is unprocessed data in the input buffer that
     *       the protocol has not yet consumed. This is useful for determining if more data needs
     *       to be read or if the buffer is empty.
     *
     * @note **Example Usage:**
     * @code
     * if (this->has_pending_read()) {
     *   LOG_DEBUG("Input buffer contains " << this->pendingRead() << " bytes of unprocessed data");
     *   // The protocol may need more data to complete a message, or there may be multiple messages
     * } else {
     *   LOG_DEBUG("Input buffer is empty or all data has been processed");
     * }
     * @endcode
     *
     * @note This method requires a valid protocol (`_protocol != nullptr` and `_protocol->ok() == true`).
     *       If no protocol is set or the protocol is invalid, this method returns false.
     */
    [[nodiscard]] bool
    has_pending_read() const noexcept {
        return _protocol && _protocol->ok() && static_cast<_Derived const &>(*this).pendingRead() > 0;
    }

    /**
     * @brief Checks if there is pending data to be written to the output buffer.
     * @return true if there is data in the output buffer waiting to be sent, false otherwise.
     *
     * @note **Usage:** This method checks if there is buffered data that has not yet been written
     *       to the underlying transport. This is useful for determining if the output buffer
     *       is empty or if there is data waiting to be flushed.
     *
     * @note **Example Usage:**
     * @code
     * *this << "Message 1" << Protocol::end;
     * *this << "Message 2" << Protocol::end;
     *
     * if (this->has_pending_write()) {
     *   LOG_DEBUG("Output buffer contains " << this->pendingWrite() << " bytes waiting to be sent");
     *   // The data will be written automatically when the transport becomes writable
     * } else {
     *   LOG_DEBUG("All data has been written to the transport");
     * }
     * @endcode
     */
    [[nodiscard]] bool
    has_pending_write() const noexcept {
        return static_cast<_Derived const &>(*this).pendingWrite() > 0;
    }

    /**
     * @brief Gets the maximum allowed message size for DoS protection.
     * @return The current maximum message size in bytes.
     *
     * @note **Usage:** This method returns the configured maximum message size limit.
     *       Messages exceeding this size will cause the protocol to be marked as invalid
     *       and trigger a disconnection with `reason = -2` (Message too large).
     *
     * @note **Default:** The default value is `QB_MAX_MESSAGE_SIZE` (10MB by default).
     *       This can be changed at runtime via `set_max_message_size()`.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t current_limit = this->max_message_size();
     * LOG_INFO("Current message size limit: " << current_limit << " bytes");
     * @endcode
     */
    [[nodiscard]] std::size_t
    max_message_size() const noexcept {
        return _max_message_size;
    }

    /**
     * @brief Sets the maximum allowed message size for DoS protection.
     * @param size Maximum message size in bytes. Must be greater than 0.
     *
     * @note **Usage:** This method allows configuring the maximum message size limit at runtime.
     *       Messages exceeding this size will cause the protocol to be marked as invalid
     *       and trigger a disconnection with `reason = -2` (Message too large).
     *
     * @note **Security:** Setting a very large value may expose the application to DoS attacks
     *       via oversized messages. Setting it too small may cause legitimate large messages
     *       to be rejected. Choose a value appropriate for your application's needs.
     *
     * @note **Example Usage:**
     * @code
     * // Set a custom limit of 5MB
     * this->set_max_message_size(5 * 1024 * 1024);
     *
     * // Or use a smaller limit for a specific use case
     * this->set_max_message_size(1024 * 1024); // 1MB
     * @endcode
     */
    void
    set_max_message_size(std::size_t size) noexcept {
        _max_message_size = size;
    }

    /**
     * @brief Gets the disconnection reason code.
     * @return The reason code for disconnection. Returns `0` if no disconnection has been initiated.
     *
     * @note **Usage:** This method returns the reason code that was set via `disconnect()` or
     *       automatically set by the framework when an error occurs. Common values:
     *       - `0`: No disconnection initiated (normal state)
     *       - `1`: User-initiated disconnect
     *       - `> 0`: Reserved for application-specific use (e.g., `qbm-http` uses 0-5)
     *       - `-1`: Protocol error (automatic, from qb-io)
     *       - `-2`: Message too large (DoS protection, automatic)
     *       - `-3`: Buffer size limit exceeded (DoS protection, automatic)
     *
     * @note **Example Usage:**
     * @code
     * if (this->disconnection_reason() != 0) {
     *   LOG_WARN("Disconnection reason: " << this->disconnection_reason());
     * }
     * @endcode
     */
    [[nodiscard]] int
    disconnection_reason() const noexcept {
        return _reason;
    }

    /**
     * @brief Gets the system error code from the last I/O operation.
     * @return The system error code (errno) if an I/O error occurred, `0` otherwise.
     *
     * @note **Usage:** This method returns the system error code that was captured during
     *       the last I/O error. This is useful for detailed error diagnostics and logging.
     *       The error code can be converted to a human-readable message using `std::system_category().message()`.
     *
     * @note **Example Usage:**
     * @code
     * if (this->system_error() != 0) {
     *   std::error_code ec(this->system_error(), std::system_category());
     *   LOG_ERROR("System error: " << ec.message());
     * }
     * @endcode
     */
    [[nodiscard]] int
    system_error() const noexcept {
        return _system_error;
    }

    /**
     * @brief Stops I/O operations without triggering disconnection cleanup.
     * @details Stops the event watcher and pauses all I/O operations, but does not call `dispose()`
     *          or trigger `event::disconnected`. This allows the component to be restarted later
     *          via `start()` without going through the full disconnection/reconnection cycle.
     *
     * @note **Usage:** This method is useful for temporarily pausing I/O operations (e.g., during
     *       maintenance, rate limiting, or when waiting for external conditions). Unlike `disconnect()`,
     *       this does not mark the component as disposed, so it can be resumed by calling `start()` again.
     *
     * @note **Example Usage:**
     * @code
     * // Temporarily pause I/O operations
     * this->stop();
     *
     * // Later, resume operations
     * this->start();
     * @endcode
     *
     * @note **Difference from `disconnect()`:** `disconnect()` initiates a full cleanup cycle
     *       and triggers `event::disconnected`, while `stop()` only pauses operations and can be resumed.
     */
    void
    stop() noexcept {
        this->_async_event.stop();
    }

    /**
     * @brief Gets the total number of bytes read from the transport.
     * @return The cumulative count of bytes read since the component was created.
     *
     * @note **Usage:** This counter is incremented each time data is successfully read from the transport.
     *       It's useful for monitoring, statistics, and debugging purposes.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t total_read = this->bytes_read();
     * LOG_INFO("Total bytes read: " << total_read);
     * @endcode
     */
    [[nodiscard]] std::size_t
    bytes_read() const noexcept {
        return _bytes_read;
    }

    /**
     * @brief Gets the total number of bytes written to the transport.
     * @return The cumulative count of bytes written since the component was created.
     *
     * @note **Usage:** This counter is incremented each time data is successfully written to the transport.
     *       It's useful for monitoring, statistics, and debugging purposes.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t total_written = this->bytes_written();
     * LOG_INFO("Total bytes written: " << total_written);
     * @endcode
     */
    [[nodiscard]] std::size_t
    bytes_written() const noexcept {
        return _bytes_written;
    }

    /**
     * @brief Gets the total number of messages successfully processed.
     * @return The cumulative count of messages that have been successfully parsed and processed.
     *
     * @note **Usage:** This counter is incremented each time a complete message is successfully
     *       processed by the protocol. It's useful for monitoring message throughput and statistics.
     *
     * @note **Example Usage:**
     * @code
     * std::size_t msg_count = this->messages_processed();
     * LOG_INFO("Messages processed: " << msg_count);
     * @endcode
     */
    [[nodiscard]] std::size_t
    messages_processed() const noexcept {
        return _messages_processed;
    }

    /**
     * @brief Requests connection closure after all pending output data is delivered.
     * @details Marks the current protocol as invalid (`_protocol->not_ok()`).
     *          The `on(event::io&)` handler will then initiate `dispose()` after successfully
     *          writing any remaining buffered output if the protocol is not ok and output buffer is empty.
     */
    void
    close_after_deliver() noexcept {
        if (_protocol)
            _protocol->not_ok();
    }

    /**
     * @brief Publishes data to the output buffer and ensures write readiness.
     * @tparam _Args Types of data to publish.
     * @param args Data arguments to stream into `_Derived::out()`.
     * @return Reference to `_Derived::out()` buffer.
     */
    template <typename... _Args>
    inline auto &
    publish(_Args &&...args) {
        if (unlikely(_is_disposed || _reason))
            return Derived.out();
        const auto max_write = Derived.max_write_buffer_size();
        if (unlikely(max_write != static_cast<std::size_t>(-1) && Derived.pendingWrite() >= max_write)) {
            _system_error = 0;
            disconnect(event::disconnect_reason::buffer_overflow);
            return Derived.out();
        }

        ready_to_write();
        if constexpr (sizeof...(_Args)) {
            const auto before = Derived.pendingWrite();
            (Derived.out() << ... << std::forward<_Args>(args));
            const auto after = Derived.pendingWrite();
            if (unlikely(max_write != static_cast<std::size_t>(-1) && after > max_write)) {
                const auto added    = after - before;
                const auto overflow = after - max_write;
                if constexpr (requires { Derived.out().free_back(std::size_t{}); }) {
                    if (overflow <= added)
                        Derived.out().free_back(overflow);
                }
                _system_error = 0;
                disconnect(event::disconnect_reason::buffer_overflow);
            }
        }
        return Derived.out();
    }

    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    /**
     * @brief Initiates a graceful disconnection of the I/O component.
     * @param reason An optional integer code indicating the reason for disconnection.
     *                Defaults to `1` for user-initiated disconnection. Common values:
     *                - `0`: Normal shutdown (peer closed connection)
     *                - `1`: User-initiated disconnect
     *                - `2`: Protocol error
     *                - `3`: Message too large (DoS protection)
     * @details Sets an internal flag (`_reason`) and feeds an `EV_UNDEF` event to the listener.
     *          This typically causes the `on(event::io&)` handler to enter its error path,
     *          leading to the invocation of `dispose()` for cleanup.
     *
     * @note **Actor Integration:** When used within a `qb::Actor`, calling `disconnect()` will trigger
     *       `on(event::disconnected&)` if implemented, allowing the actor to handle the disconnection
     *       gracefully (e.g., attempt reconnection, notify other actors, or call `kill()`).
     *
     * @note This method is safe to call multiple times; subsequent calls will update the reason code
     *       but the disconnection process will only occur once.
     *
     * @note **Reason `0` caveat.** Explicit `disconnect(0)` is remapped to
     *       `disconnect_reason::user_initiated` because `_reason == 0` is the internal
     *       sentinel for "not disconnecting". Use the `disconnect(disconnect_reason)`
     *       overload for the strongly-typed path.
     */
    void
    disconnect(int reason = 1) noexcept {
        _reason = reason ? reason : static_cast<int>(event::disconnect_reason::user_initiated);
        this->_async_event.feed_event(EV_UNDEF);
    }

    /**
     * @brief Strongly-typed overload accepting a `disconnect_reason` enum value.
     */
    void
    disconnect(event::disconnect_reason reason) noexcept {
        disconnect(static_cast<int>(reason));
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, io>;

    /**
     * @brief Processes messages from the input buffer using the protocol.
     * @return true if processing succeeded, false if an error occurred.
     * @details When close_after_deliver() was called during message processing,
     *          this defers disposal until all pending output data has been flushed.
     */
    bool
    process_messages() noexcept {
        std::size_t ret = 0u;

        _on_message = true;
        while ((ret = this->_protocol->getMessageSize()) > 0) {
            const auto frame_exceeds_pending = [&]() noexcept {
                if (!this->_protocol->should_flush())
                    return false;
                if constexpr (requires {
                                  Derived.in();
                                  Derived.pendingRead();
                              })
                    return ret > Derived.pendingRead();
                else
                    return false;
            }();
            if (unlikely(ret > _max_message_size || frame_exceeds_pending)) {
                this->_protocol->not_ok();
                _system_error = 0;
                _reason       = -2;
                _on_message   = false;
                return false;
            }
            // Snapshot the protocol pointer AND its should_flush() before onMessage():
            // onMessage() may switch_protocol() (handshake → upgrade) or even
            // clear_protocols(), either of which can leave `protocol` dangling. The OLD
            // protocol's should_flush() governs flushing the bytes of THIS message, and
            // should_flush() is a per-protocol invariant (no setter is ever called), so
            // capturing it now is equivalent and removes every post-onMessage deref.
            auto      *protocol         = this->_protocol;
            const bool old_should_flush = protocol->should_flush();
            protocol->onMessage(ret);
            ++_messages_processed;
            if (unlikely(_reason)) {
                if (likely(old_should_flush))
                    Derived.flush(ret);
                if (Derived.pendingWrite()) {
                    this->ready_to_write();
                    _on_message = false;
                    return true;
                }
                _on_message = false;
                return false;
            }
            if (unlikely(!this->_protocol->ok())) {
                if (likely(old_should_flush))
                    Derived.flush(ret);
                if (Derived.pendingWrite()) {
                    this->ready_to_write();
                    _on_message = false;
                    return true;
                }
                _system_error = 0;
                _reason       = -1;
                _on_message   = false;
                return false;
            }
            if (likely(old_should_flush))
                Derived.flush(ret);
        }
        _on_message = false;
        return true;
    }

    /**
     * @brief Handles post-read processing (eof, pending_read events).
     */
    void
    handle_post_read() {
        if constexpr (qb::has_on<_Derived, event::pending_read> || qb::has_on<_Derived, event::eof>) {
            const auto pendingRead = Derived.pendingRead();
            if (pendingRead) {
                if constexpr (qb::has_on<_Derived, event::pending_read>) {
                    auto evt__pending_read = event::pending_read{pendingRead};
                    Derived.on(std::move(evt__pending_read));
                }
            } else {
                if constexpr (qb::has_on<_Derived, event::eof>) {
                    auto evt__eof = event::eof{};
                    Derived.on(std::move(evt__eof));
                }
            }
        }
    }

    /**
     * @brief Handles write operations and post-write events.
     * @return true if write succeeded, false if an error occurred.
     */
    bool
    handle_write() noexcept {
        auto raw_ret = Derived.write();
        if (unlikely(raw_ret < 0)) {
            if (qb::io::socket::not_send_error(qb::io::socket::get_last_errno())) {
                ready_to_write();
                return true;
            }
            return false;
        }
        auto ret = static_cast<std::size_t>(raw_ret);

        _bytes_written += ret;

        if (!Derived.pendingWrite()) {
            if (unlikely(_reason || !_protocol || !_protocol->ok()))
                return false;
            this->_async_event.set(EV_READ);
            if constexpr (qb::has_on<_Derived, event::eos>) {
                auto evt__eos = event::eos{};
                Derived.on(std::move(evt__eos));
            }
        } else if constexpr (qb::has_on<_Derived, event::pending_write>) {
            auto evt__pending_write = event::pending_write{Derived.pendingWrite()};
            Derived.on(std::move(evt__pending_write));
        }
        return true;
    }

    /**
     * @brief Internal I/O event handler for read and write readiness.
     * @param event The `event::io` that triggered (`event._revents` holds `EV_READ` and/or `EV_WRITE`).
     * @details
     * This is the central dispatch for I/O events, delegating to helper methods
     * to reduce complexity: `process_messages()`, `handle_post_read()`, and `handle_write()`.
     */
    void
    on(event::io const &event) {
        // Keep a strong reference to prevent UAF when process_messages() triggers
        // extractSession() (e.g. WebSocket upgrade), which can destroy *this before
        // onMessage() / Derived.eof() / handle_post_read() finish executing.
        // Stored as shared_ptr<void> so it works when _Derived inherits from
        // enable_shared_from_this<Base> where Base != _Derived (e.g. HTTP CRTP sessions).
        // Declaration is at function scope (above all goto targets) to satisfy
        // C++ goto-past-initialization rules.
        std::shared_ptr<void> _self_guard;
        bool                  ok = false; // Declare early to avoid goto bypassing initialization

        if (_on_message)
            return;

        if constexpr (qb::has_shared_from_this<_Derived>) {
            try {
                _self_guard = Derived.shared_from_this();
            } catch (std::bad_weak_ptr const &) {
            }
        } else if constexpr (requires { Derived.shared(); }) {
            _self_guard = Derived.shared();
        }

        if (_reason)
            goto error;

        if (event._revents & EV_READ && _protocol && _protocol->ok()) {
            constexpr const std::size_t invalid_ret = static_cast<std::size_t>(-1);

            auto ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret)) {
                if (qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
                    return;
                goto error;
            }

            // Check for buffer size limit exceeded (DoS protection)
            if (unlikely(ret == static_cast<std::size_t>(-2))) {
                _system_error = 0;
                _reason       = -3; // Buffer size limit exceeded (DoS protection)
                goto error;
            }

            // Update statistics
            _bytes_read += ret;

            if (!process_messages())
                goto error;

            Derived.eof();
            handle_post_read();
            ok = true;
        }

        // Drain any output that piled up during process_messages() in the same
        // invocation, instead of waiting for libev to fire a separate EV_WRITE.
        // Rationale: on bidirectional sessions where the server echoes every
        // request (or any responder pattern), each EV_READ wakeup adds N
        // messages to the output buffer. With epoll/kqueue under sustained
        // EV_READ pressure (notably SSL-over-UDS on macOS, where the kernel
        // socket buffer is much smaller than TCP loopback), EV_WRITE
        // notifications can be starved and the output buffer grows without
        // bound across many cycles, eventually triggering a hard close on the
        // peer side. Issuing a non-blocking handle_write() here keeps the
        // outgoing pipe flowing as long as the kernel has space; if the
        // socket would block (or SSL needs a renegotiation read), handle_write
        // simply leaves bytes in the buffer and the watcher's EV_WRITE
        // registration takes over for the next round.
        if ((event._revents & EV_WRITE) || (ok && Derived.pendingWrite() > 0)) {
            if (!handle_write())
                goto error;
            ok = true;
        }

        if (ok)
            return;
    error:
#ifdef _WIN32
        // Ignore a spurious would-block only when no explicit reason is pending.
        // When _reason is set (user disconnect(), protocol error, DoS guard), the
        // last WSA error is unrelated/stale — letting it suppress dispose() would
        // silently drop the disconnection and leave a zombie session.
        if (!_reason && qb::io::socket::get_last_errno() == QB_WINDOWS_WOULDBLOCK_ERROR)
            return;
#endif
        if (!_reason)
            _system_error = qb::io::socket::get_last_errno();
        dispose();
    }

protected:
    /**
     * @brief Disposes of resources and finalizes disconnection for the I/O component.
     * @details Ensures cleanup happens only once (via `_is_disposed`).
     *          Triggers `_Derived::on(event::disconnected&)` (with `_reason`)
     *          or `_Derived::on(event::dispose&)` based on derived class capabilities and server association.
     *          This is the primary cleanup point before the `async::base` destructor unregisters the watcher.
     *
     * @note **Important: Difference between server-associated and standalone clients:**
     *       - **Server-associated clients** (`has_server = true`): Created via `accept()` on a server.
     *         The server manages the lifecycle, so we notify it via `server().disconnected(id())`
     *         and let the server handle watcher cleanup. No explicit `stop()` is needed here.
     *       - **Standalone clients** (`has_server = false`): Created via `connect()` for outgoing connections.
     *         These manage their own lifecycle, so we explicitly call `_async_event.stop()` to prevent
     *         further events from being processed after disconnection, before calling `event::dispose`.
     *
     * @note **Actor Lifecycle Integration:** When used within a `qb::Actor`, this method is called
     *       during the I/O component's cleanup phase. The sequence is:
     *       1. `on(event::disconnected&)` is called if implemented, allowing the actor to handle the disconnection
     *          (e.g., attempt reconnection, notify other actors, or call `kill()` if termination is required).
     *       2. `on(event::dispose&)` is called if implemented, providing a final cleanup hook before destruction.
     *       3. The base class destructor unregisters the event watcher from the listener.
     *       Actors should implement `on(event::disconnected&)` to handle connection loss gracefully.
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;

        if constexpr (qb::has_on<_Derived, event::disconnected>) {
            if (_system_error != 0) {
                auto evt = event::disconnected::with_error(_reason, _system_error);
                Derived.on(std::move(evt));
            } else {
                auto evt = event::disconnected{_reason};
                Derived.on(std::move(evt));
            }
        }

        if constexpr (_Derived::has_server) {
            // Server-associated client: server manages lifecycle, just notify it
            Derived.server().disconnected(Derived.id());
        } else {
            // Standalone client: must stop watcher immediately to prevent further events
            // This is critical for clients created via connect() to avoid processing events after disconnect
            this->_async_event.stop(); // Stop the watcher to prevent further events
            if constexpr (qb::has_on<_Derived, event::dispose>) {
                auto evt__dispose = event::dispose{};
                Derived.on(std::move(evt__dispose));
            }
        }
    }

    /**
     * @brief Resets internal disconnection state so the component can be reused.
     *
     * After `dispose()` sets `_is_disposed = true`, subsequent calls to `dispose()`
     * are no-ops, preventing `on(disconnected)` from firing on the next connection drop.
     * Call this before restarting I/O on the same object (e.g. for auto-reconnect) to
     * restore a clean slate identical to the freshly-constructed state.
     *
     * @pre The old async event must be stopped and the old transport socket closed
     *      before calling this — typically done from within the reconnect setup path.
     */
    void
    reset_io_state() noexcept {
        _is_disposed  = false;
        _on_message   = false;
        _reason       = 0;
        _system_error = 0;
    }
};

} // namespace qb::io::async

#undef Derived
#endif // QB_IO_ASYNC_IO_H
