/**
 * @file qb/io/protocol/json.h
 * @brief JSON protocol implementations for the QB IO system.
 *
 * This file contains protocol implementations for parsing and handling JSON messages.
 * It leverages the `nlohmann/json` library for JSON manipulation and provides
 * protocols for handling null-terminated JSON strings and MessagePack encoded JSON.
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

#ifndef QB_IO_PROTOCOL_JSON_H
#define QB_IO_PROTOCOL_JSON_H
#include <cstdint>
#include <utility>
#include <vector>
#include "base.h"
// Angle form, not quote: this is a third-party header that a consumer may legitimately supply
// themselves (see qbDependencies.cmake, find_package(nlohmann_json)). The quote form would search
// this file's own directory first, which is neither where it lives nor where it should be found.
#include <nlohmann/json.hpp>

namespace qb {
namespace protocol {

namespace detail {
/**
 * @brief Default maximum JSON nesting depth accepted by the json protocol.
 * @details nlohmann::json's parser is recursive-descent: a payload made of
 *          thousands of nested `[`/`{` exhausts the stack (DoS) before any
 *          message-size limit applies. 512 is far above any sane document.
 */
inline constexpr std::size_t kJsonMaxNestingDepth = 512;

/**
 * @brief Linear, string-aware pre-scan that bounds JSON nesting depth.
 * @return true when the maximum nesting depth stays within @p max_depth.
 */
inline bool
json_depth_within(const char *data, std::size_t size, std::size_t max_depth) noexcept {
    std::size_t depth     = 0;
    bool        in_string = false;
    bool        escaped   = false;
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        switch (c) {
            case '"':
                in_string = true;
                break;
            case '{':
            case '[':
                if (++depth > max_depth)
                    return false;
                break;
            case '}':
            case ']':
                if (depth)
                    --depth;
                break;
            default:
                break;
        }
    }
    return true;
}

/**
 * @brief Minimal SAX consumer that only validates container nesting depth.
 * @details Implements the nlohmann json_sax interface (duck-typed) but builds
 *          no DOM — every scalar callback is a no-op; start_array/start_object
 *          return false once @ref kJsonMaxNestingDepth is exceeded. Driving a
 *          MessagePack decode through this aborts the recursive binary reader
 *          before it can exhaust the stack (a try/catch cannot recover a stack
 *          overflow).
 */
struct msgpack_depth_sax {
    using number_integer_t  = ::nlohmann::json::number_integer_t;
    using number_unsigned_t = ::nlohmann::json::number_unsigned_t;
    using number_float_t    = ::nlohmann::json::number_float_t;
    using string_t          = ::nlohmann::json::string_t;
    using binary_t          = ::nlohmann::json::binary_t;

    std::size_t depth = 0;
    std::size_t max_depth;
    explicit msgpack_depth_sax(std::size_t md) noexcept
        : max_depth(md) {}

    bool
    null() noexcept {
        return true;
    }
    bool
    boolean(bool) noexcept {
        return true;
    }
    bool
    number_integer(number_integer_t) noexcept {
        return true;
    }
    bool
    number_unsigned(number_unsigned_t) noexcept {
        return true;
    }
    bool
    number_float(number_float_t, const string_t &) noexcept {
        return true;
    }
    bool
    string(string_t &) noexcept {
        return true;
    }
    bool
    binary(binary_t &) noexcept {
        return true;
    }
    bool
    key(string_t &) noexcept {
        return true;
    }
    bool
    start_object(std::size_t) noexcept {
        return ++depth <= max_depth;
    }
    bool
    end_object() noexcept {
        if (depth)
            --depth;
        return true;
    }
    bool
    start_array(std::size_t) noexcept {
        return ++depth <= max_depth;
    }
    bool
    end_array() noexcept {
        if (depth)
            --depth;
        return true;
    }
    template <typename Ex>
    bool
    parse_error(std::size_t, const std::string &, const Ex &) noexcept {
        return false;
    }
};

/**
 * @brief Returns true if the MessagePack-encoded buffer nests no deeper than
 *        @p max_depth (and is structurally parseable).
 */
inline bool
msgpack_depth_within(const char *data, std::size_t size, std::size_t max_depth) noexcept {
    msgpack_depth_sax sax(max_depth);
    return ::nlohmann::json::sax_parse(std::string_view(data, size), &sax, ::nlohmann::json::input_format_t::msgpack,
                                       /*strict=*/true, /*ignore_comments=*/false);
}

/**
 * @brief Resumable scanner that frames exactly one MessagePack value -- the framing of `json_packed`.
 * @details MessagePack is self-delimiting: every value states its own length in its header (a string, a
 *          binary or an extension its byte count, a container its element count, every other type a size its
 *          type byte fixes). The scanner reads those headers and steps over each payload by its length, never
 *          looking inside it, so a zero byte INSIDE a value -- the integer 0, a length byte, a float, a binary or
 *          string byte -- is data, which a scan for a terminator byte cannot tell from the terminator.
 *
 *          It keeps the remaining element count of every open container, so it stops at the start of the first
 *          value whose bytes have not all arrived and resumes there on the next call: each call costs the
 *          headers it has not read yet, and a long string arriving in many reads is skipped, not re-scanned. It
 *          refuses, in the same pass, the byte MessagePack never uses (0xc1) and a container nested deeper than
 *          the bound -- the depth `msgpack_depth_within` checks, counted the same way (an empty container
 *          counts), so the recursive `from_msgpack` that decodes the framed value is bounded before it runs.
 *          It validates framing only: the decode still rejects what framing cannot see.
 */
class msgpack_value_scanner {
public:
    /** @brief What `scan()` found. */
    enum class status {
        incomplete, /**< The value is not whole yet: more bytes are needed. */
        complete,   /**< One whole value: it spans `[0, end())`. */
        invalid     /**< Not MessagePack (0xc1), or nested deeper than the bound. */
    };

    /**
     * @brief Advances over `data[0, size)` from where the previous call stopped.
     * @param data      The buffer, the value starting at `data[0]`; bytes are only ever appended between calls.
     * @param size      The number of bytes in the buffer.
     * @param max_depth The deepest container nesting accepted.
     * @return The status; once `complete`, every later call returns `complete` until `reset()`.
     */
    status
    scan(const unsigned char *data, std::size_t size, std::size_t max_depth) noexcept {
        if (_offset > size) // the buffer was consumed under the scanner: start over
            reset();
        if (_complete)
            return status::complete;
        while (_offset < size) {
            const unsigned char type      = data[_offset];
            std::size_t         width     = 0; // bytes of the big-endian length or count after the type byte
            std::size_t         extra     = 0; // the type byte of a variable-length extension
            std::uint64_t       body      = 0; // payload bytes after the header
            std::uint64_t       items     = 0; // values a container holds (a map: keys and values)
            bool                container = false;
            bool                pairs     = false;
            if (type <= 0x7fu || type >= 0xe0u) {
                // positive / negative fixint: the type byte is the value
            } else if (type <= 0x8fu) {
                container = true;
                items     = 2u * (type & 0x0fu); // fixmap
            } else if (type <= 0x9fu) {
                container = true;
                items     = type & 0x0fu; // fixarray
            } else if (type <= 0xbfu) {
                body = type & 0x1fu; // fixstr
            } else {
                switch (type) {
                    case 0xc0: // nil
                    case 0xc2: // false
                    case 0xc3: // true
                        break;
                    case 0xc1: // never used
                        return status::invalid;
                    case 0xc4: // bin 8 / 16 / 32
                    case 0xd9: // str 8 / 16 / 32
                        width = 1;
                        break;
                    case 0xc5:
                    case 0xda:
                        width = 2;
                        break;
                    case 0xc6:
                    case 0xdb:
                        width = 4;
                        break;
                    case 0xc7: // ext 8 / 16 / 32: a length, a type byte, then the data
                        width = 1;
                        extra = 1;
                        break;
                    case 0xc8:
                        width = 2;
                        extra = 1;
                        break;
                    case 0xc9:
                        width = 4;
                        extra = 1;
                        break;
                    case 0xcc: // uint 8, int 8
                    case 0xd0:
                        body = 1;
                        break;
                    case 0xcd: // uint 16, int 16
                    case 0xd1:
                        body = 2;
                        break;
                    case 0xca: // float 32, uint 32, int 32
                    case 0xce:
                    case 0xd2:
                        body = 4;
                        break;
                    case 0xcb: // float 64, uint 64, int 64
                    case 0xcf:
                    case 0xd3:
                        body = 8;
                        break;
                    case 0xd4: // fixext 1 / 2 / 4 / 8 / 16: a type byte, then the data
                        body = 2;
                        break;
                    case 0xd5:
                        body = 3;
                        break;
                    case 0xd6:
                        body = 5;
                        break;
                    case 0xd7:
                        body = 9;
                        break;
                    case 0xd8:
                        body = 17;
                        break;
                    case 0xdc: // array 16 / 32
                        width     = 2;
                        container = true;
                        break;
                    case 0xdd:
                        width     = 4;
                        container = true;
                        break;
                    case 0xde: // map 16 / 32
                        width     = 2;
                        container = true;
                        pairs     = true;
                        break;
                    case 0xdf:
                        width     = 4;
                        container = true;
                        pairs     = true;
                        break;
                }
            }
            const std::size_t header = 1 + width + extra;
            if (size - _offset < header)
                return status::incomplete;
            if (width) {
                std::uint64_t field = 0;
                for (std::size_t i = 1; i <= width; ++i)
                    field = (field << 8) | data[_offset + i];
                if (container)
                    items = pairs ? 2u * field : field;
                else
                    body = field;
            }
            if (body > size - _offset - header)
                return status::incomplete; // the value starts here again on the next call
            _offset += header + static_cast<std::size_t>(body);

            if (container) {
                if (_open.size() >= max_depth)
                    return status::invalid; // this container would sit at depth _open.size() + 1
                if (items) {
                    try {
                        _open.push_back(items);
                    } catch (...) {
                        return status::invalid;
                    }
                    continue;
                }
            }
            // One whole value: it completes its container when it was the last one owed, and so on outwards.
            while (!_open.empty() && --_open.back() == 0)
                _open.pop_back();
            if (_open.empty()) {
                _complete = true;
                return status::complete;
            }
        }
        return status::incomplete;
    }

    /** @brief The length of the value once `scan()` returned `complete`. */
    [[nodiscard]] std::size_t
    end() const noexcept {
        return _offset;
    }

    /** @brief Forgets the value: the next `scan()` starts a new one at `data[0]`. */
    void
    reset() noexcept {
        _offset   = 0;
        _complete = false;
        _open.clear();
    }

private:
    std::size_t                _offset = 0;       /**< Start of the first value not read yet (its end, once complete). */
    std::vector<std::uint64_t> _open;             /**< Values still owed by each open container, innermost last. */
    bool                       _complete = false; /**< The value is whole. */
};
} // namespace detail

/**
 * @class json
 * @ingroup Protocol
 * @brief Protocol for parsing null-terminated JSON messages.
 *
 * This class implements a protocol to handle JSON messages that are expected
 * to be terminated by a NULL character (`'\0'`). It uses the
 * `qb::protocol::base::byte_terminated` protocol as its base and parses the received
 * data (excluding the terminator) as a JSON object using `nlohmann::json`.
 *
 * The `onMessage` method, when invoked by the framework, will provide a
 * `message` struct containing the raw data, its size, and the parsed `nlohmann::json` object
 * to the associated I/O component's handler.
 *
 * @tparam IO_ The I/O component type (e.g., a TCP session class) that will use this protocol.
 *             It must be compatible with `base::byte_terminated`.
 */
template <typename IO_>
class json : public base::byte_terminated<IO_, '\0'> {
public:
    /**
     * @brief Default constructor (deleted)
     */
    json() = delete;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    explicit json(IO_ &io) noexcept
        : base::byte_terminated<IO_, '\0'>(io) {}

    /**
     * @struct message
     * @brief Structure representing a JSON message
     *
     * This structure contains information about a complete JSON message,
     * including its size, raw data, and the parsed JSON object.
     */
    struct message {
        const std::size_t size; /**< Message size */
        const char       *data; /**< Pointer to the raw data */
        nlohmann::json    json; /**< Parsed JSON object */
    };

    /**
     * @brief Process a received message
     *
     * This method is called when a complete message is received.
     * It builds a message object with the parsed JSON and passes it to the I/O object.
     *
     * @param size Message size with the delimiter
     */
    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data   = this->_io.in().cbegin();
        // DoS guard: nlohmann's recursive parser can blow the stack on deeply
        // nested input; reject pathological nesting before parsing.
        if (!detail::json_depth_within(data, parsed, detail::kJsonMaxNestingDepth)) {
            this->not_ok();
            return;
        }
        try {
            auto json = nlohmann::json::parse(std::string_view(data, parsed), nullptr, false);
            if (json.is_discarded()) {
                this->not_ok();
                return;
            }
            this->_io.on(message{parsed, data, std::move(json)});
        } catch (...) {
            this->not_ok();
        }
    }
};

/**
 * @class json_packed
 * @ingroup Protocol
 * @brief Protocol for MessagePack-encoded JSON messages, each followed by a NULL byte.
 *
 * Each message is one MessagePack value followed by a NULL byte (`'\0'`) -- what
 * `nlohmann::json::to_msgpack(value)` then `'\0'` writes. The framing is the VALUE, not the byte:
 * MessagePack carries zero bytes inside its values (the integer 0, a length, a float, a string or a binary),
 * so the message ends where `detail::msgpack_value_scanner` finds the end of one whole value, and the byte
 * after it must be the NULL -- anything else is a protocol error (`not_ok()`), as is a byte MessagePack never
 * uses or a nesting deeper than `detail::kJsonMaxNestingDepth`. The value is then decoded with
 * `nlohmann::json::from_msgpack`.
 *
 * The `onMessage` method provides a `message` struct containing the raw MessagePack data,
 * its size, and the deserialized `nlohmann::json` object to the I/O component's handler.
 *
 * @tparam IO_ The I/O component type that will use this protocol.
 *             It must provide `in()`, the input buffer, and `on(message&&)`.
 */
template <typename IO_>
class json_packed : public io::async::AProtocol<IO_> {
    detail::msgpack_value_scanner _scanner; /**< Where the framing of the pending message stopped. */

public:
    static constexpr std::size_t delimiter_size = 1;    /**< The NULL byte after each value */
    static constexpr char        end            = '\0'; /**< The byte that must follow each value */

    /**
     * @brief Default constructor (deleted)
     */
    json_packed() = delete;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    explicit json_packed(IO_ &io) noexcept
        : io::async::AProtocol<IO_>(io) {}

    /**
     * @struct message
     * @brief Structure representing a MessagePack encoded JSON message
     *
     * This structure contains information about a complete MessagePack message,
     * including its size, raw data, and the parsed JSON object.
     */
    struct message {
        const std::size_t size; /**< Message size */
        const char       *data; /**< Pointer to the raw data */
        nlohmann::json    json; /**< Parsed JSON object */
    };

    /**
     * @brief Calculates the message size without the delimiter
     *
     * @param size Total size including the delimiter
     * @return Message size without the delimiter
     */
    [[nodiscard]] inline std::size_t
    shiftSize(std::size_t const size) const noexcept {
        return size >= delimiter_size ? size - delimiter_size : 0;
    }

    /**
     * @brief Determines the size of the next complete message
     *
     * Frames one whole MessagePack value, resuming where the previous call stopped, then requires the NULL
     * byte right after it.
     *
     * @return The value's size plus the NULL byte once both are in the buffer, 0 otherwise (and `not_ok()`
     *         when the bytes cannot be a message)
     */
    std::size_t
    getMessageSize() noexcept final {
        const auto       &buffer = this->_io.in();
        const std::size_t size   = buffer.size();
        const auto       *data   = reinterpret_cast<const unsigned char *>(buffer.cbegin());
        switch (_scanner.scan(data, size, detail::kJsonMaxNestingDepth)) {
            case detail::msgpack_value_scanner::status::incomplete:
                return 0;
            case detail::msgpack_value_scanner::status::invalid:
                this->not_ok();
                return 0;
            case detail::msgpack_value_scanner::status::complete:
                break;
        }
        const std::size_t value_size = _scanner.end();
        if (value_size == size)
            return 0; // the value is whole; its NULL has not arrived yet
        if (data[value_size] != static_cast<unsigned char>(end)) {
            this->not_ok();
            return 0;
        }
        _scanner.reset();
        return value_size + delimiter_size;
    }

    /**
     * @brief Process a received message
     *
     * This method is called when a complete message is received.
     * It builds a message object with the JSON parsed from MessagePack format
     * and passes it to the I/O object.
     *
     * @param size Message size with the delimiter
     */
    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data   = this->_io.in().cbegin();
        // The framing already bounded the nesting (kJsonMaxNestingDepth), so from_msgpack()'s recursive
        // binary reader cannot blow the stack (a try/catch cannot recover a stack overflow).
        try {
            auto json = nlohmann::json::from_msgpack(std::string_view(data, parsed), true, false);
            if (json.is_discarded()) {
                this->not_ok();
                return;
            }
            this->_io.on(message{parsed, data, std::move(json)});
        } catch (...) {
            this->not_ok();
        }
    }

    /**
     * @brief Resets the protocol state
     */
    void
    reset() noexcept final {
        _scanner.reset();
    }
};

} // namespace protocol
} // namespace qb
#endif // QB_IO_PROTOCOL_JSON_H
