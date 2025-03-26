/**
 * @file qb/io/async/protocol.h
 * @brief Protocol interfaces for message processing in the asynchronous IO framework
 * 
 * This file defines the protocol interfaces used for message processing in the
 * asynchronous IO framework. It provides base classes for implementing custom
 * protocols that can parse and process messages from IO streams.
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
 * @class IProtocol
 * @brief Base interface for all protocols
 * 
 * This interface defines the basic methods that all protocols must implement
 * for message parsing and processing.
 */
class IProtocol {
public:
    /**
     * @brief Virtual destructor
     */
    virtual ~IProtocol() = default;
    
    /**
     * @brief Get the size of the next complete message in the buffer
     * 
     * This method should examine the current buffer contents and determine
     * if a complete message is available. If so, it should return the size
     * of that message. If no complete message is available, it should return 0.
     * 
     * @return Size of the next complete message, or 0 if no complete message is available
     */
    virtual std::size_t getMessageSize() noexcept = 0;
    
    /**
     * @brief Process a complete message
     * 
     * This method is called when a complete message has been identified.
     * It should process the message of the given size from the current
     * buffer position.
     * 
     * @param size Size of the message to process
     */
    virtual void onMessage(std::size_t size) noexcept = 0;
    
    /**
     * @brief Reset the protocol state
     * 
     * This method should reset any internal state of the protocol,
     * preparing it to start parsing from the beginning of a new message.
     */
    virtual void reset() noexcept = 0;
};

/**
 * @class AProtocol
 * @brief Abstract base class for IO-specific protocols
 * 
 * This template class extends the IProtocol interface with IO-specific
 * functionality. It serves as a base class for protocols that need access
 * to the underlying IO object.
 * 
 * @tparam _IO_ The IO type that this protocol works with
 */
template <typename _IO_>
class AProtocol : public IProtocol {
    friend typename _IO_::base_io_t;

    bool _status = true; /**< Protocol status flag */
    
protected:
    _IO_ &_io;  /**< Reference to the IO object */

    /**
     * @brief Default constructor is deleted
     */
    AProtocol() = delete;
    
    /**
     * @brief Constructor
     * @param io Reference to the IO object
     */
    AProtocol(_IO_ &io) noexcept
        : _io(io) {}
        
    /**
     * @brief Virtual destructor
     */
    virtual ~AProtocol() = default;
    
    /**
     * @brief Get the size of the next complete message in the buffer
     * @return Size of the next complete message, or 0 if no complete message is available
     */
    virtual std::size_t getMessageSize() noexcept = 0;
    
    /**
     * @brief Process a complete message
     * @param size Size of the message to process
     */
    virtual void onMessage(std::size_t size) noexcept = 0;
    
    /**
     * @brief Reset the protocol state
     */
    virtual void reset() noexcept = 0;

public:
    /**
     * @brief Check if the protocol is in a valid state
     * @return true if the protocol is valid, false otherwise
     */
    [[nodiscard]] bool
    ok() const noexcept {
        return _status;
    }

    /**
     * @brief Mark the protocol as invalid
     * 
     * This method can be called to indicate that the protocol has
     * encountered an error and is no longer in a valid state.
     */
    void not_ok() noexcept {
        _status = false;
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_PROTOCOL_H
