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

#include <qb/io/uri.h>

namespace qb::io {

const char uri::tbl[256] = {
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

uri::uri(uri const &rhs) noexcept
    : _af(rhs._af)
    , _source(rhs._source) {
    parse();
}

uri::uri(std::string const &str, int af) noexcept
    : _af(af)
    , _source(str) {
    parse();
}

uri::uri(std::string &&str, int af) noexcept
    : _af(af)
    , _source(std::move(str)) {
    parse();
}

bool
uri::parse_queries(std::size_t pos) noexcept {
    static const std::regex query_regex("(\\?|&)([^=]*)=([^&]*)");

    if (pos != std::string::npos) {
        _path = {_full_path.data(), static_cast<std::size_t>(pos)};
        auto search = _full_path.data() + pos;
        const auto end = _full_path.data() + _full_path.size();
        std::cmatch what;
        while (std::regex_search(search, end, what, query_regex)) {
            _queries[std::string(what[2].first,
                                 static_cast<std::size_t>(what[2].length()))]
                .push_back(
                    decode(what[3].first,
                           what[3].first + static_cast<std::size_t>(what[3].length())));
            search += what[0].length();
        }
    } else
        _path = _full_path;

    return true;
}

static const std::regex uri_regex_p1("((\\w+)://)?(([^:]+)(:(.+))?@)?");
static const std::regex uri_regex_p2("(:(\\d+))?(.+)?");

bool
uri::parse_v6(std::size_t pos) noexcept {
    const auto begin_p1 = _source.c_str();
    const auto end_p1 = begin_p1 + pos;

    std::cmatch what;
    if (!std::regex_match(begin_p1, end_p1, what, uri_regex_p1))
        return false;

    _scheme = {what[2].first, static_cast<std::size_t>(what[2].length())};
    _user = {what[4].first, static_cast<std::size_t>(what[4].length())};
    _password = {what[6].first, static_cast<std::size_t>(what[6].length())};

    const auto begin_host = end_p1 + 1;
    const auto end = begin_p1 + _source.size();
    const auto end_host = std::find(begin_host, end, ']');

    if (end_host == end)
        return false;

    _host = {begin_host, static_cast<std::size_t>(end_host - begin_host)};

    std::regex_match(end_host + 1, end, what, uri_regex_p2);

    _port = {what[2].first, static_cast<std::size_t>(what[2].length())};
    _full_path =
        what[3].length()
            ? std::string_view{what[3].first, static_cast<std::size_t>(what[3].length())}
            : no_path;

    return parse_queries(_full_path.find_first_of('?'));
}

static const std::regex
    uri_regex("((\\w+)://)?(([^:]+)(:(.+))?@)?([a-zA-Z0-9\\.-]+)(:(\\d+))?(.+)?");

bool
uri::parse_v4() noexcept {
    std::cmatch what;
    if (!std::regex_match(_source.c_str(), what, uri_regex))
        return false;
    _scheme = {what[2].first, static_cast<std::size_t>(what[2].length())};
    _user = {what[4].first, static_cast<std::size_t>(what[4].length())};
    _password = {what[6].first, static_cast<std::size_t>(what[6].length())};
    _host = {what[7].first, static_cast<std::size_t>(what[7].length())};
    _port = {what[9].first, static_cast<std::size_t>(what[9].length())};
    _full_path = what[10].length() || _af == AF_UNIX
                     ? std::string_view{what[10].first,
                                        static_cast<std::size_t>(what[10].length())}
                     : no_path;

    return parse_queries(_full_path.find_first_of('?'));
}

static const qb::unordered_map<std::string_view, std::string_view> default_ports{
    {"unix", "0"},    {"ftp", "21"},    {"sftp", "22"},   {"ssh", "22"},
    {"telnet", "23"}, {"smtp", "25"},   {"dns", "53"},    {"http", "80"},
    {"ws", "80"},     {"pop", "110"},   {"pop3", "110"},  {"nntp", "119"},
    {"imap", "143"},  {"https", "443"}, {"nntps", "563"}, {"wss", "443"},
    {"smtps", "465"}, {"ftps", "990"},  {"imaps", "993"}, {"pops", "995"},
    {"pop3s", "995"}, {"mqtt", "1883"}, {"nfs", "2049"},  {"amqps", "5671"},
    {"amqp", "5672"}, {"mqtts", "8883"}};

bool
uri::parse() noexcept {
    _queries.clear();
    const auto pos_v6 = _source.find_first_of('[');
    const auto ret = pos_v6 == std::string::npos
                         ? parse_v4()
                         : (parse_v6(pos_v6) && (_af = AF_INET6));
    if (!_port.size()) {
        const auto it = default_ports.find(_scheme);
        if (it != default_ports.end())
            _port = it->second;
    }

    if (_scheme == "unix")
        _af = AF_UNIX;

    return ret;
}

bool
uri::from(std::string const &rhs) noexcept {
    _source = rhs;
    return parse();
}

bool
uri::from(std::string &&rhs) noexcept {
    _source = std::move(rhs);
    return parse();
}

std::string
uri::decode(std::string const &input) noexcept {
    return decode(input.cbegin(), input.cend());
}

std::string
uri::encode(std::string const &input) noexcept {
    return encode(input.cbegin(), input.cend());
}

std::string
uri::decode(std::string_view const &input) noexcept {
    return decode(input.cbegin(), input.cend());
}

std::string
uri::encode(std::string_view const &input) noexcept {
    return encode(input.cbegin(), input.cend());
}

std::string
uri::decode(const char *input, std::size_t size) noexcept {
    return decode(std::string_view(input, size));
}

std::string
uri::encode(const char *input, std::size_t size) noexcept {
    return encode(std::string_view(input, size));
}

uri
uri::parse(std::string const &str, int af) noexcept {
    return {str, af};
}

uri &
uri::operator=(std::string const &str) {
    from(str);
    return *this;
}

uri &
uri::operator=(std::string &&str) {
    from(str);
    return *this;
}

uri &
uri::operator=(uri const &rhs) {
    from(rhs._source);
    return *this;
}

uri &
uri::operator=(uri &&rhs) {
    from(std::move(rhs._source));
    return *this;
}

} // namespace qb::io