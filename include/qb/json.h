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
#include "io/protocol/json.h"
#include "uuid.h"

namespace nlohmann {

// template <typename IO_>
// using protocol = qb::protocol::json<IO_>;
// template <typename IO_>
// using protocol_packed = qb::protocol::json_packed<IO_>;

using pointer  = json::pointer;
using object   = json;
using array    = json::array_t;
using string   = json::string_t;
using number   = json::number_integer_t;
using floating = json::number_float_t;
using boolean  = json::boolean_t;

struct jsonb {
    using json = nlohmann::json;
    json data;

    // --- Constructors ---
    jsonb() = default;
    jsonb(const json &j)
        : data(j) {}
    jsonb(json &&j) noexcept
        : data(std::move(j)) {}
    jsonb(std::initializer_list<json::value_type> init)
        : data(init) {}

    // --- Conversion operators ---
    operator json &() {
        return data;
    }
    operator const json &() const {
        return data;
    }

    // --- Forwarded operators ---
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
    json &
    unwrap() {
        return data;
    }
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
void to_json(qb::json &obj, qb::uuid const &id);
void from_json(qb::json const &obj, qb::uuid &id);
} // namespace uuids

// Stream output support
inline std::ostream &
operator<<(std::ostream &os, const qb::jsonb &j) {
    return os << j.dump();
}

// Hash support
namespace std {
template <>
struct hash<qb::jsonb> {
    std::size_t
    operator()(const qb::jsonb &j) const noexcept {
        return std::hash<std::string>{}(j.dump());
    }
};
} // namespace std

// Optional: fmtlib support
#ifdef FMT_VERSION
#include <fmt/format.h>
template <>
struct fmt::formatter<qb::jsonb> : formatter<std::string> {
    auto
    format(const qb::jsonb &j, fmt::format_context &ctx) {
        return formatter<std::string>::format(j.dump(), ctx);
    }
};
#endif

#endif // QB_JSON_H
