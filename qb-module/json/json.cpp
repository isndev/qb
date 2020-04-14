/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include "json.h"

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put<qb::json::object>(const qb::json::object &val) {
    switch (val.type()) {
    case nlohmann::json::value_t::object: {
        if (val.empty()) {
            *this << "{}";
            return *this;
        }

        *this << '{';

        // first n-1 elements
        auto i = val.cbegin();
        for (std::size_t cnt = 0; cnt < val.size() - 1; ++cnt, ++i) {
            *this << '\"';
            *this << i.key();
            *this << "\":";
            put(i.value());
            *this << ',';
        }

        // last element
        *this << '\"';
        *this << i.key();
        *this << "\":";
        put(i.value());

        *this << '}';

        return *this;
    }

    case nlohmann::json::value_t::array: {
        if (val.empty()) {
            *this << "[]";
            return *this;
        }
        *this << '[';

        // first n-1 elements
        for (auto i = val.cbegin(); i != val.cend() - 1; ++i) {
            put(i.value());
            *this << ',';
        }

        // last element
        put(val.back());

        *this << ']';

        return *this;
    }

    case nlohmann::json::value_t::string: {
        *this << '\"';
        *this << val.get<std::string>();
        // dump_escaped(*val.m_value.string, ensure_ascii);
        *this << '\"';
        return *this;
    }

    case nlohmann::json::value_t::boolean: {
        if (val.get<bool>()) {
            *this << "true";
        } else {
            *this << "false";
        }
        return *this;
    }

    case nlohmann::json::value_t::number_integer: {
        *this << val.get<int>();
        // dump_integer(val.m_value.number_integer);
        return *this;
    }

    case nlohmann::json::value_t::number_unsigned: {
        *this << val.get<unsigned int>();
        // dump_integer(val.m_value.number_unsigned);
        return *this;
    }

    case nlohmann::json::value_t::number_float: {
        *this << val.get<float>();
        // dump_float(val.m_value.number_float);
        return *this;
    }

    case nlohmann::json::value_t::discarded: {
        *this << "<discarded>";
        return *this;
    }

    case nlohmann::json::value_t::null: {
        *this << "null";
        return *this;
    }

    default:
        break; // LCOV_EXCL_LINE
    }
    return *this;
}

} // namespace qb::allocator
