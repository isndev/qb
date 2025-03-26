/**
 * @file qb/io/async/io.h
 * @brief Core asynchronous I/O class templates
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

#include "event/all.h"
#include "listener.h"
#include "protocol.h"
#include <qb/utility/type_traits.h>

CREATE_MEMBER_CHECK(Protocol);
GENERATE_HAS_METHOD(flush)

#define Derived static_cast<_Derived &>(*this)

namespace qb::io::async {

/**
 * @class base
 * @brief Base class for all asynchronous I/O components
 * 
 * This template class provides the foundation for all asynchronous I/O components.
 * It handles registration and unregistration of events with the listener.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _EV_EVENT The event type to register
 */
template <typename _Derived, typename _EV_EVENT>
class base {
protected:
    _EV_EVENT &_async_event; /**< Reference to the registered event */
    
    /**
     * @brief Constructor
     * 
     * Registers the event with the current listener.
     */
    base()
        : _async_event(listener::current.registerEvent<_EV_EVENT>(Derived)) {}
    
    /**
     * @brief Destructor
     * 
     * Unregisters the event from the current listener.
     */
    ~base() {
        listener::current.unregisterEvent(_async_event._interface);
    }
};

/**
 * @class with_timeout
 * @brief Adds timeout functionality to asynchronous components
 * 
 * This template class extends the base class with timeout functionality.
 * It allows setting a timeout after which an action is triggered if no
 * activity is detected.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class with_timeout : public base<with_timeout<_Derived>, event::timer> {
    ev_tstamp _timeout;      /**< Timeout value in seconds */
    ev_tstamp _last_activity; /**< Timestamp of the last activity */

public:
    /**
     * @brief Constructor
     * 
     * @param timeout Timeout value in seconds (0 = disabled)
     */
    explicit with_timeout(ev_tstamp timeout = 3)
        : _timeout(timeout)
        , _last_activity(0) {
        if (timeout > 0.)
            this->_async_event.start(_timeout);
    }

    /**
     * @brief Updates the last activity timestamp
     * 
     * This method should be called whenever activity occurs to prevent
     * the timeout from triggering.
     */
    void
    updateTimeout() noexcept {
        _last_activity = this->_async_event.loop.now();
    }

    /**
     * @brief Sets a new timeout value
     * 
     * @param timeout New timeout value in seconds (0 = disabled)
     */
    void
    setTimeout(ev_tstamp timeout) noexcept {
        _timeout = timeout;
        if (_timeout) {
            _last_activity = ev_time();
            this->_async_event.set(_timeout);
            this->_async_event.start();
        } else
            this->_async_event.stop();
    }

    /**
     * @brief Gets the current timeout value
     * 
     * @return Current timeout value in seconds
     */
    auto
    getTimeout() const noexcept {
        return _timeout;
    }

private:
    friend class listener::RegisteredKernelEvent<event::timer, with_timeout>;

    /**
     * @brief Timer event handler
     * 
     * This method is called when the timer event is triggered.
     * It checks if the timeout has been exceeded and calls the derived
     * class's handler if it has.
     * 
     * @param event The timer event
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
 * @brief Execute a function after a timeout
 * 
 * This utility class executes a function after a specified timeout.
 * It automatically deletes itself after execution.
 * 
 * @tparam _Func The function type to execute
 */
template <typename _Func>
class Timeout : public with_timeout<Timeout<_Func>> {
    _Func _func; /**< Function to execute */

public:
    /**
     * @brief Constructor
     * 
     * @param func Function to execute
     * @param timeout Timeout before execution in seconds (0 = immediate)
     */
    Timeout(_Func &&func, double timeout = 0.)
        : _func(std::forward<_Func>(func))
        , with_timeout<Timeout<_Func>>(timeout) {
        if (!timeout) {
            _func();
            delete this;
        }
    }
    
    /**
     * @brief Timer event handler
     * 
     * This method is called when the timeout expires.
     * It executes the function and deletes this object.
     * 
     * @param event The timer event
     */
    void
    on(event::timer const &) const {
        _func();
        delete this;
    }
};

/**
 * @brief Utility function to execute a callback after a timeout
 * 
 * @tparam _Func The function type to execute
 * @param func Function to execute
 * @param timeout Timeout before execution in seconds (0 = immediate)
 */
template <typename _Func>
void
callback(_Func &&func, double timeout = 0.) {
    new Timeout<_Func>(std::forward<_Func>(func), timeout);
}

/**
 * @class file_watcher
 * @brief Watches a file for changes and processes its contents
 * 
 * This template class watches a file for changes and processes its contents
 * using a protocol. It triggers events when the file changes and reads
 * the updated content.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class file_watcher : public base<file_watcher<_Derived>, event::file> {
    using base_t = base<file_watcher<_Derived>, event::file>;
    AProtocol<_Derived> *_protocol = nullptr; /**< Protocol for processing file contents */
    std::vector<AProtocol<_Derived> *> _protocol_list; /**< List of created protocols */

public:
    using base_io_t = file_watcher<_Derived>; /**< Base I/O type */
    constexpr static const bool do_read = true; /**< Flag indicating that reading is enabled */

    /**
     * @brief Default constructor
     */
    file_watcher() = default;
    
    /**
     * @brief Constructor with protocol
     * 
     * @param protocol Protocol to use for processing file contents
     */
    file_watcher(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {}
        
    /**
     * @brief Copy constructor (deleted)
     */
    file_watcher(file_watcher const &) = delete;
    
    /**
     * @brief Destructor
     * 
     * Cleans up all created protocols.
     */
    ~file_watcher() noexcept {
        for (auto protocol : _protocol_list)
            delete protocol;
    }

    /**
     * @brief Switch to a new protocol
     * 
     * Creates a new protocol instance and uses it for processing file contents.
     * 
     * @tparam _Protocol Protocol type to create
     * @tparam _Args Types of arguments for protocol construction
     * @param args Arguments for protocol construction
     * @return Pointer to the new protocol, or nullptr on failure
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
     * @brief Start watching a file
     * 
     * @param fpath Path to the file to watch
     * @param ts Interval between checks in seconds
     */
    void
    start(std::string const &fpath, ev_tstamp ts = 0.1) noexcept {
        this->_async_event.start(fpath.c_str(), ts);
    }

    /**
     * @brief Stop watching the file
     */
    void
    disconnect() noexcept {
        this->_async_event.stop();
    }

    /**
     * @brief Read all available data from the file
     * 
     * This method reads all available data from the file and processes it
     * using the protocol. It continues reading until no more data is available
     * or an error occurs.
     * 
     * @return 0 on success, -1 on error
     */
    int
    read_all() {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        std::size_t ret = 0u;
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
     * @brief File event handler
     * 
     * This method is called when a file change event is triggered.
     * It reads the new content if the file has grown.
     * 
     * @param event The file event
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
 * @brief Watches a directory for changes
 * 
 * This template class watches a directory for changes and triggers events
 * when changes occur. Unlike file_watcher, it does not read any content.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class directory_watcher : public base<directory_watcher<_Derived>, event::file> {
    using base_t = base<file_watcher<_Derived>, event::file>;

public:
    using base_io_t = directory_watcher<_Derived>; /**< Base I/O type */
    constexpr static const bool do_read = false; /**< Flag indicating that reading is disabled */

    /**
     * @brief Default constructor
     */
    directory_watcher() = default;
    
    /**
     * @brief Destructor
     */
    ~directory_watcher() = default;

    /**
     * @brief Start watching a directory
     * 
     * @param fpath Path to the directory to watch
     * @param ts Interval between checks in seconds
     */
    void
    start(std::string const &fpath, ev_tstamp ts = 0.1) noexcept {
        this->_async_event.start(fpath.c_str(), ts);
    }

    /**
     * @brief Stop watching the directory
     */
    void
    disconnect() noexcept {
        this->_async_event.stop();
    }

private:
    friend class listener::RegisteredKernelEvent<event::file, directory_watcher>;

    /**
     * @brief Directory event handler
     * 
     * This method is called when a directory change event is triggered.
     * It forwards the event to the derived class if it has an appropriate handler.
     * 
     * @param event The file event
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
 * @brief Handles asynchronous input operations
 * 
 * This template class provides functionality for asynchronous input operations.
 * It reads data from an I/O source and processes it using a protocol.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class input : public base<input<_Derived>, event::io> {
    using base_t = base<input<_Derived>, event::io>;
    AProtocol<_Derived> *_protocol = nullptr; /**< Protocol for processing input */
    std::vector<AProtocol<_Derived> *> _protocol_list; /**< List of created protocols */
    bool _disconnected_by_user = false; /**< Flag indicating user-initiated disconnection */
    bool _on_message = false; /**< Flag indicating message processing in progress */
    bool _is_disposed = false; /**< Flag indicating resource disposal */

public:
    using base_io_t = input<_Derived>; /**< Base I/O type */
    constexpr static const bool has_server = false; /**< Flag indicating server association */

    /**
     * @brief Default constructor
     */
    input() = default;
    
    /**
     * @brief Constructor with protocol
     * 
     * @param protocol Protocol to use for processing input
     */
    input(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {
        _protocol_list.push_back(protocol);
    }
    
    /**
     * @brief Copy constructor (deleted)
     */
    input(input const &) = delete;
    
    /**
     * @brief Destructor
     * 
     * Cleans up all created protocols.
     */
    ~input() noexcept {
        clear_protocols();
    }

    /**
     * @brief Switch to a new protocol
     * 
     * Creates a new protocol instance and uses it for processing input.
     * 
     * @tparam _Protocol Protocol type to create
     * @tparam _Args Types of arguments for protocol construction
     * @param args Arguments for protocol construction
     * @return Pointer to the new protocol, or nullptr on failure
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
     * @brief Clear all protocols
     * 
     * Deletes all created protocols and resets the current protocol.
     */
    void
    clear_protocols() {
        for (auto protocol : _protocol_list)
            delete protocol;
        _protocol_list.clear();
        _protocol = nullptr;
    };

    /**
     * @brief Get the current protocol
     * 
     * @return Pointer to the current protocol
     */
    AProtocol<_Derived> *
    protocol() {
        return _protocol;
    }

    /**
     * @brief Start asynchronous input operations
     * 
     * Sets up non-blocking I/O and starts listening for read events.
     */
    void
    start() noexcept {
        _disconnected_by_user = false;
        Derived.transport().set_nonblocking(true);
        this->_async_event.start(Derived.transport().native_handle(), EV_READ);
    }

    /**
     * @brief Enable read event listening
     * 
     * Makes sure the event is set up to listen for read events.
     */
    void
    ready_to_read() noexcept {
        if (!(this->_async_event.events & EV_READ))
            this->_async_event.start(Derived.transport().native_handle(), EV_READ);
    }

    /**
     * @brief Disconnect the input
     * 
     * Marks the input as disconnected by the user and triggers an undefined event
     * to break out of the event loop.
     * 
     * @param reason Reason code for disconnection
     */
    void
    disconnect(int reason = 0) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            Derived.on(event::disconnected{reason});
        }
        _disconnected_by_user = true;
        listener::current.loop().feed_fd_event(Derived.transport().native_handle(),
                                               EV_UNDEF);
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, input>;

    /**
     * @brief I/O event handler
     * 
     * This method is called when an I/O event is triggered.
     * It reads data and processes it using the protocol.
     * 
     * @param event The I/O event
     */
    void
    on(event::io const &event) {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        std::size_t ret = 0u;
        if (_on_message)
            return;
        if (_disconnected_by_user || !_protocol->ok())
            goto error;

        if (likely(event._revents & EV_READ)) {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                goto error;
            _on_message = true;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                this->_protocol->onMessage(ret);
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
        dispose();
    }

protected:
    /**
     * @brief Dispose of resources
     * 
     * This method is called when an error occurs or when disconnection is requested.
     * It cleans up resources and notifies the server or parent object if applicable.
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;
        if (!_disconnected_by_user)
            disconnect();
        Derived.close();
        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else if constexpr (has_method_on<_Derived, void, event::dispose>::value) {
            Derived.on(event::dispose{});
        }
    }
};

/**
 * @class output
 * @brief Handles asynchronous output operations
 * 
 * This template class provides functionality for asynchronous output operations.
 * It writes data to an I/O destination.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class output : public base<output<_Derived>, event::io> {
    using base_t = base<output<_Derived>, event::io>;
    bool _disconnected_by_user = false; /**< Flag indicating user-initiated disconnection */
    bool _is_disposed = false; /**< Flag indicating resource disposal */

public:
    using base_io_t = output<_Derived>; /**< Base I/O type */
    constexpr static const bool has_server = false; /**< Flag indicating server association */

    /**
     * @brief Default constructor
     */
    output() = default;
    
    /**
     * @brief Copy constructor (deleted)
     */
    output(output const &) = delete;
    
    /**
     * @brief Destructor
     */
    ~output() = default;

    /**
     * @brief Start asynchronous output operations
     * 
     * Sets up non-blocking I/O and starts listening for write events.
     */
    void
    start() noexcept {
        _disconnected_by_user = false;
        Derived.transport().set_nonblocking(true);
        this->_async_event.start(Derived.transport().native_handle(), EV_WRITE);
    }

    /**
     * @brief Enable write event listening
     * 
     * Makes sure the event is set up to listen for write events.
     */
    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE))
            this->_async_event.set(EV_WRITE);
    }

    /**
     * @brief Publish data to the output
     * 
     * Enables write event listening and writes the provided data to the output.
     * 
     * @tparam _Args Types of data to publish
     * @param args Data to publish
     * @return Reference to the output stream
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
     * @brief Stream operator for publishing data
     * 
     * @tparam T Type of data to publish
     * @param data Data to publish
     * @return Reference to the output stream
     */
    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    /**
     * @brief Disconnect the output
     * 
     * Marks the output as disconnected by the user and triggers an undefined event
     * to break out of the event loop.
     * 
     * @param reason Reason code for disconnection
     */
    void
    disconnect(int reason = 0) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            Derived.on(event::disconnected{reason});
        }
        _disconnected_by_user = true;
        listener::current.loop().feed_fd_event(Derived.transport().native_handle(),
                                               EV_UNDEF);
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, output>;

    /**
     * @brief I/O event handler
     * 
     * This method is called when an I/O event is triggered.
     * It writes pending data to the output.
     * 
     * @param event The I/O event
     */
    void
    on(event::io const &event) {
        auto ret = 0;

        if (_disconnected_by_user)
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
        dispose();
    }

protected:
    /**
     * @brief Dispose of resources
     * 
     * This method is called when an error occurs or when disconnection is requested.
     * It cleans up resources and notifies the server or parent object if applicable.
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;
        if (!_disconnected_by_user)
            disconnect();
        Derived.close();
        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else if constexpr (has_method_on<_Derived, void, event::dispose>::value) {
            Derived.on(event::dispose{});
        }
    }
};

/**
 * @class io
 * @brief Handles bidirectional asynchronous I/O operations
 * 
 * This template class provides functionality for bidirectional asynchronous I/O.
 * It combines input and output capabilities for reading from and writing to an I/O source.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class io : public base<io<_Derived>, event::io> {
    using base_t = base<io<_Derived>, event::io>;
    AProtocol<_Derived> *_protocol = nullptr; /**< Protocol for processing I/O */
    std::vector<AProtocol<_Derived> *> _protocol_list; /**< List of created protocols */
    bool _disconnected_by_user = false; /**< Flag indicating user-initiated disconnection */
    bool _on_message = false; /**< Flag indicating message processing in progress */
    bool _is_disposed = false; /**< Flag indicating resource disposal */

public:
    typedef io<_Derived> base_io_t; /**< Base I/O type */
    constexpr static const bool has_server = false; /**< Flag indicating server association */

    /**
     * @brief Default constructor
     */
    io() = default;
    
    /**
     * @brief Constructor with protocol
     * 
     * @param protocol Protocol to use for processing I/O
     */
    io(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {
        _protocol_list.push_back(protocol);
    }
    
    /**
     * @brief Copy constructor (deleted)
     */
    io(io const &) = delete;
    
    /**
     * @brief Destructor
     * 
     * Cleans up all created protocols.
     */
    ~io() noexcept {
        clear_protocols();
    }

    /**
     * @brief Switch to a new protocol
     * 
     * Creates a new protocol instance and uses it for processing I/O.
     * 
     * @tparam _Protocol Protocol type to create
     * @tparam _Args Types of arguments for protocol construction
     * @param args Arguments for protocol construction
     * @return Pointer to the new protocol, or nullptr on failure
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
     * @brief Clear all protocols
     * 
     * Deletes all created protocols and resets the current protocol.
     */
    void
    clear_protocols() {
        for (auto protocol : _protocol_list)
            delete protocol;
        _protocol_list.clear();
        _protocol = nullptr;
    };

    /**
     * @brief Get the current protocol
     * 
     * @return Pointer to the current protocol
     */
    AProtocol<_Derived> *
    protocol() {
        return _protocol;
    }

    /**
     * @brief Start bidirectional asynchronous I/O operations
     * 
     * Sets up non-blocking I/O and starts listening for read events.
     */
    void
    start() noexcept {
        _disconnected_by_user = false;
        Derived.transport().set_nonblocking(true);
        this->_async_event.start(Derived.transport().native_handle(), EV_READ);
        // Derived.in().reserve(32768);
        // Derived.out().reserve(32768);
    }

    /**
     * @brief Enable read event listening
     * 
     * Makes sure the event is set up to listen for read events.
     */
    void
    ready_to_read() noexcept {
        if (!(this->_async_event.events & EV_READ))
            this->_async_event.set(this->_async_event.events | EV_READ);
    }

    /**
     * @brief Enable write event listening
     * 
     * Makes sure the event is set up to listen for write events.
     */
    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE))
            this->_async_event.set(this->_async_event.events | EV_WRITE);
    }

    /**
     * @brief Request closure after delivering pending data
     * 
     * Marks the protocol as invalid, which will cause the connection to close
     * after all pending data has been delivered.
     */
    void
    close_after_deliver() const noexcept {
        _protocol->not_ok();
    }

    /**
     * @brief Publish data to the output
     * 
     * Enables write event listening and writes the provided data to the output.
     * 
     * @tparam _Args Types of data to publish
     * @param args Data to publish
     * @return Reference to the output stream
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
     * @brief Stream operator for publishing data
     * 
     * @tparam T Type of data to publish
     * @param data Data to publish
     * @return Reference to the output stream
     */
    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    /**
     * @brief Disconnect the I/O
     * 
     * Marks the I/O as disconnected by the user and feeds an undefined event
     * to break out of the event loop.
     * 
     * @param reason Reason code for disconnection
     */
    void
    disconnect(int reason = 0) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            Derived.on(event::disconnected{reason});
        }
        _disconnected_by_user = true;
        this->_async_event.feed_event(EV_UNDEF);
    }
private:
    friend class listener::RegisteredKernelEvent<event::io, io>;

    /**
     * @brief I/O event handler
     * 
     * This method is called when an I/O event is triggered.
     * It handles both read and write events.
     * 
     * @param event The I/O event
     */
    void
    on(event::io const &event) {
        constexpr const std::size_t invalid_ret = static_cast<std::size_t>(-1);
        std::size_t ret = 0u;
        bool ok = false;

        if (_on_message)
            return;
        if (_disconnected_by_user)
            goto error;
        if (event._revents & EV_READ && _protocol->ok()) {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                goto error;

            _on_message = true;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                // prevent recall in message
                this->_protocol->onMessage(ret);
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
        dispose();
    }

protected:
    /**
     * @brief Dispose of resources
     * 
     * This method is called when an error occurs or when disconnection is requested.
     * It cleans up resources and notifies the server or parent object if applicable.
     */
    void
    dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;
        if (!_disconnected_by_user)
            disconnect();
        Derived.close();
        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(Derived.id());
        } else if constexpr (has_method_on<_Derived, void, event::dispose>::value) {
            Derived.on(event::dispose{});
        }
    }
};

} // namespace qb::io::async

#undef Derived
#endif // QB_IO_ASYNC_IO_H
