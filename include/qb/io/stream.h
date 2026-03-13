/**
 * @file qb/io/stream.h
 * @brief Core stream abstraction classes for the QB IO library
 *
 * This file provides the fundamental stream abstraction classes that serve
 * as the foundation for all transport implementations in the QB IO library.
 * It defines three template classes:
 * - istream: Input stream functionality
 * - ostream: Output stream functionality
 * - stream: Combined input/output stream functionality
 *
 * These templates are parameterized by an IO type that implements the actual
 * transport-specific operations.
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

#ifndef QB_IO_STREAM_H_
#define QB_IO_STREAM_H_
#include <qb/io/config.h>
#include <qb/system/allocator/pipe.h>
#include <qb/utility/type_traits.h>

namespace qb::io {

/**
 * @class istream
 * @brief Input stream template class
 *
 * This template class provides input stream functionality for various
 * transport implementations. It manages an input buffer and provides
 * methods for reading data from the underlying IO object into the buffer.
 *
 * @tparam _IO_ The IO type that implements the actual transport operations
 */
template <typename _IO_>
class istream {
public:
    /** @brief Type of the underlying transport IO */
    using transport_io_type = _IO_;

    /** @brief Type of the input buffer */
    using input_buffer_type = qb::allocator::pipe<char>;

protected:
    _IO_              _in;        /**< The underlying IO object */
    input_buffer_type _in_buffer; /**< Buffer for incoming data */
    std::size_t       _max_read_buffer_size = static_cast<std::size_t>(-1); /**< Maximum allowed size for the input buffer (DoS protection). -1 (SIZE_MAX) = unlimited (default). Configurable at runtime. */

public:
    /**
     * @brief Destructor
     *
     * Ensures that the stream is closed properly when destroyed.
     */
    ~istream() noexcept {
        close();
    }

    /**
     * @brief Get the underlying transport object
     * @return Reference to the transport object
     */
    [[nodiscard]] _IO_ &
    transport() noexcept {
        return _in;
    }

    /**
     * @brief Get the underlying transport object (const version)
     * @return Const reference to the transport object
     */
    [[nodiscard]] const _IO_ &
    transport() const noexcept {
        return _in;
    }

    /**
     * @brief Get the input buffer
     * @return Reference to the input buffer
     */
    [[nodiscard]] input_buffer_type &
    in() noexcept {
        return _in_buffer;
    }

    /**
     * @brief Get the number of bytes available for reading
     * @return Number of bytes in the input buffer
     */
    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _in_buffer.size();
    }

    /**
     * @brief Get the maximum allowed size for the input buffer
     * @return Maximum buffer size in bytes, or SIZE_MAX (-1) if unlimited
     */
    [[nodiscard]] std::size_t
    max_read_buffer_size() const noexcept {
        return _max_read_buffer_size;
    }

    /**
     * @brief Set the maximum allowed size for the input buffer
     * @param size Maximum buffer size in bytes, or -1 (SIZE_MAX) for unlimited
     * @note Setting this to -1 (SIZE_MAX) disables the limit (default).
     *       This is a security-critical setting that prevents DoS attacks via buffer exhaustion.
     */
    void
    set_max_read_buffer_size(std::size_t size) noexcept {
        _max_read_buffer_size = size;
    }

    /**
     * @brief Read data from the transport into the input buffer
     * @return Number of bytes read on success, error code on failure
     *
     * This method is enabled only if the IO type has a compatible read method.
     * It reads data in fixed-size chunks and adjusts the buffer size based
     * on the actual number of bytes read.
     * 
     * @note **Security:** If the buffer size would exceed `max_read_buffer_size()` after
     *       this read, the operation fails with error code -2 to prevent DoS attacks.
     */
    template <typename Available = void>
    [[nodiscard]] int
    read(std::enable_if_t<has_method_read<_IO_, int, char *, std::size_t>::value,
                          Available> * = nullptr) noexcept {
        // Use safe buffer size that won't overflow when cast to 32-bit for socket APIs
        constexpr std::size_t bucket_read = QB_DEFAULT_READ_BUFFER_SIZE;
        static_assert(bucket_read <= QB_MAX_IO_SIZE, "Buffer size exceeds safe I/O limits");
        
        // Security check: prevent DoS via buffer exhaustion
        // We only check the actual data size, not the capacity. The pipe may resize internally,
        // but what matters is the actual number of bytes present in the buffer.
        // If _max_read_buffer_size == SIZE_MAX (-1), the comparison will always be false (unlimited).
        if (_in_buffer.size() + bucket_read > _max_read_buffer_size) {
            return -2; // Special error code for buffer size limit exceeded
        }
        
        // Clamp to max I/O size to prevent integer overflow in platform APIs
        const std::size_t read_size = (bucket_read > QB_MAX_IO_SIZE) ? QB_MAX_IO_SIZE : bucket_read;
        
        const auto ret = _in.read(_in_buffer.allocate_back(read_size), read_size);
        if (ret >= 0)
            _in_buffer.free_back(read_size - static_cast<std::size_t>(ret));
        else
            _in_buffer.free_back(read_size); // Release entire reservation on read failure (e.g. WSAEWOULDBLOCK)
        return ret;
    }

    /**
     * @brief Remove data from the front of the input buffer
     * @param size Number of bytes to remove
     *
     * This method is typically called after processing data from the
     * input buffer to free up space.
     */
    void
    flush(std::size_t size) noexcept {
        _in_buffer.free_front(size);
    }

    /**
     * @brief Handle end-of-file condition
     *
     * Resets or reorders the input buffer based on whether it contains data.
     */
    void
    eof() noexcept {
        if (!_in_buffer.size())
            _in_buffer.reset();
        else
            _in_buffer.reorder();
    }

    /**
     * @brief Close the stream
     *
     * Resets the input buffer and closes or disconnects the underlying
     * transport, depending on the available methods.
     */
    void
    close() noexcept {
        _in_buffer.reset();
        if constexpr (has_member_func_disconnect<_IO_>::value)
            _in.disconnect();
        _in.close();
    }
};

/**
 * @class ostream
 * @brief Output stream template class
 *
 * This template class provides output stream functionality for various
 * transport implementations. It manages an output buffer and provides
 * methods for writing data from the buffer to the underlying IO object.
 *
 * @tparam _IO_ The IO type that implements the actual transport operations
 */
template <typename _IO_>
class ostream {
public:
    /** @brief Type of the underlying transport IO */
    using transport_io_type = _IO_;

    /** @brief Type of the output buffer */
    using output_buffer_type = qb::allocator::pipe<char>;

protected:
    _IO_               _out;        /**< The underlying IO object */
    output_buffer_type _out_buffer; /**< Buffer for outgoing data */
    std::size_t        _max_write_buffer_size = static_cast<std::size_t>(-1); /**< Maximum allowed size for the output buffer (DoS protection). -1 (SIZE_MAX) = unlimited (default). Configurable at runtime. */

public:
    /**
     * @brief Destructor
     *
     * Ensures that the stream is closed properly when destroyed.
     */
    ~ostream() noexcept {
        close();
    }

    /**
     * @brief Get the underlying transport object
     * @return Reference to the transport object
     */
    [[nodiscard]] _IO_ &
    transport() noexcept {
        return _out;
    }

    /**
     * @brief Get the underlying transport object (const version)
     * @return Const reference to the transport object
     */
    [[nodiscard]] const _IO_ &
    transport() const noexcept {
        return _out;
    }

    /**
     * @brief Get the output buffer
     * @return Reference to the output buffer
     */
    [[nodiscard]] output_buffer_type &
    out() noexcept {
        return _out_buffer;
    }

    /**
     * @brief Get the number of bytes pending for writing
     * @return Number of bytes in the output buffer
     */
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out_buffer.size();
    }

    /**
     * @brief Write data from the output buffer to the transport
     * @return Number of bytes written on success, error code on failure
     *
     * This method is enabled only if the IO type has a compatible write method.
     * It writes the entire buffer content and adjusts or resets the buffer
     * based on the actual number of bytes written.
     */
    template <typename Available = void>
    [[nodiscard]] int
    write(std::enable_if_t<has_method_write<_IO_, int, const char *, std::size_t>::value,
                           Available> * = nullptr) noexcept {
        const auto ret = _out.write(_out_buffer.begin(), _out_buffer.size());

        if (ret > 0) {
            if (ret != _out_buffer.size()) {
                _out_buffer.free_front(ret);
                _out_buffer.reorder();
            } else
                _out_buffer.reset();
        }

        return ret;
    }

    /**
     * @brief Add data to the output buffer for later writing
     * @param data Pointer to the data to add
     * @param size Size of the data to add
     * @return Pointer to the copied data in the output buffer
     *
     * Copies the specified data to the output buffer for later
     * transmission by the write method.
     */
    char *
    publish(char const *data, std::size_t size) noexcept {
        // Security check: prevent DoS via buffer exhaustion
        // We only check the actual data size, not the capacity. The pipe may resize internally,
        // but what matters is the actual number of bytes present in the buffer.
        // If _max_write_buffer_size == SIZE_MAX (-1), the comparison will always be false (unlimited).
        if (_out_buffer.size() + size > _max_write_buffer_size) {
            return nullptr; // Buffer limit would be exceeded
        }
        
        return static_cast<char *>(
            std::memcpy(_out_buffer.allocate_back(size), data, size));
    }

    /**
     * @brief Close the stream
     *
     * Resets the output buffer and closes or disconnects the underlying
     * transport, depending on the available methods.
     */
    void
    close() noexcept {
        _out_buffer.reset();
        if constexpr (has_member_func_disconnect<_IO_>::value)
            _out.disconnect();
        _out.close();
    }
};

/**
 * @class stream
 * @brief Combined input/output stream template class
 *
 * This template class provides both input and output stream functionality
 * for various transport implementations. It inherits from istream for input
 * operations and adds output buffer management and writing methods.
 *
 * This is the primary base class for most transport implementations in the library.
 *
 * @tparam _IO_ The IO type that implements the actual transport operations
 */
template <typename _IO_>
class stream : public istream<_IO_> {
public:
    /** @brief Type of the underlying transport IO */
    using transport_io_type = _IO_;

    /** @brief Type of the output buffer */
    using output_buffer_type = qb::allocator::pipe<char>;

    /**
     * @brief Flag indicating whether the implementation resets pending reads
     *
     * This flag is used by derived classes to indicate if they need special
     * handling for pending read operations. Default is false.
     */
    constexpr static const bool has_reset_on_pending_read = false;

protected:
    output_buffer_type _out_buffer; /**< Buffer for outgoing data */
    std::size_t        _max_write_buffer_size = static_cast<std::size_t>(-1); /**< Maximum allowed size for the output buffer (DoS protection). -1 (SIZE_MAX) = unlimited (default). Configurable at runtime. */

public:
    /**
     * @brief Get the output buffer
     * @return Reference to the output buffer
     */
    [[nodiscard]] output_buffer_type &
    out() noexcept {
        return _out_buffer;
    }

    /**
     * @brief Get the number of bytes pending for writing
     * @return Number of bytes in the output buffer
     */
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out_buffer.size();
    }

    /**
     * @brief Write data from the output buffer to the transport
     * @return Number of bytes written on success, error code on failure
     *
     * This method is enabled only if the IO type has a compatible write method.
     * It writes the entire buffer content and adjusts or resets the buffer
     * based on the actual number of bytes written.
     *
     * Note that this implementation uses the input IO object (_in) for writing,
     * which is suitable for bidirectional transports like sockets.
     */
    template <typename Available = void>
    [[nodiscard]] int
    write(std::enable_if_t<has_method_write<_IO_, int, const char *, std::size_t>::value,
                           Available> * = nullptr) noexcept {
        const auto ret = this->_in.write(_out_buffer.begin(), _out_buffer.size());
        if (ret > 0) {
            if (static_cast<std::size_t>(ret) != _out_buffer.size()) {
                _out_buffer.free_front(ret);
                _out_buffer.reorder();
            } else
                _out_buffer.reset();
        }
        return ret;
    }

    /**
     * @brief Get the maximum allowed size for the output buffer
     * @return Maximum buffer size in bytes, or SIZE_MAX (-1) if unlimited
     */
    [[nodiscard]] std::size_t
    max_write_buffer_size() const noexcept {
        return _max_write_buffer_size;
    }

    /**
     * @brief Set the maximum allowed size for the output buffer
     * @param size Maximum buffer size in bytes, or -1 (SIZE_MAX) for unlimited
     * @note Setting this to -1 (SIZE_MAX) disables the limit (default).
     *       This is a security-critical setting that prevents DoS attacks via buffer exhaustion.
     */
    void
    set_max_write_buffer_size(std::size_t size) noexcept {
        _max_write_buffer_size = size;
    }

    /**
     * @brief Add data to the output buffer for later writing
     * @param data Pointer to the data to add
     * @param size Size of the data to add
     * @return Pointer to the copied data in the output buffer, or nullptr if buffer limit would be exceeded
     *
     * Copies the specified data to the output buffer for later
     * transmission by the write method.
     * 
     * @note **Security:** If the buffer size would exceed `max_write_buffer_size()` after
     *       this operation, returns nullptr to prevent DoS attacks. The caller should
     *       handle this case appropriately (e.g., disconnect the connection).
     */
    char *
    publish(char const *data, std::size_t size) noexcept {
        // Security check: prevent DoS via buffer exhaustion
        // We only check the actual data size, not the capacity. The pipe may resize internally,
        // but what matters is the actual number of bytes present in the buffer.
        // If _max_write_buffer_size == SIZE_MAX (-1), the comparison will always be false (unlimited).
        if (_out_buffer.size() + size > _max_write_buffer_size) {
            return nullptr; // Buffer limit would be exceeded
        }
        
        return static_cast<char *>(
            std::memcpy(_out_buffer.allocate_back(size), data, size));
    }

    /**
     * @brief Close the stream
     *
     * Resets the output buffer and closes the underlying input stream.
     */
    void
    close() noexcept {
        _out_buffer.reset();
        static_cast<istream<_IO_> &>(*this).close();
    }
};

} // namespace qb::io

#endif // QB_IO_STREAM_H_
