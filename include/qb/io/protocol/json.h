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
#include <utility>
#include "base.h"
#include "nlohmann/json.hpp"

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
 * @brief Protocol for parsing null-terminated, MessagePack encoded JSON messages.
 *
 * This class implements a protocol to handle messages that are MessagePack encoded JSON,
 * terminated by a NULL character (`'\0'`). It uses `base::byte_terminated` for framing
 * and then deserializes the MessagePack data into an `nlohmann::json` object.
 *
 * The `onMessage` method provides a `message` struct containing the raw MessagePack data,
 * its size, and the deserialized `nlohmann::json` object to the I/O component's handler.
 *
 * @tparam IO_ The I/O component type that will use this protocol.
 *             It must be compatible with `base::byte_terminated`.
 */
template <typename IO_>
class json_packed : public base::byte_terminated<IO_, '\0'> {
public:
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
        : base::byte_terminated<IO_, '\0'>(io) {}

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
        // DoS guard: bound MessagePack nesting before from_msgpack()'s recursive
        // binary reader can blow the stack (a try/catch cannot recover a stack
        // overflow). Mirrors the text json protocol's json_depth_within pre-scan;
        // the pre-scan itself aborts at kJsonMaxNestingDepth, so it is bounded too.
        if (!detail::msgpack_depth_within(data, parsed, detail::kJsonMaxNestingDepth)) {
            this->not_ok();
            return;
        }
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
};

} // namespace protocol
} // namespace qb
#endif // QB_IO_PROTOCOL_JSON_H
