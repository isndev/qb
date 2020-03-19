/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

#include <algorithm>
#include <cstring>
#include <string>
#include <iostream>
#include <array>
#include <qb/utility/build_macros.h>

#ifndef QB_STRING_H_
# define QB_STRING_H_

namespace qb {

    template <std::size_t _Size = 30>
    class string : public std::array<char, _Size + 1> {
        using base_t = std::array<char, _Size + 1>;
        unsigned char _size = 0;
    public:

        string() noexcept
            : base_t {'\0'} {}


        template <typename T>
        string(T const &rhs) noexcept {
            assign(rhs);
        }

        string &assign(char const *rhs, std::size_t size) noexcept {
            *base_t::begin() = '\0';
            _size = static_cast<unsigned char>(std::min(size, static_cast<std::size_t>(_Size)));
            strncat(base_t::data(), rhs, _size);
            return *this;
        }

        template <typename T>
        string &assign(T const &rhs) noexcept {
            return assign(rhs.c_str(), rhs.size());
        }

        string &assign(char const *rhs) noexcept {
            return assign(rhs, strlen(rhs));
        }

        template <typename T>
        string &operator=(T const &rhs) noexcept {
            return assign(rhs);
        }

        string &operator=(char const *rhs) noexcept {
            return assign(rhs);
        }

        operator std::string() const noexcept {
            return std::string(base_t::data());
        }

        template <typename T>
        bool operator==(T const &rhs) const noexcept {
            return rhs == base_t::data();
        }

        bool operator==(char const *rhs) const noexcept {
            return !strcmp(base_t::data(), rhs);
        }

        typename base_t::iterator end() noexcept { return base_t::begin() + _size; }
        typename base_t::const_iterator cend() const noexcept { return base_t::cbegin() + _size; }
        typename base_t::reverse_iterator rbegin() noexcept { return std::reverse_iterator(end()); }
        typename base_t::const_reverse_iterator crbegin() const noexcept { return std::reverse_iterator(cend()); }
        char const *c_str() const noexcept { return base_t::data(); }
        std::size_t size() const noexcept { return _size; }

    };

}

template <std::size_t _Size>
std::ostream &operator<<(std::ostream &os, qb::string<_Size> const &str) noexcept {
    os << str.c_str();
    return os;
}

template <std::size_t _Size>
std::istream &operator>>(std::istream &is, qb::string<_Size> const &str) noexcept {
    str.c_str() >> is;
    return is;
}

#endif // QB_STRING_H_
