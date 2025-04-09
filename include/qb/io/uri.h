/**
 * @file qb/io/uri.h
 * @brief URI parsing and manipulation utilities
 *
 * This file provides a comprehensive URI (Uniform Resource Identifier) implementation
 * that follows RFC 3986 standards. It includes functionality for parsing, encoding,
 * decoding, and manipulating URIs with support for schemes, authority components,
 * paths, queries, and fragments.
 *
 * The implementation supports both IPv4 and IPv6 addressing formats and provides
 * utility functions for character classification according to URI specifications.
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
 * @ingroup IO
 */

#include <charconv>
#include <qb/io/config.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/build_macros.h>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#ifndef QB_IO_URI_H_
#define QB_IO_URI_H_

namespace qb::io {

/**
 * @brief Checks if a character is alphanumeric
 * @param c The character to check
 * @return true if the character is alphanumeric, false otherwise
 */
inline bool
is_alnum(int c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * @brief Checks if a character is unreserved according to RFC 3986
 * @param c The character to check
 * @return true if the character is unreserved, false otherwise
 */
inline bool
is_unreserved(int c) {
    return is_alnum((char) c) || c == '-' || c == '.' || c == '_' || c == '~';
}

/**
 * @brief Checks if a character is a general delimiter according to RFC 3986
 * @param c The character to check
 * @return true if the character is a general delimiter, false otherwise
 */
inline bool
is_gen_delim(int c) {
    return c == ':' || c == '/' || c == '?' || c == '#' || c == '[' || c == ']' ||
           c == '@';
}

/**
 * @brief Checks if a character is a subdelimiter according to RFC 3986
 * @param c The character to check
 * @return true if the character is a subdelimiter, false otherwise
 */
inline bool
is_sub_delim(int c) {
    switch (c) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
            return true;
        default:
            return false;
    }
}

/**
 * @brief Checks if a character is reserved according to RFC 3986
 * @param c The character to check
 * @return true if the character is reserved, false otherwise
 */
inline bool
is_reserved(int c) {
    return is_gen_delim(c) || is_sub_delim(c);
}

/**
 * @brief Checks if a character is valid for a URI scheme
 * @param c The character to check
 * @return true if the character is valid for a scheme, false otherwise
 */
inline bool
is_scheme_character(int c) {
    return is_alnum((char) c) || c == '+' || c == '-' || c == '.';
}

/**
 * @brief Checks if a character is valid for URI user information
 * @param c The character to check
 * @return true if the character is valid for user information, false otherwise
 */
inline bool
is_user_info_character(int c) {
    return is_unreserved(c) || is_sub_delim(c) || c == '%' || c == ':';
}

/**
 * @brief Checks if a character is valid for the authority part of a URI
 * @param c The character to check
 * @return true if the character is valid for authority, false otherwise
 */
inline bool
is_authority_character(int c) {
    return is_unreserved(c) || is_sub_delim(c) || c == '%' || c == '@' || c == ':' ||
           c == '[' || c == ']';
}

/**
 * @brief Checks if a character is valid for the path part of a URI
 * @param c The character to check
 * @return true if the character is valid for path, false otherwise
 */
inline bool
is_path_character(int c) {
    return is_unreserved(c) || is_sub_delim(c) || c == '%' || c == '/' || c == ':' ||
           c == '@';
}

/**
 * @brief Checks if a character is valid for the query part of a URI
 * @param c The character to check
 * @return true if the character is valid for query, false otherwise
 */
inline bool
is_query_character(int c) {
    return is_path_character(c) || c == '?';
}

/**
 * @brief Checks if a character is valid for the fragment part of a URI
 * @param c The character to check
 * @return true if the character is valid for fragment, false otherwise
 */
inline bool
is_fragment_character(int c) {
    // this is intentional, they have the same set of legal characters
    return is_query_character(c);
}

/**
 * @class uri
 * @brief Class for parsing, manipulating, and representing URIs
 *
 * This class implements URIs according to RFC 3986 and supports
 * URI components such as scheme, authority (including host and port),
 * path, query, and fragment. It also handles encoding and decoding
 * of special characters and supports IPv4 and IPv6 addressing formats.
 */
class uri {
    int              _af = AF_INET; /**< Address family (AF_INET, AF_INET6, AF_UNIX) */
    std::string      _source;       /**< Source string of the URI */
    std::string_view _scheme;       /**< URI scheme (e.g., http, https, ftp) */
    std::string_view _user_info;    /**< User information in the URI */
    std::string_view _host;         /**< Host name or IP address in the URI */
    std::string_view _port;         /**< Port number in the URI */
    std::string_view _path;         /**< Path in the URI */
    std::string_view _raw_queries;  /**< Raw query string in the URI */
    std::string_view _fragment;     /**< Fragment in the URI */
    qb::icase_unordered_map<std::vector<std::string>>
        _queries; /**< Parsed and decoded query parameters */

    constexpr static const char no_path[] = "/"; /**< Default path */

    bool parse_queries(std::size_t pos) noexcept; /**< Parse query parameters */
    //    bool parse_v6(std::size_t pos) noexcept;                /**< Parse an IPv6
    //    address */ bool parse_v4() noexcept;                               /**< Parse
    //    an IPv4 address */

    /**
     * @brief Parse the source URI and fill the object fields
     * @return true if parsing succeeded, false otherwise
     */
    bool parse() noexcept;

    /**
     * @brief Initialize the URI from a string
     * @param rhs The source string for the URI
     * @return true if initialization succeeded, false otherwise
     */
    bool from(std::string const &rhs) noexcept;

    /**
     * @brief Initialize the URI from a string (move version)
     * @param rhs The source string for the URI (will be moved)
     * @return true if initialization succeeded, false otherwise
     */
    bool from(std::string &&rhs) noexcept;

public:
    /**
     * @brief Default constructor
     */
    uri() = default;

    /**
     * @brief Move constructor
     * @param rhs Source URI to move from
     */
    uri(uri &&rhs) noexcept;

    /**
     * @brief Copy constructor
     * @param rhs Source URI to copy from
     */
    uri(uri const &rhs);

    /**
     * @brief Constructor from a string
     * @param str The source string for the URI
     * @param af The address family (defaults to AF_INET)
     */
    uri(std::string const &str, int af = AF_INET);

    /**
     * @brief Constructor from a string (move version)
     * @param str The source string for the URI (will be moved)
     * @param af The address family (defaults to AF_INET)
     */
    uri(std::string &&str, int af = AF_INET) noexcept;

    /**
     * @brief Conversion table for hexadecimal characters
     * Used for decoding %XX sequences to characters
     */
    static const char tbl[256];

    /**
     * @brief Decodes a sequence of URI-encoded characters
     *
     * Converts %XX sequences to corresponding characters
     *
     * @tparam _IT Iterator type
     * @param begin Begin iterator
     * @param end End iterator
     * @return The decoded string
     */
    template <typename _IT>
    static std::string
    decode(_IT begin, _IT end) noexcept {
        std::string out;
        char        c, v1, v2{};

        while (begin != end) {
            c = *(begin++);
            if (c == '%') {
                if ((v1 = tbl[(unsigned char) *(begin++)]) < 0 ||
                    (v2 = tbl[(unsigned char) *(begin++)]) < 0) {
                    break;
                }
                c = (v1 << 4) | v2;
            }
            out += (c);
        }

        return out;
    }

    /**
     * @brief Hexadecimal characters for encoding
     */
    constexpr static const char hex[] = "0123456789ABCDEF";

    /**
     * @brief Encodes a sequence of characters in URI format
     *
     * Converts special characters to %XX sequences
     *
     * @tparam _IT Iterator type
     * @param begin Begin iterator
     * @param end End iterator
     * @return The encoded string
     */
    template <typename _IT>
    static std::string
    encode(_IT begin, _IT end) noexcept {
        std::string encoded;

        encoded.reserve(static_cast<ptrdiff_t>(end - begin) * 3);
        for (auto iter = begin; iter != end; ++iter) {
            // for utf8 encoded string, char ASCII can be greater than 127.
            int ch = static_cast<unsigned char>(*iter);
            // ch should be same under both utf8 and utf16.
            if (!is_unreserved(ch) && !is_reserved(ch)) {
                encoded.push_back('%');
                encoded.push_back(hex[(ch >> 4) & 0xF]);
                encoded.push_back(hex[ch & 0xF]);
            } else {
                // ASCII don't need to be encoded, which should be same on both utf8 and
                // utf16.
                encoded.push_back((char) ch);
            }
        }

        return encoded;
    }

    /**
     * @brief Decodes a URI-encoded string
     * @param input The string to decode
     * @return The decoded string
     */
    static std::string decode(std::string const &input) noexcept;

    /**
     * @brief Encodes a string in URI format
     * @param input The string to encode
     * @return The encoded string
     */
    static std::string encode(std::string const &input) noexcept;

    /**
     * @brief Decodes a URI-encoded string view
     * @param input The string view to decode
     * @return The decoded string
     */
    static std::string decode(std::string_view const &input) noexcept;

    /**
     * @brief Encodes a string view in URI format
     * @param input The string view to encode
     * @return The encoded string
     */
    static std::string encode(std::string_view const &input) noexcept;

    /**
     * @brief Decodes a URI-encoded memory block
     * @param input Pointer to the data to decode
     * @param size Size of the data
     * @return The decoded string
     */
    static std::string decode(const char *input, std::size_t size) noexcept;

    /**
     * @brief Encodes a memory block in URI format
     * @param input Pointer to the data to encode
     * @param size Size of the data
     * @return The encoded string
     */
    static std::string encode(const char *input, std::size_t size) noexcept;

    /**
     * @brief Creates and parses a URI from a string
     * @param str The source string for the URI
     * @param af The address family (defaults to AF_INET)
     * @return A parsed URI instance
     */
    static uri parse(std::string const &str, int af = AF_INET) noexcept;

    /**
     * @brief Copy assignment operator
     * @param rhs Source URI to copy from
     * @return Reference to this URI
     */
    uri &operator=(uri const &rhs);

    /**
     * @brief Move assignment operator
     * @param rhs Source URI to move from
     * @return Reference to this URI
     */
    uri &operator=(uri &&rhs) noexcept;

    /**
     * @brief String assignment operator
     * @param str The source string for the URI
     * @return Reference to this URI
     */
    uri &operator=(std::string const &str);

    /**
     * @brief String move assignment operator
     * @param str The source string for the URI (will be moved)
     * @return Reference to this URI
     */
    uri &operator=(std::string &&str) noexcept;

    /**
     * @brief Returns the address family of this URI
     * @return The address family (AF_INET, AF_INET6, AF_UNIX)
     */
    [[nodiscard]] inline auto
    af() const {
        return _af;
    }

    /**
     * @brief Returns the source string of this URI
     * @return The source string
     */
    [[nodiscard]] inline const auto &
    source() const {
        return _source;
    }

    /**
     * @brief Returns the scheme of this URI
     * @return The scheme (e.g., http, https, ftp)
     */
    [[nodiscard]] inline auto
    scheme() const {
        return _scheme;
    }

    /**
     * @brief Returns the user information of this URI
     * @return The user information
     */
    [[nodiscard]] inline auto
    user_info() const {
        return _user_info;
    }

    /**
     * @brief Returns the host name or IP address of this URI
     * @return The host name or IP address
     */
    [[nodiscard]] inline auto
    host() const {
        return _host;
    }

    /**
     * @brief Returns the port number of this URI
     * @return The port number as a string
     */
    [[nodiscard]] inline auto
    port() const {
        return _port;
    }

    /**
     * @brief Returns the port number of this URI as a numeric value
     * @return The port number as an unsigned 16-bit integer
     */
    [[nodiscard]] uint16_t
    u_port() const {
        int p = 0;

        std::from_chars(port().data(), port().data() + port().size(), p);
        return static_cast<uint16_t>(p);
    }

    /**
     * @brief Returns the path of this URI
     * @return The path
     */
    [[nodiscard]] inline auto
    path() const {
        return _path;
    }

    /**
     * @brief Returns the raw query string of this URI
     * @return The raw query string
     */
    [[nodiscard]] inline const auto &
    encoded_queries() const {
        return _raw_queries;
    }

    /**
     * @brief Returns the parsed query parameters of this URI
     * @return The query parameters as a map
     */
    [[nodiscard]] inline const auto &
    queries() const {
        return _queries;
    }

    /**
     * @brief Returns the value of a specific query parameter
     *
     * @tparam T Parameter name type
     * @param name Name of the parameter to retrieve
     * @param index Index of the value to retrieve (for multi-value parameters)
     * @param not_found Value to return if the parameter is not found
     * @return The parameter value or not_found if not found
     */
    template <typename T>
    [[nodiscard]] std::string const &
    query(T &&name, std::size_t const index = 0,
          std::string const &not_found = "") const {
        const auto &it = this->_queries.find(std::forward<T>(name));
        if (it != this->_queries.cend() && index < it->second.size())
            return it->second[index];

        return not_found;
    }

    /**
     * @brief Returns the fragment of this URI
     * @return The fragment
     */
    [[nodiscard]] inline auto
    fragment() const {
        return _fragment;
    }
};
} // namespace qb::io

#endif // QB_IO_URI_H_
