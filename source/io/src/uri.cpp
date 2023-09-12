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
DISABLE_WARNING_PUSH
DISABLE_WARNING_NARROWING
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
DISABLE_WARNING_POP

uri::uri(uri &&rhs) noexcept {
    *this = std::forward<uri>(rhs);
}

uri::uri(uri const &rhs) {
    *this = rhs;
}

uri::uri(std::string const &str, int af)
    : _af(af)
    , _source(str) {
    parse();
}

uri::uri(std::string &&str, int af) noexcept
    : _af(af)
    , _source(std::move(str)) {
    parse();
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

    const char *p = _source.c_str();
    const char *scheme_begin = nullptr;
    const char *scheme_end = nullptr;
    const char *uinfo_begin = nullptr;
    const char *uinfo_end = nullptr;
    const char *host_begin = nullptr;
    const char *host_end = nullptr;
    const char *port_begin = nullptr;
    const char *port_end = nullptr;
    const char *path_begin = nullptr;
    const char *path_end = nullptr;
    const char *query_begin = nullptr;
    const char *query_end = nullptr;
    const char *fragment_begin = nullptr;
    const char *fragment_end = nullptr;

    // IMPORTANT -- A uri may either be an absolute uri, or an relative-reference
    // Absolute: 'http://host.com'
    // Relative-Reference: '//:host.com', '/path1/path2?query', './path1:path2'
    // A Relative-Reference can be disambiguated by parsing for a ':' before the first
    // slash

    bool is_relative_reference = true;
    const char *p2 = p;
    for (; *p2 != '/' && *p2 != '\0'; p2++) {
        if (*p2 == ':') {
            // found a colon, the first portion is a scheme
            is_relative_reference = false;
            break;
        }
    }

    if (!is_relative_reference) {
        // the first character of a scheme must be a letter
        if (!isalpha(*p)) {
            return false;
        }

        // start parsing the scheme, it's always delimited by a colon (must be present)
        scheme_begin = p++;
        for (; *p != ':'; p++) {
            if (!is_scheme_character(*p)) {
                return false;
            }
        }
        scheme_end = p;

        // skip over the colon
        p++;
    }

    // if we see two slashes next, then we're going to parse the authority portion
    // later on we'll break up the authority into the port and host
    const char *authority_begin = nullptr;
    const char *authority_end = nullptr;
    if (*p == '/' && p[1] == '/') {
        // skip over the slashes
        p += 2;
        authority_begin = p;

        // the authority is delimited by a slash (resource), question-mark (query) or
        // octothorpe (fragment) or by EOS. The authority could be empty
        // ('file:///C:\file_name.txt')
        for (; *p != '/' && *p != '?' && *p != '#' && *p != '\0'; p++) {
            // We're NOT currently supporting IPvFuture or username/password in authority
            // IPv6 as the host (i.e. http://[:::::::]) is allowed as valid URI and
            // passed to subsystem for support.
            if (!is_authority_character(*p)) {
                return false;
            }
        }
        authority_end = p;

        // now lets see if we have a port specified -- by working back from the end
        if (authority_begin != authority_end) {
            // the port is made up of all digits
            port_begin = authority_end - 1;
            for (; isdigit(*port_begin) && port_begin != authority_begin; port_begin--) {
            }

            if (*port_begin == ':') {
                // has a port
                host_begin = authority_begin;
                host_end = port_begin;

                // skip the colon
                port_begin++;
                port_end = authority_end;
            } else {
                // no port
                host_begin = authority_begin;
                host_end = authority_end;
            }

            // look for a user_info component
            const char *u_end = host_begin;
            for (; is_user_info_character(*u_end) && u_end != host_end; u_end++) {
            }

            if (*u_end == '@') {
                host_begin = u_end + 1;
                uinfo_begin = authority_begin;
                uinfo_end = u_end;
            }
        }
    }

    // if we see a path character or a slash, then the
    // if we see a slash, or any other legal path character, parse the path next
    if (*p == '/' || is_path_character(*p)) {
        path_begin = p;

        // the path is delimited by a question-mark (query) or octothorpe (fragment) or
        // by EOS
        for (; *p != '?' && *p != '#' && *p != '\0'; p++) {
            if (!is_path_character(*p)) {
                return false;
            }
        }
        path_end = p;
    }

    // if we see a ?, then the query is next
    if (*p == '?') {
        // skip over the question mark
        p++;
        query_begin = p;

        // the query is delimited by a '#' (fragment) or EOS
        for (; *p != '#' && *p != '\0'; p++) {
            if (!is_query_character(*p)) {
                return false;
            }
        }
        query_end = p;
    }

    // if we see a #, then the fragment is next
    if (*p == '#') {
        // skip over the hash mark
        p++;
        fragment_begin = p;

        // the fragment is delimited by EOS
        for (; *p != '\0'; p++) {
            if (!is_fragment_character(*p)) {
                return false;
            }
        }
        fragment_end = p;
    }

    if (scheme_begin) {
        _scheme = {scheme_begin, scheme_end - scheme_begin};
        if (_scheme == "unix")
            _af = AF_UNIX;
    }

    if (uinfo_begin)
        _user_info = {uinfo_begin, uinfo_end - uinfo_begin};

    if (host_begin) {
        _host = {host_begin, host_end - host_begin};
        if (_af != AF_UNIX)
            _af = _host.find_first_of(':') == std::string::npos ? AF_INET : AF_INET6;
    }

    if (port_end)
        _port = {port_begin, port_end - port_begin};
    else {
        auto it = default_ports.find(std::string(_scheme));
        if (it != std::end(default_ports))
            _port = it->second;
    }

    if (path_begin)
        _path = {path_begin, path_end - path_begin};
    else
        _path = "/";

    if (query_begin) {
        _raw_queries = {query_begin, query_end - query_begin};
        static const std::regex query_regex("(&?)([^=]*)=([^&]*)");

        auto search = query_begin;
        const auto end = query_end;
        std::cmatch what;
        while (std::regex_search(search, end, what, query_regex)) {
            _queries[decode(what[2].first,
                                 static_cast<std::size_t>(what[2].length()))]
                .push_back(
                    decode(what[3].first,
                           what[3].first + static_cast<std::size_t>(what[3].length())));
            search += what[0].length();
        }
    }

    if (fragment_begin)
        _fragment = {fragment_begin, fragment_end - fragment_begin};

    return true;
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
uri::operator=(std::string &&str) noexcept {
    _source = std::move(str);
    parse();
    return *this;
}

uri &
uri::operator=(uri const &rhs) {
    _source = rhs._source;
    parse();
    return *this;
}

uri &
uri::operator=(uri &&rhs) noexcept {
    _source = std::move(rhs._source);
    parse();
    return *this;
}

} // namespace qb::io