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
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include <qb/utility/type_traits.h>
#include "event/all.h"
#include "listener.h"
#include "protocol.h"

CREATE_MEMBER_CHECK(Protocol);
GENERATE_HAS_METHOD(flush)

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
        //std::cout << "handle=" << _async_event.fd
        //          << " disposed async.stop()" << std::endl;
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
    explicit with_timeout(ev_tstamp timeout = 3)
        : _timeout(timeout)
        , _last_activity(0) {
        if (timeout > 0.)
            this->_async_event.start(_timeout);
    }

    /**
     * @brief Updates the last activity timestamp to the current event loop time.
     * @details This method should be called by the derived class whenever an activity occurs
     *          that should reset the timeout countdown (e.g., receiving data, user input).
     *          It sets `_last_activity` to `this->_async_event.loop.now()`.
     */
    void
    updateTimeout() noexcept {
        _last_activity = this->_async_event.loop.now();
    }

    /**
     * @brief Sets a new timeout value and restarts the timer.
     * @param timeout New timeout value in seconds. If `0.0` or less, the timer is stopped (disabled).
     *                Otherwise, the timer is configured with the new timeout and started.
     *                The `_last_activity` timestamp is also updated to the current event loop time.
     */
    void
    setTimeout(ev_tstamp timeout) noexcept {
        _timeout = timeout;
        if (_timeout > 0.) { // Check against 0, not just if(_timeout)
            _last_activity = ev_time(); // Consider using this->_async_event.loop.now() for consistency
            this->_async_event.set(_timeout);
            this->_async_event.start();
        } else
            this->_async_event.stop();
    }

    /**
     * @brief Gets the current configured timeout value.
     * @return Current timeout value in seconds.
     */
    auto
    getTimeout() const noexcept {
        return _timeout;
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

        if (after < 0.)
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
 * @tparam _Func The function type (or callable object type) to execute after the timeout.
 */
template <typename _Func>
class Timeout : public with_timeout<Timeout<_Func>> {
    _Func _func; /**< The callable (function, lambda, functor) to execute upon timeout. */

public:
    /**
     * @brief Constructor that schedules a function to be called after a timeout.
     * @param func The function to execute. It will be moved into the Timeout object.
     * @param timeout Timeout duration in seconds before execution. If `0.0` or less,
     *                the function is executed immediately (or in the next loop iteration,
     *                depending on `with_timeout` behavior) and this `Timeout` object is deleted.
     */
    Timeout(_Func &&func, double timeout = 0.)
        : with_timeout<Timeout<_Func>>(timeout)
        , _func(std::forward<_Func>(func)) {
        if (!timeout) { // More direct check for zero or less
            _func();
            delete this;
        }
    }

    /**
     * @brief Timer event handler called when the timeout expires.
     * @param event The `event::timer` that triggered (marked as unused here as specific timer details are not needed).
     * @details Executes the stored function `_func` and then deletes this `Timeout` object,
     *          ensuring one-shot execution and automatic cleanup.
     */
    void
    on(event::timer const & /*event*/) const { // Marked event as unused
        _func();
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
callback(_Func &&func, double timeout = 0.) {
    new Timeout<_Func>(std::forward<_Func>(func), timeout);
}

template <typename _Func, typename Rep, typename Period>
void callback(_Func&& func, std::chrono::duration<Rep, Period> timeout_duration) {
    double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(timeout_duration).count();
    callback(std::forward<_Func>(func), seconds);
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
    using base_t = base<file_watcher<_Derived>, event::file>;
    AProtocol<_Derived> *_protocol = nullptr; /**< Protocol instance for processing file contents. Can be null. */
    std::vector<AProtocol<_Derived> *> _protocol_list; /**< List of owned protocol instances for cleanup. */

public:
    using base_io_t = file_watcher<_Derived>; /**< Base I/O type alias for CRTP. */
    constexpr static const bool do_read = true; /**< Flag indicating this watcher type reads file content. */

    /**
     * @brief Default constructor.
     * Initializes the file_watcher without a specific protocol.
     * A protocol can be set later using `switch_protocol`.
     */
    file_watcher() = default;

    /**
     * @brief Constructor with an externally managed protocol.
     * @param protocol Pointer to an existing protocol instance. This `file_watcher` will use it
     *                 but not take ownership unless it's the same instance later set via `switch_protocol`
     *                 which would then add it to `_protocol_list`.
     *                 If ownership by `file_watcher` is desired from construction, prefer using `switch_protocol` after default construction.
     */
    file_watcher(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {}

    /**
     * @brief Deleted copy constructor to prevent unintended copying of watcher state and resources.
     */
    file_watcher(file_watcher const &) = delete;

    /**
     * @brief Destructor.
     * @details Cleans up all protocol instances created and owned by this `file_watcher`
     *          (i.e., those added to `_protocol_list` via `switch_protocol`).
     */
    ~file_watcher() noexcept {
        for (auto protocol_ptr : _protocol_list) // Renamed to avoid conflict
            delete protocol_ptr;
    }

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
     */
    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&...args) {
        auto new_protocol = new _Protocol(std::forward<_Args>(args)...);
        if (new_protocol->ok()) {
            _protocol = new_protocol;
            _protocol_list.push_back(new_protocol);
            return new_protocol;
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
    start(std::string const &fpath, ev_tstamp ts = 0.1) noexcept {
        this->_async_event.start(fpath.c_str(), ts);
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
     * @note Requires `_Derived::do_read` to be true (which it is for `file_watcher` by default).
     *       Assumes `_protocol` is not null if messages are expected.
     */
    int
    read_all() {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        std::size_t          ret         = 0u;
        do {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                return -1;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                // has a new message to read
                this->_protocol->onMessage(ret);
                Derived.flush(ret);
            }
            Derived.eof();
            if constexpr (has_method_on<_Derived, void, event::pending_read>::value ||
                          has_method_on<_Derived, void, event::eof>::value) {
                const auto pendingRead = Derived.pendingRead();
                if (pendingRead) {
                    if constexpr (has_method_on<_Derived, void,
                                                event::pending_read>::value) {
                        Derived.on(event::pending_read{pendingRead});
                    }
                } else {
                    if constexpr (has_method_on<_Derived, void, event::eof>::value) {
                        Derived.on(event::eof{});
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
        int ret = 0u;

        // forward event to Derived if desired
        if constexpr (has_method_on<_Derived, void, event::file>::value) {
            Derived.on(event);
        }

        auto diff_read = event.attr.st_size - event.prev.st_size;
        if (!_protocol->ok() || !event.attr.st_nlink ||
            (diff_read < 0 && lseek(Derived.transport().native_handle(), 0, SEEK_SET)))
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
    using base_io_t = directory_watcher<_Derived>; /**< Base I/O type alias for CRTP. */
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
    start(std::string const &fpath, ev_tstamp ts = 0.1) noexcept {
        this->_async_event.start(fpath.c_str(), ts);
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
        //        int ret = 0u;

        // forward event to Derived if desired
        if constexpr (has_method_on<_Derived, void, event::file>::value) {
            Derived.on(event);
        }

        //        if (ret < 0) {
        //            this->_async_event.stop();
        //        }
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
 * @tparam _Derived The derived class type (CRTP pattern) that provides the transport
 *                  (e.g., a socket wrapper) and handles protocol messages and I/O events.
 */
template <typename _Derived>
class input : public base<input<_Derived>, event::io> {
    using base_t                   = base<input<_Derived>, event::io>;
    AProtocol<_Derived> *_protocol = nullptr; /**< Current protocol for processing input. Can be changed via `switch_protocol`. */
    std::vector<AProtocol<_Derived> *> _protocol_list; /**< List of owned protocol instances for cleanup. */
    bool _on_message  = false; /**< Internal flag to prevent re-entrant calls to `on(event::io&)` during message processing. */
    bool _is_disposed = false; /**< Internal flag to ensure `dispose()` is called only once. */
    int _reason = 0; /**< Stores the reason for disconnection if initiated by `disconnect()`. */

public:
    using base_io_t = input<_Derived>; /**< Base I/O type alias for CRTP. */
    constexpr static const bool has_server = false; /**< Indicates this component is not inherently a server (e.g., an acceptor). */

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
    input(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {
        _protocol_list.push_back(protocol); // Assumes ownership
    }

    /**
     * @brief Deleted copy constructor to prevent unintended copying of I/O state and resources.
     */
    input(input const &) = delete;

    /**
     * @brief Destructor.
     * @details Calls `clear_protocols()` to clean up all protocol instances owned by this component.
     *          The base class destructor will handle unregistering the `event::io` watcher.
     */
    ~input() noexcept {
        clear_protocols();
    }

    /**
     * @brief Switches to a new protocol for processing input, taking ownership of the new protocol.
     * @tparam _Protocol The concrete `AProtocol` type to instantiate.
     * @tparam _Args Argument types for the `_Protocol` constructor.
     * @param args Arguments to forward to the `_Protocol` constructor.
     * @return Pointer to the newly created and activated protocol instance if successful (protocol's `ok()` returns true),
     *         otherwise `nullptr` (and the created protocol instance is deleted).
     * @details The new protocol instance is added to an internal list and will be cleaned up by this `input` component's destructor.
     *          The current active protocol is set to this new instance. Previous protocols in the list are not deleted by this call.
     */
    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&...args) {
        auto new_protocol = new _Protocol(std::forward<_Args>(args)...);
        if (new_protocol->ok()) {
            _protocol = new_protocol;
            _protocol_list.push_back(new_protocol);
            return new_protocol;
        } else
            delete new_protocol;
        return nullptr;
    }

    /**
     * @brief Clears all owned protocol instances.
     * @details Deletes all protocol instances stored in the internal `_protocol_list` and resets the current `_protocol` pointer to `nullptr`.
     *          This is called automatically by the destructor.
     */
    void
    clear_protocols() {
        for (auto protocol : _protocol_list)
            delete protocol;
        _protocol_list.clear();
        _protocol = nullptr;
    };

    /**
     * @brief Gets a pointer to the current active protocol instance.
     * @return Pointer to the `AProtocol<_Derived>` currently in use for message parsing, or `nullptr` if no protocol is set.
     */
    AProtocol<_Derived> *
    protocol() {
        return _protocol;
    }

    /**
     * @brief Starts asynchronous input operations.
     * @details Sets the underlying transport (obtained via `_Derived::transport()`) to non-blocking mode
     *          and starts the `event::io` watcher to listen for read events (`EV_READ`).
     *          Resets any previous disconnection reason (`_reason = 0`).
     */
    void
    start() noexcept {
        _reason = 0;
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
     * @brief Initiates a graceful disconnection of the input component.
     * @param reason An optional integer code indicating the reason for disconnection (e.g., user action, protocol error).
     *               This value will be passed in the `event::disconnected` if that event is handled by the derived class.
     * @details Sets an internal flag (`_reason`) with the provided reason and feeds an `EV_UNDEF` event to the listener.
     *          This typically causes the `on(event::io&)` handler to enter its error path during the next event loop cycle,
     *          leading to the invocation of the `dispose()` method for cleanup.
     */
    void
    disconnect(int reason = 1) {
        _reason = reason;
        this->_async_event.feed_event(EV_UNDEF);
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, input>;

    /**
     * @brief Internal I/O event handler, called by the listener when `event::io` triggers for read readiness.
     * @param event The `event::io` that was triggered. `event._revents` contains the actual events (e.g., `EV_READ`).
     * @details
     * This is the core logic for handling incoming data.
     * If not currently processing a message (`_on_message` is false to prevent re-entrance), no disconnection is pending (`_reason` is 0),
     * and the protocol is valid (`_protocol->ok()`):
     * 1. If `EV_READ` is set in `event._revents`, it attempts to read data from `_Derived::read()` into the input buffer.
     * 2. If the read is successful (returns >= 0 bytes), it sets `_on_message = true` to guard against re-entrant calls during protocol processing.
     * 3. It then enters a loop, repeatedly calling `this->_protocol->getMessageSize()` and `this->_protocol->onMessage()`
     *    to parse and handle complete messages from the buffer, flushing processed data via `_Derived::flush()`.
     * 4. After the loop (no more complete messages), `_on_message` is reset to `false`, and `_Derived::eof()` is called.
     * 5. It then checks if `_Derived` has handlers for `event::pending_read` (if data remains in buffer) or `event::eof` (if buffer is empty)
     *    and invokes them accordingly.
     * If any OS-level read error occurs (read returns < 0) or if `_reason` is set (due to `disconnect()` call), it jumps to an error handling path which calls `dispose()`.
     * The `_on_message` flag is critical for preventing issues if `_protocol->onMessage()` itself causes new events to be processed by the loop immediately.
     */
    void
    on(event::io const &event) {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        std::size_t                 ret         = 0u;

        if (_on_message)
            return;
        if (_reason || !_protocol->ok())
            goto error;

        if (likely(event._revents & EV_READ)) {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                goto error;
            _on_message = true;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                auto protocol = this->_protocol;
                protocol->onMessage(ret);
                if (protocol->should_flush())
                    Derived.flush(ret);
            }
            _on_message = false;
            Derived.eof();
            if constexpr (has_method_on<_Derived, void, event::pending_read>::value ||
                          has_method_on<_Derived, void, event::eof>::value) {
                const auto pendingRead = Derived.pendingRead();
                if (pendingRead) {
                    if constexpr (has_method_on<_Derived, void,
                                                event::pending_read>::value) {
                        Derived.on(event::pending_read{pendingRead});
                    }
                } else {
                    if constexpr (has_method_on<_Derived, void, event::eof>::value) {
                        Derived.on(event::eof{});
                    }
                }
            }
            return;
        }
    error:
#ifdef _WIN32
        if (socket::get_last_errno() == 10035)
            return;
#endif
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
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;    

        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            Derived.on(event::disconnected{_reason});
        }

        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else if constexpr (has_method_on<_Derived, void, event::dispose>::value) {
            Derived.on(event::dispose{});
        }
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
 * @tparam _Derived The derived class type (CRTP pattern) that provides the transport
 *                  and handles I/O events like `eos` (end of stream) or `pending_write`.
 */
template <typename _Derived>
class output : public base<output<_Derived>, event::io> {
    using base_t = base<output<_Derived>, event::io>;
    bool _is_disposed = false; /**< Internal flag to ensure `dispose()` is called only once. */
    int _reason = 0; /**< Stores the reason for disconnection if initiated by `disconnect()`. */

public:
    using base_io_t = output<_Derived>; /**< Base I/O type alias for CRTP. */
    constexpr static const bool has_server = false; /**< Indicates this component is not inherently a server. */

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
     *          Resets any previous disconnection reason (`_reason = 0`).
     */
    void
    start() noexcept {
        _reason = 0;
        Derived.transport().set_nonblocking(true);
        this->_async_event.start(Derived.transport().native_handle(), EV_WRITE);
    }

    /**
     * @brief Ensures the I/O watcher is listening for write events (`EV_WRITE`).
     * @details If the internal `event::io` watcher (from the `base` class) is not currently set to listen for `EV_WRITE`,
     *          this method reconfigures it to include `EV_WRITE` in its watched events. This is often called implicitly
     *          by `publish()` or `operator<<` to signal that there is data ready to be written.
     */
    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE))
            this->_async_event.set(EV_WRITE);
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
    publish(_Args &&...args) noexcept {
        ready_to_write();
        if constexpr (sizeof...(_Args))
            (Derived.out() << ... << std::forward<_Args>(args));
        return Derived.out();
    }

    /**
     * @brief Stream operator for publishing data, equivalent to `publish(std::forward<T>(data))`.
     * @tparam T Type of data to publish.
     * @param data Data to publish.
     * @return A reference to the `_Derived::out()` buffer.
     * @see publish()
     */
    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    /**
     * @brief Initiates a graceful disconnection of the output component.
     * @param reason An optional integer code indicating the reason for disconnection.
     * @details Sets an internal flag (`_reason`) and feeds an `EV_UNDEF` event to the listener.
     *          This typically causes the `on(event::io&)` handler to enter its error path,
     *          leading to the invocation of `dispose()` for cleanup.
     */
    void
    disconnect(int reason = 1) {
        _reason = reason;
        this->_async_event.feed_event(EV_UNDEF);
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
        auto ret = 0;

        if (_reason)
            goto error;

        if (likely(event._revents & EV_WRITE)) {
            ret = Derived.write();
            if (unlikely(ret < 0))
                goto error;
            if (!Derived.pendingWrite()) {
                this->_async_event.set(EV_NONE);
                if constexpr (has_method_on<_Derived, void, event::eos>::value) {
                    Derived.on(event::eos{});
                }
            } else if constexpr (has_method_on<_Derived, void,
                                               event::pending_write>::value) {
                Derived.on(event::pending_write{Derived.pendingWrite()});
            }
            return;
        }
    error:
#ifdef _WIN32
        if (socket::get_last_errno() == 10035)
            return;
#endif
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
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;

        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            Derived.on(event::disconnected{_reason});
        }

        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else if constexpr (has_method_on<_Derived, void, event::dispose>::value) {
            Derived.on(event::dispose{});
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
 * @tparam _Derived The derived class type (CRTP pattern) that provides the transport
 *                  and handles protocol messages and I/O events.
 */
template <typename _Derived>
class io : public base<io<_Derived>, event::io> {
    using base_t                   = base<io<_Derived>, event::io>;
    AProtocol<_Derived> *_protocol = nullptr; /**< Current protocol for I/O processing. */
    std::vector<AProtocol<_Derived> *> _protocol_list; /**< List of owned protocol instances. */
    bool _on_message  = false; /**< Internal flag for re-entrance protection in `on(event::io&)`. */
    bool _is_disposed = false; /**< Internal flag for `dispose()` idempotency. */
    int _reason = 0; /**< Disconnection reason code. */

public:
    typedef io<_Derived>        base_io_t; /**< Base I/O type alias for CRTP. */
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
    io(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {
        _protocol_list.push_back(protocol);
    }

    /**
     * @brief Deleted copy constructor.
     */
    io(io const &) = delete;

    /**
     * @brief Destructor.
     * @details Cleans up owned protocols via `clear_protocols()`. Base class handles watcher unregistration.
     */
    ~io() noexcept {
        clear_protocols();
    }

    /**
     * @brief Switches to a new protocol for I/O processing, taking ownership.
     * @tparam _Protocol The concrete `AProtocol` type to instantiate.
     * @tparam _Args Argument types for the `_Protocol` constructor.
     * @param args Arguments to forward to the `_Protocol` constructor.
     * @return Pointer to the new protocol if successful, `nullptr` otherwise.
     */
    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&...args) {
        auto new_protocol = new _Protocol(std::forward<_Args>(args)...);
        if (new_protocol->ok()) {
            _protocol = new_protocol;
            _protocol_list.push_back(new_protocol);
            return new_protocol;
        } else
            delete new_protocol;
        return nullptr;
    }

    /**
     * @brief Clears all owned protocol instances.
     * @details Deletes protocols in `_protocol_list` and resets `_protocol` pointer to `nullptr`.
     */
    void
    clear_protocols() {
        for (auto protocol : _protocol_list)
            delete protocol;
        _protocol_list.clear();
        _protocol = nullptr;
    };

    /**
     * @brief Gets a pointer to the current active protocol instance.
     * @return Pointer to the `AProtocol<_Derived>` for message parsing/formatting.
     */
    AProtocol<_Derived> *
    protocol() {
        return _protocol;
    }

    /**
     * @brief Starts bidirectional asynchronous I/O operations.
     * @details Sets the transport to non-blocking and starts listening for read events (`EV_READ`).
     *          Resets disconnection reason.
     */
    void
    start() noexcept {
        _reason = 0;
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
     * @brief Ensures the I/O watcher is listening for write events (`EV_WRITE`).
     * @details Modifies the event watcher flags to include `EV_WRITE` if not already set.
     */
    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE)) {
            this->_async_event.set(this->_async_event.events | EV_WRITE);
        }
    }

    /**
     * @brief Requests connection closure after all pending output data is delivered.
     * @details Marks the current protocol as invalid (`_protocol->not_ok()`).
     *          The `on(event::io&)` handler will then initiate `dispose()` after successfully
     *          writing any remaining buffered output if the protocol is not ok and output buffer is empty.
     */
    void
    close_after_deliver() const noexcept {
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
    publish(_Args &&...args) noexcept {
        ready_to_write();
        if constexpr (sizeof...(_Args))
            (Derived.out() << ... << std::forward<_Args>(args));
        return Derived.out();
    }

    /**
     * @brief Stream operator for publishing data.
     * @tparam T Type of data.
     * @param data Data to publish.
     * @return Reference to `_Derived::out()` buffer.
     */
    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    /**
     * @brief Initiates a graceful disconnection.
     * @param reason Optional reason code for disconnection.
     * @details Triggers the `dispose()` mechanism via an `EV_UNDEF` event sent to the event loop.
     */
    void
    disconnect(int reason = 1) {
        _reason = reason;
        this->_async_event.feed_event(EV_UNDEF);
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, io>;

    /**
     * @brief Internal I/O event handler for read and write readiness.
     * @param event The `event::io` that triggered (`event._revents` holds `EV_READ` and/or `EV_WRITE`).
     * @details
     * This is the central dispatch for I/O events.
     * - If `_reason` is set (from `disconnect()`), calls `dispose()`.
     * - If `event._revents` has `EV_READ` and protocol is `ok()`:
     *   - Calls `_Derived::read()` to get data into the input buffer.
     *   - If read error, calls `dispose()`.
     *   - Otherwise, sets `_on_message = true`, loops to process messages via `_protocol`,
     *     calls `_Derived::flush()` for processed data, then `_Derived::eof()`.
     *   - Triggers `event::pending_read` or `event::eof` on `_Derived` as appropriate.
     *   - Resets `_on_message = false`.
     * - If `event._revents` has `EV_WRITE`:
     *   - Calls `_Derived::write()` to send data from output buffer.
     *   - If write error, calls `dispose()`.
     *   - If output buffer becomes empty (`_Derived::pendingWrite() == 0`):
     *     - If protocol is not `ok()` (due to `close_after_deliver()`), calls `dispose()`.
     *     - Else, sets watcher to only `EV_READ` and triggers `event::eos` on `_Derived`.
     *   - Else (output buffer still has data), triggers `event::pending_write` on `_Derived`.
     * - If neither read nor write event occurred but handler was called (e.g. `EV_UNDEF`), calls `dispose()`.
     * The `_on_message` flag guards against re-entrant calls during protocol message processing.
     */
    void
    on(event::io const &event) {
        constexpr const std::size_t invalid_ret = static_cast<std::size_t>(-1);
        std::size_t                 ret         = 0u;
        bool                        ok          = false;

        if (_on_message)
            return;
        if (_reason)
            goto error;
        if (event._revents & EV_READ && _protocol->ok()) {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                goto error;

            _on_message = true;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                auto protocol = this->_protocol;
                protocol->onMessage(ret);
                if (protocol->should_flush())
                    Derived.flush(ret);
            }
            _on_message = false;
            Derived.eof();
            if constexpr (has_method_on<_Derived, void, event::pending_read>::value ||
                          has_method_on<_Derived, void, event::eof>::value) {
                const auto pendingRead = Derived.pendingRead();
                if (pendingRead) {
                    if constexpr (has_method_on<_Derived, void,
                                                event::pending_read>::value) {
                        Derived.on(event::pending_read{pendingRead});
                    }
                } else {
                    if constexpr (has_method_on<_Derived, void, event::eof>::value) {
                        Derived.on(event::eof{});
                    }
                }
            }
            ok = true;
        }
        if (event._revents & EV_WRITE) {
            ret = static_cast<std::size_t>(Derived.write());
            if (unlikely(ret == invalid_ret))
                goto error;
            if (!Derived.pendingWrite()) {
                if (!_protocol->ok())
                    goto error;
                this->_async_event.set(EV_READ);
                if constexpr (has_method_on<_Derived, void, event::eos>::value) {
                    Derived.on(event::eos{});
                }
            } else if constexpr (has_method_on<_Derived, void,
                                               event::pending_write>::value) {
                Derived.on(event::pending_write{Derived.pendingWrite()});
            }
            ok = true;
        }
        if (ok)
            return;
    error:
#ifdef _WIN32
        if (socket::get_last_errno() == 10035)
            return;
#endif
        dispose();
    }

protected:
    /**
     * @brief Disposes of resources and finalizes disconnection for the I/O component.
     * @details Ensures cleanup happens only once (via `_is_disposed`).
     *          Triggers `_Derived::on(event::disconnected&)` (with `_reason`)
     *          or `_Derived::on(event::dispose&)` based on derived class capabilities and server association.
     *          This is the primary cleanup point before the `async::base` destructor unregisters the watcher.
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;

        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            Derived.on(event::disconnected{_reason});
        }

        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else {
            if constexpr (has_method_on<_Derived, void, event::dispose>::value)
                Derived.on(event::dispose{});
            this->_async_event.stop(); // Stop the watcher to prevent further events
        }
    }
};

} // namespace qb::io::async

#undef Derived
#endif // QB_IO_ASYNC_IO_H
