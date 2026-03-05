/**
 * @file qb/io/async/protocol.h
 * @brief Protocol interfaces for message processing in the asynchronous IO framework.
 *
 * This file defines the protocol interfaces used for message processing in the
 * asynchronous IO framework. It provides base classes for implementing custom
 * protocols that can parse and process messages from IO streams.
 * These protocols are crucial for defining how raw byte streams are interpreted as
 * distinct application-level messages.
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

#ifndef QB_IO_ASYNC_PROTOCOL_H
#define QB_IO_ASYNC_PROTOCOL_H

#include <qb/utility/type_traits.h>

namespace qb::io::async {

/**
 * @interface IProtocol
 * @ingroup Protocol
 * @brief Base interface for all message processing protocols.
 *
 * This interface defines the essential methods that all protocol implementations
 * must provide for message framing (identifying message boundaries in a byte stream)
 * and processing.
 */
class IProtocol {
    bool _status = true; /**< Protocol status flag. `true` if the protocol is in a valid operational state, `false` otherwise (e.g., after a parsing error). */
    bool _should_flush = true; /**< Flag indicating whether the protocol should flush the input buffer after processing a message. */
public:
    /**
     * @brief Virtual destructor.
     * Ensures proper cleanup for derived protocol classes.
     */
    virtual ~IProtocol() = default;

    /**
     * @brief Determines the size of the next complete message in the input buffer.
     *
     * This method must be implemented by concrete protocols to examine the current
     * input buffer (typically accessed via the associated I/O component) and determine
     * if a complete message is available according to the protocol's framing rules.
     *
     * @return Size of the next complete message in bytes (including any headers or delimiters
     *         that are part of the message unit). Returns `0` if no complete message is yet
     *         available in the buffer, indicating more data is needed.
     * 
     * @note **Inspection Only:** This method should not consume data from the buffer; it only inspects it.
     *       The actual consumption happens when `onMessage()` is called with the returned size.
     * 
     * @note **Error Handling:** If the protocol detects an invalid message format during inspection
     *       (e.g., malformed header, invalid size field), it should call `not_ok()` to mark itself
     *       as invalid. The I/O component will then detect this and trigger appropriate error handling.
     * 
     * @note **Performance:** This method is called frequently in the hot path (potentially for every
     *       event loop iteration when data is available). Implementations should be optimized for speed
     *       and avoid unnecessary allocations or complex computations.
     * 
     * @note **Message Size Limits:** The I/O component enforces a maximum message size (`QB_MAX_MESSAGE_SIZE`)
     *       to prevent DoS attacks. If a protocol returns a size exceeding this limit, the I/O component
     *       will automatically mark the protocol as `not_ok()` and trigger disconnection with `reason = 3`.
     */
    virtual std::size_t getMessageSize() noexcept = 0;

    /**
     * @brief Processes a complete message that has been identified in the input buffer.
     *
     * This method is called by the framework when `getMessageSize()` has returned a non-zero size.
     * The implementation should process the message of the given `size` starting from the
     * beginning of the current input buffer. After processing, the derived I/O component
     * is usually responsible for flushing these `size` bytes from its input buffer.
     *
     * @param size The size of the complete message to process, as determined by `getMessageSize()`.
     * 
     * @note **Error Handling:** If the protocol encounters an error during message processing
     *       (e.g., invalid message format, parsing failure), it should call `not_ok()` to mark
     *       itself as invalid. This will cause the I/O component to detect the error and trigger
     *       a disconnection with appropriate error reporting via `event::disconnected`.
     * 
     * @note **Message Dispatch:** The protocol should typically dispatch the parsed message
     *       to a handler in its associated I/O component (e.g., by calling `_io.on(MyProtocol::message{...})`).
     * 
     * @note **Thread Safety:** This method is called from the event loop within a single VirtualCore
     *       (single-threaded context), so no synchronization is needed. However, the protocol
     *       should ensure that message processing is atomic and does not leave the protocol
     *       in an inconsistent state if an error occurs.
     */
    virtual void onMessage(std::size_t size) noexcept = 0;

    /**
     * @brief Resets the internal state of the protocol.
     *
     * This method should be called to clear any partial parsing state, preparing the protocol
     * to start parsing a new message from a fresh state. This is important after errors,
     * disconnections, or when switching protocols.
     * 
     * @note **Error Recovery:** After calling `reset()`, the protocol's status (`ok()`) is not
     *       automatically restored. If the protocol was marked as `not_ok()`, it will remain
     *       in that state. To fully recover, a new protocol instance should typically be created.
     * 
     * @note **Usage:** This method is typically called:
     *       - When switching to a new protocol instance
     *       - After a disconnection to prepare for reconnection
     *       - When explicitly resetting the protocol state (though this is less common)
     * 
     * @note **Implementation:** Derived protocols should reset all internal parsing state,
     *       including any partial message buffers, state machines, or parsing flags.
     *       However, configuration settings (like `_should_flush`) should typically be preserved.
     */
    virtual void reset() noexcept = 0;

public:
    /**
     * @brief Checks if the protocol is in a valid operational state.
     * @return `true` if the protocol is considered okay and can continue processing,
     *         `false` if it has encountered an unrecoverable error or has been marked as not okay.
     */
    [[nodiscard]] bool
    ok() const noexcept {
        return _status;
    }

    /**
     * @brief Marks the protocol as being in an invalid or non-operational state.
     * @details This method can be called by the protocol implementation (or externally)
     *          to indicate that it has encountered an unrecoverable parsing error or that
     *          the connection should be closed after processing any pending data.
     *          The I/O component might check this status via `ok()`.
     * 
     * @note **Error Handling:** When a protocol calls `not_ok()`, the I/O component will
     *       detect this during message processing (via `protocol->ok()`) and trigger
     *       a disconnection with `reason = 2` (Protocol error). The `event::disconnected`
     *       event will be dispatched to the actor, allowing it to handle the error appropriately.
     * 
     * @note **Usage:** Protocols should call `not_ok()` when they encounter:
     *       - Invalid message format that cannot be recovered
     *       - Parsing errors that indicate protocol violation
     *       - Security violations (e.g., unauthorized access attempts)
     *       - Any condition that requires connection termination
     * 
     * @note **Recovery:** Once `not_ok()` is called, the protocol cannot be recovered.
     *       The I/O component will initiate disconnection. If recovery is needed,
     *       a new protocol instance should be created via `switch_protocol()`.
     */
    void
    not_ok() noexcept {
        _status = false;
    }

    /**
     * @brief Sets the flag indicating whether the protocol should flush the input buffer after processing a message.
     * @param should_flush `true` if the protocol should flush the input buffer after processing a message, `false` otherwise.
     */
    void set_should_flush(bool should_flush) noexcept {
        _should_flush = should_flush;
    }

    /**
     * @brief Gets the flag indicating whether the protocol should flush the input buffer after processing a message.
     * @return `true` if the protocol should flush the input buffer after processing a message, `false` otherwise.
     */
    bool should_flush() const noexcept {
        return _should_flush;
    }
};

/**
 * @class AProtocol
 * @ingroup Protocol
 * @brief Abstract base class for I/O-component-aware protocols (CRTP).
 *
 * This template class extends the `IProtocol` interface and is designed to be used
 * with the Curiously Recurring Template Pattern (CRTP). It provides the protocol
 * implementation with a reference (`_io`) to its associated I/O component (`_IO_`),
 * allowing the protocol to interact with the I/O component's buffers and dispatch
 * parsed messages to its handlers.
 *
 * @tparam _IO_ The I/O component type (e.g., `MyTcpSession`) that this protocol works with.
 *              This `_IO_` type is expected to provide access to input/output buffers (e.g., `_io.in()`, `_io.out()`)
 *              and an `on(typename ProtocolType::message&&)` handler for received messages.
 */
template <typename _IO_>
class AProtocol : public IProtocol {
    /**
     * @brief Friend declaration for the base I/O class of the associated I/O component
     * @details 
     * This friendship declaration allows the base I/O template class of the associated I/O component
     * to access protected members of this protocol. The base_io_t is typically defined within the
     * I/O component class (_IO_) as an alias to one of several possible base template classes:
     * - qb::io::async::io<_Derived>
     * - qb::io::async::file_watcher<_Derived>
     * - qb::io::async::directory_watcher<_Derived>
     * - qb::io::async::input<_Derived>
     * - qb::io::async::output<_Derived>
     * - qb::io::async::tcp::client<_Derived, _Transport, _Server>
     * 
     * The exact base_io_t is determined at compile time based on the template parameter _IO_.
     */
    friend typename _IO_::base_io_t;


protected:
    _IO_ &_io; /**< Reference to the I/O component instance that this protocol is associated with. */

    /**
     * @brief Default constructor is deleted to ensure an I/O component is always associated.
     */
    AProtocol() = delete;

    /**
     * @brief Constructor that associates the protocol with an I/O component.
     * @param io Reference to the I/O component (`_IO_&`) that will use this protocol instance.
     */
    AProtocol(_IO_ &io) noexcept
        : _io(io) {}

    /**
     * @brief Virtual destructor.
     */
    virtual ~AProtocol() = default;

    // Pure virtual methods inherited from IProtocol, to be implemented by concrete protocols.

    /**
     * @brief Determines the size of the next complete message in the input buffer of the associated I/O component.
     * @return Size of the next complete message in bytes, or `0` if not enough data is available.
     * @details Concrete protocols must implement this to define their message framing logic by inspecting `this->_io.in()`.
     * @see IProtocol::getMessageSize()
     */
    virtual std::size_t getMessageSize() noexcept = 0;

    /**
     * @brief Processes a complete message from the input buffer of the associated I/O component.
     * @param size The size of the complete message to process.
     * @details Concrete protocols must implement this to parse the message and typically call `this->_io.on(typename ConcreteProtocol::message{...})`.
     * @see IProtocol::onMessage()
     */
    virtual void onMessage(std::size_t size) noexcept = 0;

    /**
     * @brief Resets the internal parsing state of the protocol.
     * @see IProtocol::reset()
     */
    virtual void reset() noexcept = 0;


};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_PROTOCOL_H