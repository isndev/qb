/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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


#include <qb/io/config.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/build_macros.h>
#include <regex>
#include <string>
#include <string_view>
#include <vector>
#include <charconv>

#ifndef QB_IO_URI_H_
#    define QB_IO_URI_H_

namespace qb::io {
class uri {
    int _af = AF_INET;
    std::string _source;
    std::string_view _scheme;
    std::string_view _user;
    std::string_view _password;
    std::string_view _host;
    std::string_view _port;
    std::string_view _full_path;
    std::string_view _path;
    qb::icase_unordered_map<std::vector<std::string>> _queries;

    constexpr static const char no_path[] = "/";

    bool parse_queries(std::size_t pos) noexcept;
    bool parse_v6(std::size_t pos) noexcept;
    bool parse_v4() noexcept;
    bool parse() noexcept;
    bool from(std::string const &rhs) noexcept;
    bool from(std::string &&rhs) noexcept;

public:
    uri() = default;
    uri(uri &&rhs) = default;
    uri(uri const &rhs) noexcept;
    uri(std::string const &str, int af = AF_INET) noexcept;
    uri(std::string &&str, int af = AF_INET) noexcept;

    static const char tbl[256];

    template <typename _IT>
    static std::string
    decode(_IT begin, _IT end) noexcept {
        std::string out;
        char c, v1, v2;

        while (begin != end) {
            c = *(begin++);
            if (c == '%') {
                if ((v1 = tbl[(unsigned char)*(begin++)]) < 0 ||
                    (v2 = tbl[(unsigned char)*(begin++)]) < 0) {
                    break;
                }
                c = (v1 << 4) | v2;
            }
            out += (c);
        }

        return out;
    }

    constexpr static const char dont_escape[] = "._-$,;~()";
    constexpr static const char hex[] = "0123456789abcdef";
    template <typename _IT>
    static std::string
    encode(_IT begin, _IT end) noexcept {
        std::string dst;
        dst.reserve(static_cast<ptrdiff_t>(end - begin) * 3);

        while (begin != end) {
            if (isalnum(*begin) || strchr(dont_escape, *begin) != NULL) {
                dst.push_back(*begin);
            } else {
                dst.push_back('%');
                dst.push_back(hex[(*begin) >> 4]);
                dst.push_back(hex[(*begin) & 0xf]);
            }
            ++begin;
        }

        return dst;
    }

    static std::string decode(std::string const &input) noexcept;
    static std::string encode(std::string const &input) noexcept;
    static std::string decode(std::string_view const &input) noexcept;
    static std::string encode(std::string_view const &input) noexcept;
    static std::string decode(const char *input, std::size_t size) noexcept;
    static std::string encode(const char *input, std::size_t size) noexcept;
    static uri parse(std::string const &str, int af = AF_INET) noexcept;

    uri &operator=(uri const &str);
    uri &operator=(uri &&str);
    uri &operator=(std::string const &str);
    uri &operator=(std::string &&str);

    [[nodiscard]] inline auto
    af() const {
        return _af;
    }
    [[nodiscard]] inline const auto &
    source() const {
        return _source;
    }
    [[nodiscard]] inline auto
    scheme() const {
        return _scheme;
    }
    [[nodiscard]] inline auto
    user() const {
        return _user;
    }
    [[nodiscard]] inline auto
    password() const {
        return _password;
    }
    [[nodiscard]] inline auto
    host() const {
        return _host;
    }
    [[nodiscard]] inline auto
    port() const {
        return _port;
    }
    [[nodiscard]] inline auto
    full_path() const {
        return _full_path;
    }
    [[nodiscard]] inline auto
    path() const {
        return _path;
    }
    [[nodiscard]] inline const auto &
    queries() const {
        return _queries;
    }
    [[nodiscard]] uint16_t
    u_port() const {
        int p = 0;

        std::from_chars(port().data(), port().data() + port().size(), p);
        return static_cast<uint16_t>(p);
    }

    template <typename T>
    [[nodiscard]] std::string const &
    query(T &&name, std::size_t const index = 0,
          std::string const &not_found = "") const {
        const auto &it = this->_queries.find(std::forward<T>(name));
        if (it != this->_queries.cend() && index < it->second.size())
            return it->second[index];

        return not_found;
    }
};
} // namespace qb::io

#endif // QB_IO_URI_H_
