/**
 * @file qb/json.h
 * @brief JSON utility functions and types for the qb framework
 *
 * This file provides a simple JSON utility using the nlohmann/json library.
 * It includes a namespace alias for the json type and a simple JSON utility
 * function for parsing JSON strings.
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
 * @ingroup JSON
 */

#ifndef QB_JSON_H
#define QB_JSON_H
#include "qb/io/protocol/json.h"
#include "uuid.h"

namespace nlohmann {

// template <typename IO_>
// using protocol = qb::protocol::json<IO_>;
// template <typename IO_>
// using protocol_packed = qb::protocol::json_packed<IO_>;

/**
 * @typedef pointer
 * @brief JSON pointer type from nlohmann::json library
 * @details Used for accessing nested JSON values with specific paths
 * @ingroup JSON
 */
using pointer  = json::pointer;

/**
 * @typedef object
 * @brief Alias for the main JSON type from nlohmann::json
 * @details Represents a complete JSON object with various value types
 * @ingroup JSON
 */
using object   = json;

/**
 * @typedef array
 * @brief JSON array type from nlohmann::json
 * @details Represents an ordered collection of JSON values
 * @ingroup JSON
 */
using array    = json::array_t;

/**
 * @typedef string
 * @brief JSON string type from nlohmann::json
 * @details Represents a string value in JSON
 * @ingroup JSON
 */
using string   = json::string_t;

/**
 * @typedef number
 * @brief JSON integer number type from nlohmann::json
 * @details Represents an integer numerical value in JSON
 * @ingroup JSON
 */
using number   = json::number_integer_t;

/**
 * @typedef floating
 * @brief JSON floating-point number type from nlohmann::json
 * @details Represents a floating-point numerical value in JSON
 * @ingroup JSON
 */
using floating = json::number_float_t;

/**
 * @typedef boolean
 * @brief JSON boolean type from nlohmann::json
 * @details Represents a boolean value (true/false) in JSON
 * @ingroup JSON
 */
using boolean  = json::boolean_t;

/**
 * @struct jsonb
 * @ingroup JSON
 * @brief A wrapper around `nlohmann::json` providing a `qb::jsonb` type, often used for binary JSON (BSON) or similar dense representations.
 *
 * This struct primarily forwards operations to an internal `nlohmann::json` member. It offers a distinct type
 * within the `qb` namespace that can be specialized or handled differently from `nlohmann::json` directly,
 * particularly in serialization contexts (like MessagePack potentially mapping to BSON-like structures).
 * Most methods are direct passthroughs to the underlying `nlohmann::json` object.
 */
struct jsonb {
    using json = nlohmann::json;
    json data; /**< @brief The underlying `nlohmann::json` object storing the JSON data. */

    // --- Constructors ---
    /** @brief Default constructor. Creates an empty/null JSON object. */
    jsonb() = default;
    /** @brief Constructs from an existing `nlohmann::json` object (copy). */
    jsonb(const json &j)
        : data(j) {}
    /** @brief Constructs from an existing `nlohmann::json` object (move). */
    jsonb(json &&j) noexcept
        : data(std::move(j)) {}
    /** @brief Constructs from an initializer list, same as `nlohmann::json`. */
    jsonb(std::initializer_list<json::value_type> init)
        : data(init) {}

    // --- Conversion operators ---
    /** @brief Implicit conversion to a mutable reference to the underlying `nlohmann::json` object. */
    operator json &() {
        return data;
    }
    /** @brief Implicit conversion to a constant reference to the underlying `nlohmann::json` object. */
    operator const json &() const {
        return data;
    }

    // --- Forwarded operators ---
    /** @brief Assigns from another `nlohmann::json` object (copy). */
    json &
    operator=(const json &j) {
        data = j;
        return *this;
    }
    json &
    operator=(json &&j) noexcept {
        data = std::move(j);
        return *this;
    }

    json &
    operator[](const std::string &key) {
        return data[key];
    }
    const json &
    operator[](const std::string &key) const {
        return data.at(key);
    }

    json &
    operator[](size_t i) {
        return data[i];
    }
    const json &
    operator[](size_t i) const {
        return data.at(i);
    }

    json *
    operator->() {
        return &data;
    }
    const json *
    operator->() const {
        return &data;
    }

    // --- Iterators ---
    auto
    begin() {
        return data.begin();
    }
    auto
    end() {
        return data.end();
    }
    auto
    begin() const {
        return data.begin();
    }
    auto
    end() const {
        return data.end();
    }

    // --- Common methods (forwarded) ---
    inline std::string
    dump(int indent = -1) const {
        return data.dump(indent);
    }
    inline bool
    is_null() const {
        return data.is_null();
    }
    inline bool
    is_object() const {
        return data.is_object();
    }
    inline bool
    is_array() const {
        return data.is_array();
    }
    inline bool
    is_string() const {
        return data.is_string();
    }
    inline bool
    is_number() const {
        return data.is_number();
    }
    inline bool
    is_boolean() const {
        return data.is_boolean();
    }

    inline std::size_t
    size() const {
        return data.size();
    }
    inline bool
    empty() const {
        return data.empty();
    }
    inline void
    clear() {
        data.clear();
    }
    inline void
    push_back(const json &value) {
        data.push_back(value);
    }
    inline void
    erase(const std::string &key) {
        data.erase(key);
    }
    inline bool
    contains(const std::string &key) const {
        return data.contains(key);
    }

    // --- Comparisons ---
    friend bool
    operator==(const jsonb &a, const jsonb &b) {
        return a.data == b.data;
    }
    friend bool
    operator!=(const jsonb &a, const jsonb &b) {
        return a.data != b.data;
    }
    friend bool
    operator==(const jsonb &a, const json &b) {
        return a.data == b;
    }
    friend bool
    operator==(const json &a, const jsonb &b) {
        return a == b.data;
    }

    // --- Access ---
    /**
     * @brief Provides direct mutable access to the underlying `nlohmann::json` object.
     * @return Reference to the internal `nlohmann::json` data.
     */
    json &
    unwrap() {
        return data;
    }
    /**
     * @brief Provides direct constant access to the underlying `nlohmann::json` object.
     * @return Constant reference to the internal `nlohmann::json` data.
     */
    const json &
    unwrap() const {
        return data;
    }
};

} // namespace nlohmann

namespace qb {
using namespace nlohmann;
namespace allocator {

template <>
pipe<char> &pipe<char>::put<json>(const json &c);

} // namespace allocator
} // namespace qb

namespace uuids {
/**
 * @brief Converts a UUID to a JSON object
 * @details Serializes a qb::uuid into a JSON object for storage or transmission
 * 
 * @param obj JSON object to store the UUID in
 * @param id UUID to serialize
 * @ingroup JSON
 */
void to_json(qb::json &obj, qb::uuid const &id);

/**
 * @brief Converts a JSON object to a UUID
 * @details Deserializes a UUID from a JSON object
 * 
 * @param obj JSON object containing the UUID data
 * @param id UUID object to populate
 * @ingroup JSON
 */
void from_json(qb::json const &obj, qb::uuid &id);
} // namespace uuids

// Stream output support
/**
 * @brief Stream output operator for `qb::jsonb`.
 * @ingroup JSON
 * @param os The output stream.
 * @param j The `qb::jsonb` object to output.
 * @return Reference to the output stream.
 * @details Dumps the JSON content to the stream, typically without pretty printing.
 */
inline std::ostream &
operator<<(std::ostream &os, const qb::jsonb &j) {
    return os << j.dump();
}

// Hash support
namespace std {
/**
 * @struct hash<qb::jsonb>
 * @ingroup JSON
 * @brief Specialization of `std::hash` for `qb::jsonb`.
 * @details Allows `qb::jsonb` objects to be used as keys in `std::unordered_map` and `std::unordered_set`.
 *          The hash is computed based on the string dump of the JSON object.
 */
template <>
struct hash<qb::jsonb> {
    /**
     * @brief Calculates the hash value for a `qb::jsonb` object.
     * @param j The `qb::jsonb` object to hash.
     * @return The computed hash value.
     */
    std::size_t
    operator()(const qb::jsonb &j) const noexcept {
        return std::hash<std::string>{}(j.dump());
    }
};
} // namespace std

// Optional: fmtlib support
#ifdef FMT_VERSION
#include <fmt/format.h>
/**
 * @struct formatter<qb::jsonb>
 * @ingroup JSON
 * @brief Specialization of `fmt::formatter` for `qb::jsonb` (if `fmt` library is used).
 * @details Allows `qb::jsonb` objects to be formatted using the `fmt` library.
 *          Formats by dumping the JSON content as a string.
 */
template <>
struct fmt::formatter<qb::jsonb> : formatter<std::string> {
    /**
     * @brief Formats the `qb::jsonb` object.
     * @param j The `qb::jsonb` object to format.
     * @param ctx The format context.
     * @return Iterator to the end of the formatted output.
     */
    auto
    format(const qb::jsonb &j, fmt::format_context &ctx) {
        return formatter<std::string>::format(j.dump(), ctx);
    }
};
#endif

#endif // QB_JSON_H
