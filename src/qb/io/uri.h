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

#ifndef QB_IO_URI_H_
#define QB_IO_URI_H_

#include <charconv>
#include <cstdint>
#include <iterator>
#include <system_error>
#include <qb/io/config.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/build_macros.h>
#include <string>
#include <string_view>
#include <vector>

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
    return c == ':' || c == '/' || c == '?' || c == '#' || c == '[' || c == ']' || c == '@';
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
    return is_unreserved(c) || is_sub_delim(c) || c == '%' || c == '@' || c == ':' || c == '[' || c == ']';
}

/**
 * @brief Checks if a character is valid for the path part of a URI
 * @param c The character to check
 * @return true if the character is valid for path, false otherwise
 */
inline bool
is_path_character(int c) {
    return is_unreserved(c) || is_sub_delim(c) || c == '%' || c == '/' || c == ':' || c == '@';
}

/**
 * @brief Checks if a character may stand for itself in the path of an HTTP request target a server received
 * @param c The character to check
 * @return true for a path character (is_path_character()) and for `[`, `]` and `|`, false otherwise
 *
 * `[`, `]` and `|` are the octets RFC 3986 forbids in a path that the WHATWG URL Standard's path percent-encode set
 * leaves as they are (§1.3): a browser sends them unencoded. The set leaves `\` too, which stays out: a browser sends
 * `/` for it in an http URL, and Windows takes it for a separator. `^` is percent-encoded since 2025. uri::origin_form()
 * and uri::absolute_form() read a path with this set; a URI reference, uri(text), with is_path_character().
 */
inline bool
is_request_path_character(int c) {
    return is_path_character(c) || c == '[' || c == ']' || c == '|';
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
    int                                               _af    = AF_INET; /**< Address family (AF_INET, AF_INET6, AF_UNIX) */
    bool                                              _valid = true;    /**< Whether the last parse() succeeded (see is_valid()). */
    std::string                                       _source;          /**< Source string of the URI */
    std::string_view                                  _scheme;          /**< URI scheme (e.g., http, https, ftp) */
    std::string_view                                  _user_info;       /**< User information in the URI */
    std::string_view                                  _host;            /**< Host name or IP address in the URI */
    std::string_view                                  _port;            /**< Port number in the URI */
    std::string_view                                  _path;            /**< Path in the URI */
    std::string_view                                  _raw_queries;     /**< Raw query string in the URI */
    std::string_view                                  _fragment;        /**< Fragment in the URI */
    qb::icase_unordered_map<std::vector<std::string>> _queries;         /**< Parsed and decoded query parameters */

    constexpr static const char no_path[] = "/"; /**< Default path */

    bool parse_queries(std::size_t pos) noexcept; /**< Parse query parameters */
    //    bool parse_v6(std::size_t pos) noexcept;                /**< Parse an IPv6
    //    address */ bool parse_v4() noexcept;                               /**< Parse
    //    an IPv4 address */

    /// How parse() reads the source: as a URI reference (the constructors), or as one of the HTTP request-target
    /// forms a server receives (origin_form(), absolute_form(), authority_form()). An enumeration, not a bool: a
    /// string literal converts to a bool, and the public `parse("...")` would resolve to this private member.
    enum class reading : std::uint8_t { reference, origin_form, absolute_form, authority_form };

    /**
     * @brief Parse the source URI and fill the object fields
     * @param how The reading: a reference, or the form origin_form(), absolute_form() or authority_form() reads
     * @return true if parsing succeeded, false otherwise
     */
    bool parse(reading how = reading::reference) noexcept;

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

    /**
     * @brief Parse query parameters without using regex
     * @param begin Pointer to the beginning of the query string
     * @param end Pointer to the end of the query string
     */
    void parse_query_parameters(const char *begin, const char *end);

    /**
     * @brief Takes @p rhs's family, validity and components, each view re-pointed from @p rhs_text (where @p rhs's
     *        text was) at this object's text; a view of static storage (a default port, the default path) is kept.
     */
    void adopt_views(uri const &rhs, const char *rhs_text, std::size_t rhs_size) noexcept;

public:
    /**
     * @brief Default constructor
     */
    uri() = default;

    /**
     * @brief Move constructor
     * @param rhs Source URI to move from; left as a default-constructed uri
     * @note Copies and moves carry the value as it is -- components, validity, family and the query map with any
     *       edit made through queries() -- without parsing the text again.
     */
    uri(uri &&rhs) noexcept;

    /**
     * @brief Copy constructor
     * @param rhs Source URI to copy from (its query map is copied as it is, edits included)
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
    static const signed char tbl[256];

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
    decode(_IT begin, _IT end) {
        std::string out;
        char        c;
        int         v1 = 0;
        int         v2 = 0;

        while (begin != end) {
            c = *(begin++);
            if (c == '%') {
                if (std::distance(begin, end) < 2)
                    break;
                if ((v1 = tbl[(unsigned char) *(begin++)]) < 0 || (v2 = tbl[(unsigned char) *(begin++)]) < 0) {
                    break;
                }
                c = static_cast<char>((v1 << 4) | v2);
            }
            out += c;
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
    encode(_IT begin, _IT end) {
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
     * @brief Decodes a URI-encoded string view
     * @param input The string view to decode
     * @return The decoded string
     */
    static std::string decode(std::string_view input) noexcept;

    /**
     * @brief Encodes a string view in URI format
     * @param input The string view to encode
     * @return The encoded string
     */
    static std::string encode(std::string_view input) noexcept;

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
     * @brief Reads an HTTP request target in origin-form (RFC 9112 §3.2.1): an absolute path and an optional query,
     *        with no scheme and no authority.
     * @param target The target as it was received, e.g. `/a/b?x=1`.
     * @return The target as `uri(target)` reads it, except that nothing in it is an authority: `//a/b` is the path
     *         `//a/b` with an empty host(), where a URI reference (RFC 3986 §4.2) names the host `a` and the path
     *         `/b`; and that the path may hold `[`, `]` and `|`, which a browser sends unencoded
     *         (is_request_path_character()). is_valid() is false when @p target does not begin with '/' or its path
     *         holds any other byte a URI path may not hold. The query and a fragment are read as the constructor reads
     *         them: no request target holds a fragment, and a server refuses one itself.
     * @note A server builds the URI of an origin-form request with this function and takes the authority from the
     *       `Host` field (RFC 9112 §3.3), and that of an absolute-form one with absolute_form().
     */
    [[nodiscard]] static uri origin_form(std::string target) noexcept;

    /**
     * @brief Reads an HTTP request target in absolute-form (RFC 9112 §3.2.2): a URI with a scheme and an authority,
     *        as a server receives one.
     * @param target The target as it was received, e.g. `http://example.com/a|b?x=1`; also the text an HTTP/2 or
     *        HTTP/3 server composes from `:scheme`, `:authority` and `:path`.
     * @return The target as `uri(target)` reads it, except that the path may hold `[`, `]` and `|`, which a browser
     *         sends unencoded (is_request_path_character()), as origin_form() reads one. is_valid() is false when
     *         @p target does not begin with a scheme and `://`, or its path holds any other byte a URI path may not
     *         hold. The authority is read as a reference's -- a userinfo, an empty host are accepted -- and the query
     *         and a fragment too: a server checks what HTTP requires of them itself.
     * @note A URI a client builds is a reference, `uri(text)`, which refuses `[`, `]` and `|` in a path.
     */
    [[nodiscard]] static uri absolute_form(std::string target) noexcept;

    /**
     * @brief Reads an HTTP request target in authority-form (RFC 9112 §3.2.3): the host and the port of a CONNECT
     *        request, `uri-host ":" port`, and nothing else.
     * @param target The target as it was received, e.g. `example.com:443` or `[2001:db8::1]:443`.
     * @return The host() and the port() the text names, no scheme, the path `/`. is_valid() is false when the text
     *         is not a host and a port: an empty host, no port, a userinfo, a second colon in a reg-name, a path, a
     *         query or a fragment. The host's characters and the port's digits are judged as in a reference's
     *         authority -- an IP literal is not checked to be an IPv6 address, a port is any number of digits --
     *         which a server checks itself. A URI reference reads `example.com:443` as the scheme `example.com`.
     */
    [[nodiscard]] static uri authority_form(std::string target) noexcept;

    /**
     * @brief Reads an HTTP request target in origin-form (RFC 9112 §3.2.1) into THIS uri, as origin_form() reads it
     *        into a new one: the text replaces source(), the components are parsed in place, the query map's storage
     *        is kept.
     * @param target The target as it was received, e.g. `/a/b?x=1`.
     * @return is_valid(): false when @p target does not begin with '/' or holds a byte a path may not hold.
     * @note The reader of a server that parses request after request into one message: a uri built apart and moved
     *       into the request costs two moves of its strings and its query map, this costs one assignment of the text
     *       (Huly QB-1177: the moves were 830 instructions a request on g++ 14, 54 ns of a 263 ns parse).
     */
    bool read_origin_form(std::string_view target) noexcept;

    /**
     * @brief Reads an HTTP request target in absolute-form (RFC 9112 §3.2.2) into THIS uri, as absolute_form() reads
     *        it into a new one: the text replaces source(), the components are parsed in place, the query map's
     *        storage is kept.
     * @param target The target as it was received, e.g. `http://example.com/a|b?x=1`.
     * @return is_valid(): false when @p target does not begin with a scheme and `://`, or its path holds a byte a
     *         request path may not hold, as for absolute_form().
     */
    bool read_absolute_form(std::string_view target) noexcept;

    /**
     * @brief Reads an HTTP request target in authority-form (RFC 9112 §3.2.3) into THIS uri, as authority_form()
     *        reads it into a new one: the text replaces source(), the components are parsed in place.
     * @param target The target as it was received, e.g. `example.com:443`.
     * @return is_valid(): false when the text is not a host and a port, as for authority_form().
     */
    bool read_authority_form(std::string_view target) noexcept;

    /**
     * @brief Copy assignment operator
     * @param rhs Source URI to copy from (its query map is copied as it is, edits included)
     * @return Reference to this URI
     */
    uri &operator=(uri const &rhs);

    /**
     * @brief Move assignment operator
     * @param rhs Source URI to move from; left as a default-constructed uri
     * @return Reference to this URI
     */
    uri &operator=(uri &&rhs) noexcept;

    /**
     * @brief String assignment operator
     * @param str The source string for the URI
     * @return Reference to this URI
     * @note The result is what `uri(str)` gives: the text is parsed afresh and its address family classified from
     *       it, never kept from the previous text (a `unix://` uri assigned a `tcp://` text is AF_INET or AF_INET6).
     */
    uri &operator=(std::string const &str);

    /**
     * @brief String move assignment operator
     * @param str The source string for the URI (will be moved)
     * @return Reference to this URI
     * @note As the copying form: the result is what `uri(std::move(str))` gives.
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
     * @brief Whether the source string parsed into a structurally valid URI.
     * @return `false` if the last `parse()` rejected the input (invalid authority,
     *         path or port character, an unclosed IPv6 bracket, or text after the
     *         closing bracket that is not `:port`); `true` otherwise.
     * @note The constructors and assignment operators always run `parse()`; this
     *       accessor lets callers detect a malformed URI without re-parsing.
     */
    [[nodiscard]] inline bool
    is_valid() const noexcept {
        return _valid;
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
        const auto sv        = port();
        int        p         = 0;
        const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), p);
        // Reject malformed or out-of-range ports rather than silently
        // truncating: e.g. "99999" would otherwise wrap to 34463.
        if (ec != std::errc{} || ptr != sv.data() + sv.size() || p < 0 || p > 65535) {
            return 0;
        }
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

    /** @brief Whether a query delimiter occurred, including an explicitly empty query. */
    [[nodiscard]] bool
    has_query() const noexcept {
        return _raw_queries.data() != nullptr;
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
     * @brief Returns the parsed query parameters of this URI (non-const version)
     * @return Mutable reference to the query parameters map
     * @note An edit belongs to this uri's value: query() sees it and a copy or a move carries it. The text --
     *       source() and encoded_queries() -- is not rewritten by it.
     */
    [[nodiscard]] inline auto &
    queries() {
        return _queries;
    }

    /**
     * @brief Returns the value of a specific query parameter (zero-copy).
     *
     * @tparam T Parameter name type
     * @param name Name of the parameter to retrieve
     * @param index Index of the value to retrieve (for multi-value parameters)
     * @return A constant reference to the parameter's value if present, otherwise to a static empty
     *         string. The reference is always safe to keep (never a reference to a temporary). For a
     *         custom fallback, use `query_or()`.
     */
    template <typename T>
    [[nodiscard]] std::string const &
    query(T &&name, std::size_t const index = 0) const {
        static const std::string empty;
        const auto              &it = this->_queries.find(std::forward<T>(name));
        if (it != this->_queries.cend() && index < it->second.size())
            return it->second[index];

        return empty;
    }

    /**
     * @brief Returns a query parameter value, or `fallback` if absent / `index` is out of bounds.
     *
     * Returns BY VALUE, so the fallback (literal, temporary, or lvalue) is always safe.
     *
     * @tparam T Parameter name type
     * @param name Name of the parameter to retrieve
     * @param fallback Value returned when the parameter is absent (taken by value, moved out on a miss)
     * @param index Index of the value to retrieve (for multi-value parameters)
     */
    template <typename T>
    [[nodiscard]] std::string
    query_or(T &&name, std::string fallback, std::size_t const index = 0) const {
        const auto &it = this->_queries.find(std::forward<T>(name));
        if (it != this->_queries.cend() && index < it->second.size())
            return it->second[index];

        return fallback;
    }

    /**
     * @brief Returns the fragment of this URI
     * @return The fragment
     */
    [[nodiscard]] inline auto
    fragment() const {
        return _fragment;
    }

    /**
     * @brief Validates if a string is a valid URI scheme
     * @param scheme The scheme to validate
     * @return true if valid, false otherwise
     */
    static bool is_valid_scheme(std::string_view scheme) noexcept;

    /**
     * @brief Validates if a string is a valid URI host
     * @param host The host to validate
     * @return true if valid, false otherwise
     */
    static bool is_valid_host(std::string_view host) noexcept;

    /**
     * @brief Normalizes a path by resolving '.', '..' segments and standardizing slashes
     * @param path Path to normalize (will be modified)
     * @return true if successful, false otherwise
     */
    static bool normalize_path(std::string &path) noexcept;
};
} // namespace qb::io

#endif // QB_IO_URI_H_
