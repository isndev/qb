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

#include <string>
#include <string_view>
#include <regex>
#include <vector>
#include <qb/system/container/unordered_map.h>

#ifndef QB_IO_URI_H_
#    define QB_IO_URI_H_

namespace qb::io {
    class uri {
        std::string _source;
        std::string_view _scheme;
        std::string_view _user;
        std::string_view _password;
        std::string_view _host;
        std::string_view _port;
        std::string_view _full_path;
        std::string_view _path;
        qb::icase_unordered_map<std::vector<std::string>> _queries;

        bool from(std::string const &str_uri) {
            static const std::regex uri_regex("((\\w+)://)?(([^:]+)(:(.+))?@)?([a-zA-Z0-9\\.-]+)(:(\\d+))?(.+)?");
            static const std::regex query_regex("(\\?|&)([^=]*)=([^&]*)");
            static const std::string_view no_path = {"/"};
            static const qb::unordered_map<std::string_view, std::string_view> default_ports {
                    {"ftp", "21"},
                    {"sftp", "22"},
                    {"ssh", "22"},
                    {"telnet", "23"},
                    {"smtp", "25"},
                    {"dns", "53"},
                    {"http", "80"},
                    {"ws", "80"},
                    {"pop", "110"},
                    {"pop3", "110"},
                    {"nntp", "119"},
                    {"imap", "143"},
                    {"https", "443"},
                    {"nntps", "563"},
                    {"wss", "443"},
                    {"smtps", "465"},
                    {"ftps", "990"},
                    {"imaps", "993"},
                    {"pops", "995"},
                    {"pop3s", "995"},
                    {"mqtt", "1883"},
                    {"nfs", "2049"},
                    {"amqps", "5671"},
                    {"amqp", "5672"},
                    {"mqtts", "8883"}
            };

            _source = str_uri;
            _queries.clear();

            std::cmatch what;
            if (!std::regex_match(_source.c_str(), what, uri_regex))
                return false;
            _scheme = {what[2].first, static_cast<std::size_t>(what[2].length())};
            _user = {what[4].first, static_cast<std::size_t>(what[4].length())};
            _password = {what[6].first, static_cast<std::size_t>(what[6].length())};
            _host = {what[7].first, static_cast<std::size_t>(what[7].length())};
            _port = {what[9].first, static_cast<std::size_t>(what[9].length())};
            _full_path = what[10].length() ? std::string_view{what[10].first, static_cast<std::size_t>(what[10].length())} : no_path;

            auto has_query = _full_path.find('?');
            if (has_query != std::string::npos) {
                _path = {_full_path.data(), static_cast<std::size_t>(has_query)};
                auto search = _full_path.data() + has_query;
                const auto end = _full_path.data() + _full_path.size();
                while (std::regex_search(search, end, what, query_regex)) {
                    _queries[std::string(what[2].first,
                                         static_cast<std::size_t>(what[2].length()))]
                            .push_back(decode(
                                    what[3].first, what[3].first + static_cast<std::size_t>(what[3].length())));
                    search += what[0].length();
                }
            } else
                _path = _full_path;
            if (!_port.size()) {
                const auto it = default_ports.find(_scheme);
                if (it != default_ports.end())
                    _port = it->second;

            }
            return true;
        }

    public:
        uri() = default;
        uri(uri const &rhs) {
            from(rhs._source);
        }
//        uri(uri &&rhs) = default;
//            : _source(std::move(rhs._source))
//            , _scheme(std::move(_scheme))
//            , _scheme(std::move(_scheme))
//            {}
        uri(std::string const &str_uri) {
            from(str_uri);
        }

        template <typename _IT>
        static std::string
        decode(_IT begin, _IT end) {
            static const char tbl[256] = {
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  -1, -1,
                    -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, 12,
                    13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

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

        static uri parse(std::string const &str_uri) {
            uri new_uri;
            new_uri.from(str_uri);
            return new_uri;
        }

        uri &operator=(std::string const &str) {
            from(str);
            return *this;
        }

//        uri &operator=(std::string_view const &str) {
//            parse(std::string(str));
//            return *this;
//        }

        [[nodiscard]] inline const auto &source() const { return _source; }
        [[nodiscard]] inline auto scheme() const { return _scheme; }
        [[nodiscard]] inline auto user() const { return _user; }
        [[nodiscard]] inline auto password() const { return _password; }
        [[nodiscard]] inline auto host() const { return _host; }
        [[nodiscard]] inline auto port() const { return _port; }
        [[nodiscard]] inline auto full_path() const { return _full_path; }
        [[nodiscard]] inline auto path() const { return _path; }

        template <typename T>
        [[nodiscard]] std::string const &
        query(T &&name, std::size_t const index = 0,
              std::string const &not_found = "") const {
            const auto &it = this->_queries.find(std::forward<T>(name));
            if (it != this->_queries.cend() && index < it->second.size())
                return it->second[index];

            return not_found;
        }

        [[nodiscard]] const auto &queries() const { return _queries; }

    };
}

#endif //QB_IO_URI_H_
