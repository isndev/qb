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

#ifndef QB_MODULE_HTTP_H_
#define QB_MODULE_HTTP_H_
#include <http/http_parser.h>
#include <qb/io/async.h>
#include <qb/io/transport/file.h>
#include <qb/system/allocator/pipe.h>
#include <qb/system/container/unordered_map.h>
#include <qb/system/timestamp.h>
#include <regex>
#include <sstream>
#include <string_view>

#undef DELETE // Windows :/

namespace qb::http {
constexpr const char endl[] = "\r\n";
constexpr const char sep = ' ';

template <typename _IT>
std::string
urlDecode(_IT begin, _IT end) {
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

template <typename _String>
std::string
urlDecode(_String const &str) {
    return urlDecode(str.cbegin(), str.cend());
}

std::string urlDecode(const char *str, std::size_t size);

namespace internal {

template <typename _String>
struct MessageBase {
    using string_type = _String;

    uint16_t major_version;
    uint16_t minor_version;
    qb::icase_unordered_map<std::vector<_String>> headers;
    //    qb::unordered_map<_String, _String> headers;
    uint64_t content_length;
    bool upgrade;
    _String body;

    MessageBase() noexcept
        : major_version(1)
        , minor_version(1)
        , content_length(0) {
        reset();
    }

    template <typename T>
    const auto &
    header(T &&name, std::size_t const index = 0, _String const &not_found = "") const {
        const auto &it = this->headers.find(std::forward<T>(name));
        if (it != this->headers.cend() && index < it->second.size())
            return it->second[index];
        return not_found;
    }

    void
    reset() {
        headers.clear();
        body = {};
    }
};

} // namespace internal

template <typename _MessageType>
struct Parser : public http_parser {
    using _String = typename _MessageType::string_type;

    static int
    on_message_begin(http_parser *) {
        return 0;
    }

    static int
    on_url(http_parser *parser, const char *at, size_t length) {
        static const std::regex query_regex("(\\?|&)([^=]*)=([^&]*)");
        if constexpr (_MessageType::type == HTTP_REQUEST) {
            auto &msg = static_cast<Parser *>(parser->data)->msg;
            msg.method = static_cast<http_method>(parser->method);
            msg.url = _String(at, length);
            auto has_query = msg.url.find('?');
            if (has_query != std::string::npos) {
                msg.path = _String(at, has_query);

                const char *search = at + has_query;
                std::cmatch what;
                while (std::regex_search(search, at + length, what, query_regex)) {
                    msg.queries[_String(what[2].first,
                                        static_cast<std::size_t>(what[2].length()))]
                        .push_back(std::move(urlDecode(
                            what[3].first, static_cast<std::size_t>(what[3].length()))));
                    search += what[0].length();
                }
            } else
                msg.path = msg.url;
        }
        return 0;
    }

    static int
    on_status(http_parser *parser, const char *at, size_t length) {
        if constexpr (_MessageType::type == HTTP_RESPONSE) {
            auto &msg = static_cast<Parser *>(parser->data)->msg;
            msg.status_code = static_cast<http_status>(parser->status_code);
            msg.status = _String(at, length);
        }
        return 0;
    }

    static int
    on_header_field(http_parser *parser, const char *at, size_t length) {
        static_cast<Parser *>(parser->data)->_last_header_key = _String(at, length);
        return 0;
    }

    static int
    on_header_value(http_parser *parser, const char *at, size_t length) {
        auto &msg = static_cast<Parser *>(parser->data)->msg;
        msg.headers[_String{static_cast<Parser *>(parser->data)->_last_header_key}]
            .push_back(_String(at, length));
        return 0;
    }

    static int
    on_headers_complete(http_parser *parser) {
        auto &msg = static_cast<Parser *>(parser->data)->msg;
        msg.major_version = parser->http_major;
        msg.minor_version = parser->http_major;
        if (parser->content_length != ULLONG_MAX)
            msg.content_length = parser->content_length;
        msg.upgrade = static_cast<bool>(parser->upgrade);
        static_cast<Parser *>(parser->data)->_headers_completed = true;
        return 1;
    }

    static int
    on_body(http_parser *parser, const char *at, size_t length) {
        static_cast<Parser *>(parser->data)->msg.body = _String(at, length);
        return 0;
    }

    static int
    on_message_complete(http_parser *parser) {
        return 1;
    }

    /* When on_chunk_header is called, the current chunk length is stored
     * in parser->content_length.
     */
    static int
    on_chunk_header(http_parser *) {
        // TODO : implement this
        return 0;
    }

    static int
    on_chunk_complete(http_parser *) {
        // TODO : implement this
        return 0;
    }

protected:
    _MessageType msg;

private:
    static const http_parser_settings settings;
    _String _last_header_key;
    bool _headers_completed = false;

public:
    Parser() noexcept {
        this->data = this;
        reset();
    };

    std::size_t
    parse(const char *buffer, std::size_t const size) {
        return http_parser_execute(static_cast<http_parser *>(this), &settings, buffer,
                                   size);
    }

    void
    reset() noexcept {
        http_parser_init(static_cast<http_parser *>(this), _MessageType::type);
        msg.reset();
        _headers_completed = false;
    }

    [[nodiscard]] _MessageType &
    getParsedMessage() noexcept {
        return msg;
    }

    bool
    headers_completed() const noexcept {
        return _headers_completed;
    }
};

/// Date class working with formats specified in RFC 7231 Date/Time Formats
class Date {
public:
    /// Returns the given std::chrono::system_clock::time_point as a string with the
    /// following format: Wed, 31 Jul 2019 11:34:23 GMT.
    static std::string
    to_string(qb::Timestamp const ts) noexcept {
        std::string result;
        result.reserve(29);

        const auto time = static_cast<int64_t>(ts.seconds());
        tm tm;
#if defined(_MSC_VER) || defined(__MINGW32__)
        if (gmtime_s(&tm, &time) != 0)
            return {};
        auto gmtime = &tm;
#else
        const auto gmtime = gmtime_r(&time, &tm);
        if (!gmtime)
            return {};
#endif

        switch (gmtime->tm_wday) {
        case 0:
            result += "Sun, ";
            break;
        case 1:
            result += "Mon, ";
            break;
        case 2:
            result += "Tue, ";
            break;
        case 3:
            result += "Wed, ";
            break;
        case 4:
            result += "Thu, ";
            break;
        case 5:
            result += "Fri, ";
            break;
        case 6:
            result += "Sat, ";
            break;
        }

        result +=
            gmtime->tm_mday < 10 ? '0' : static_cast<char>(gmtime->tm_mday / 10 + 48);
        result += static_cast<char>(gmtime->tm_mday % 10 + 48);

        switch (gmtime->tm_mon) {
        case 0:
            result += " Jan ";
            break;
        case 1:
            result += " Feb ";
            break;
        case 2:
            result += " Mar ";
            break;
        case 3:
            result += " Apr ";
            break;
        case 4:
            result += " May ";
            break;
        case 5:
            result += " Jun ";
            break;
        case 6:
            result += " Jul ";
            break;
        case 7:
            result += " Aug ";
            break;
        case 8:
            result += " Sep ";
            break;
        case 9:
            result += " Oct ";
            break;
        case 10:
            result += " Nov ";
            break;
        case 11:
            result += " Dec ";
            break;
        }

        const auto year = gmtime->tm_year + 1900;
        result += static_cast<char>(year / 1000 + 48);
        result += static_cast<char>((year / 100) % 10 + 48);
        result += static_cast<char>((year / 10) % 10 + 48);
        result += static_cast<char>(year % 10 + 48);
        result += ' ';

        result +=
            gmtime->tm_hour < 10 ? '0' : static_cast<char>(gmtime->tm_hour / 10 + 48);
        result += static_cast<char>(gmtime->tm_hour % 10 + 48);
        result += ':';

        result +=
            gmtime->tm_min < 10 ? '0' : static_cast<char>(gmtime->tm_min / 10 + 48);
        result += static_cast<char>(gmtime->tm_min % 10 + 48);
        result += ':';

        result +=
            gmtime->tm_sec < 10 ? '0' : static_cast<char>(gmtime->tm_sec / 10 + 48);
        result += static_cast<char>(gmtime->tm_sec % 10 + 48);

        result += " GMT";

        return result;
    }
};

template <typename _String = std::string>
struct Response : public internal::MessageBase<_String> {
    constexpr static const http_parser_type type = HTTP_RESPONSE;
    http_status status_code;
    _String status;

    Response() noexcept
        : status_code(HTTP_STATUS_OK) {}

    void
    reset() {
        status_code = HTTP_STATUS_OK;
        status = {};
        static_cast<internal::MessageBase<_String> &>(*this).reset();
    }

    template <typename _Session>
    class Router {
    public:
        struct Context {
            _Session &session;
            Response &response;

            const auto &
            header(_String const &name, _String const &not_found = "") const {
                return response.header(name, not_found);
            }
        };

    private:
        class IRoute {
        public:
            virtual ~IRoute() = default;
            virtual void process(Context &ctx) = 0;
        };

        template <typename _Func>
        class Route : public IRoute {
            _Func _func;

        public:
            Route(Route const &) = delete;
            Route(_Func &&func)
                : _func(func) {}

            virtual ~Route() = default;

            void
            process(Context &ctx) final {
                _func(ctx);
            }
        };

        qb::unordered_map<int, IRoute *> _routes;

    public:
        Router() = default;
        ~Router() noexcept {
            for (auto const &it : _routes)
                delete it.second;
        }

        bool
        route(_Session &session, Response &response) const {
            const auto &it = _routes.find(response.status_code);
            if (it != _routes.end()) {
                Context ctx{session, response};
                it->second->process(ctx);
                return true;
            }
            return false;
        }

#define REGISTER_ROUTE_FUNCTION(num, name, description)               \
    template <typename _Func>                                         \
    Router &name(_Func &&func) {                                      \
        _routes.emplace(static_cast<http_status>(num),                \
                        new Route<_Func>(std::forward<_Func>(func))); \
        return *this;                                                 \
    }

        HTTP_STATUS_MAP(REGISTER_ROUTE_FUNCTION)

#undef REGISTER_ROUTE_FUNCTION
    };
};

template <typename _String = std::string>
struct Request : public internal::MessageBase<_String> {
    constexpr static const http_parser_type type = HTTP_REQUEST;
    http_method method;
    _String url;
    _String path;
    qb::icase_unordered_map<std::vector<std::string>> queries;

public:
    Request() noexcept
        : method(HTTP_GET) {}

    template <typename T>
    [[nodiscard]] std::string const &
    query(T &&name, std::size_t const index = 0,
          std::string const &not_found = "") const {
        const auto &it = this->queries.find(std::forward<T>(name));
        if (it != this->queries.cend() && index < it->second.size())
            return it->second[index];

        return not_found;
    }

    void
    reset() {
        method = HTTP_GET;
        url = {};
        path = {};
        queries.clear();
        static_cast<internal::MessageBase<_String> &>(*this).reset();
    }

    template <typename _Session>
    class Router {
        using PathParameters = qb::unordered_map<std::string, std::string>;

    public:
        struct Context {
            _Session &session;
            const Request &request;
            PathParameters parameters;
            Response<std::string> response;

            [[nodiscard]] const auto &
            header(_String const &name, _String const &not_found = "") const {
                return request.header(name, not_found);
            }

            [[nodiscard]] std::string const &
            param(std::string const &name, std::string const &not_found = "") const {
                const auto &it = parameters.find(name);
                return it != parameters.cend() ? it->second : not_found;
            }

            [[nodiscard]] std::string const &
            query(_String const &name, std::string const &not_found = "") const {
                return request.query(name, not_found);
            }
        };

    private:
        class IRoute {
        public:
            virtual ~IRoute() = default;
            virtual void process(Context &ctx) = 0;
        };

        class ARoute : public IRoute {
        public:
            using ParameterNames = std::vector<std::string>;

        private:
            ParameterNames _param_names;
            PathParameters _parameters;
            const std::regex _regex;

            std::string
            init(std::string const &path) {
                std::string build_regex = path, search = path;
                const std::regex pieces_regex("/:(\\w+)");
                std::smatch what;
                while (std::regex_search(search, what, pieces_regex)) {
                    _param_names.push_back(what[1]);
                    _parameters.emplace(*_param_names.rbegin(), "");
                    build_regex = build_regex.replace(build_regex.find(what[0]),
                                                      what[0].length(), "/(.+)");
                    search = what.suffix();
                }

                return std::move(build_regex);
            }

        public:
            explicit ARoute(std::string const &path)
                : _regex(init(path)) {}

            virtual ~ARoute() = default;

            template <typename _Path>
            bool
            match(_Path const &path) {
                std::match_results<typename _Path::const_iterator> what;
                auto ret = std::regex_match(path.cbegin(), path.cend(), what, _regex);
                if (ret) {
                    for (size_t i = 1; i < what.size(); ++i) {
                        _parameters[_param_names[i - 1]] =
                            std::move(qb::http::urlDecode(what[i].str()));
                    }
                }
                return ret;
            }

            PathParameters &
            parameters() {
                return _parameters;
            }

            virtual void process(Context &ctx) = 0;
        };

        template <typename _Func>
        class Route : public ARoute {
            _Func _func;

        public:
            Route(Route const &) = delete;
            Route(std::string const &path, _Func &&func)
                : ARoute(path)
                , _func(func) {}

            virtual ~Route() = default;

            void
            process(Context &ctx) final {
                ctx.parameters = std::move(this->parameters());
                _func(ctx);
            }
        };

        using Routes = std::vector<ARoute *>;
        qb::unordered_map<int, Routes> _routes;
        Response<std::string> _default_response;

    public:
        Router() = default;
        ~Router() noexcept {
            for (auto const &it : _routes) {
                for (auto route : it.second)
                    delete route;
            }
        }

        Router &
        setDefaultResponse(Response<std::string> const &res) {
            _default_response = {res};
            return *this;
        }

        [[nodiscard]] Response<std::string> const &
        getDefaultResponse() const {
            return _default_response;
        }

        bool
        route(_Session &session, Request const &request) const {
            const auto &it = _routes.find(request.method);
            if (it != _routes.end()) {
                for (const auto route : it->second) {
                    if (route->match(request.path)) {
                        Context ctx{session, request, {}, _default_response};
                        route->process(ctx);
                        return true;
                    }
                }
            }
            return false;
        }

#define REGISTER_ROUTE_FUNCTION(num, name, description)                            \
    template <typename _Func>                                                      \
    Router &name(std::string const &path, _Func &&func) {                          \
        _routes[num].push_back(new Route<_Func>(path, std::forward<_Func>(func))); \
        return *this;                                                              \
    }                                                                              \
    template <typename _Func>                                                      \
    Router &name(std::vector<std::string> paths, _Func &&func) {                   \
        for (const auto &path : paths)                                             \
            name(path, std::forward<_Func>(func));                                 \
        return *this;                                                              \
    }

        HTTP_METHOD_MAP(REGISTER_ROUTE_FUNCTION)

#undef REGISTER_ROUTE_FUNCTION
    };
};

class Chunk {
    const char *_data;
    std::size_t _size;

public:
    Chunk()
        : _data(nullptr)
        , _size(0) {}
    Chunk(const char *data, std::size_t size)
        : _data(data)
        , _size(size) {}
    const char *
    data() const {
        return _data;
    }
    std::size_t
    size() const {
        return _size;
    }
};

} // namespace qb::http

namespace qb::protocol {
namespace http_internal {

template <typename _IO_, typename _Trait>
class base : public qb::io::async::AProtocol<_IO_> {
    using _String = typename qb::http::Parser<std::remove_const_t<_Trait>>::_String;
    std::size_t body_offset = 0;

protected:
    qb::http::Parser<std::remove_const_t<_Trait>> _http_obj;

public:
    template <typename _Session>
    using Router = typename _Trait::template Router<_Session>;

    base() = delete;
    base(_IO_ &io) noexcept
        : qb::io::async::AProtocol<_IO_>(io) {}

    std::size_t
    getMessageSize() noexcept final {
        if (!_http_obj.headers_completed()) {
            // parse headers
            const auto ret =
                _http_obj.parse(this->_io.in().begin(), this->_io.in().size());
            if (!_http_obj.headers_completed()) {
                // restart parsing for next time;
                _http_obj.reset();
                return 0;
            }

            auto &msg = _http_obj.getParsedMessage();

            if (msg.headers.has("Transfer-Encoding")) {
                // not implemented
                this->not_ok();
                return 0;
            }

            body_offset = ret;
        }

        auto &msg = _http_obj.getParsedMessage();
        const auto full_size = body_offset + msg.content_length;
        if (this->_io.in().size() < full_size) {
            // if is protocol view reset parser for next read
            if constexpr (std::is_same_v<std::string_view, _String>) {
                _http_obj.reset();
                body_offset = 0;
            }
            return 0; // incomplete body
        }

        if (msg.content_length)
            _http_obj.getParsedMessage().body =
                _String(this->_io.in().cbegin() + body_offset, msg.content_length);

        body_offset = 0;

        return full_size;
    }

    void
    reset() noexcept final {
        body_offset = 0;
        _http_obj.reset();
    }
};

} // namespace http_internal

template <typename _IO_>
class http_server : public http_internal::base<_IO_, qb::http::Request<std::string>> {
    using base_t = http_internal::base<_IO_, qb::http::Request<std::string>>;

public:
    http_server() = delete;
    explicit http_server(_IO_ &io) noexcept
        : base_t(io) {}

    struct request {
        const std::size_t size;
        const char *data;
        qb::http::Request<std::string> http;
    };

    void
    onMessage(std::size_t size) noexcept final {
        this->_io.on(request{size, this->_io.in().begin(),
                             std::move(this->_http_obj.getParsedMessage())});
        this->_http_obj.reset();
    }
};

template <typename _IO_>
class http_server_view
    : public http_internal::base<_IO_, qb::http::Request<std::string_view>> {
    using base_t = http_internal::base<_IO_, qb::http::Request<std::string_view>>;

public:
    http_server_view() = delete;
    explicit http_server_view(_IO_ &io) noexcept
        : base_t(io) {}

    struct request {
        const std::size_t size;
        const char *data;
        const qb::http::Request<std::string_view> http;
    };

    void
    onMessage(std::size_t size) noexcept final {
        this->_io.on(request{size, this->_io.in().begin(),
                             std::move(this->_http_obj.getParsedMessage())});
        this->_http_obj.reset();
    }
};

template <typename _IO_>
class http_client : public http_internal::base<_IO_, qb::http::Response<std::string>> {
    using base_t = http_internal::base<_IO_, qb::http::Response<std::string>>;

public:
    http_client() = delete;
    explicit http_client(_IO_ &io) noexcept
        : base_t(io) {}

    struct response {
        const std::size_t size;
        const char *data;
        qb::http::Response<std::string> http;
    };

    void
    onMessage(std::size_t size) noexcept final {
        this->_io.on(response{size, this->_io.in().begin(),
                              std::move(this->_http_obj.getParsedMessage())});
        this->_http_obj.reset();
    }
};

template <typename _IO_>
class http_client_view
    : public http_internal::base<_IO_, qb::http::Response<std::string_view>> {
    using base_t = http_internal::base<_IO_, qb::http::Response<std::string_view>>;

public:
    http_client_view() = delete;
    explicit http_client_view(_IO_ &io) noexcept
        : base_t(io) {}

    struct response {
        const std::size_t size;
        const char *data;
        const qb::http::Response<std::string_view> http;
    };

    void
    onMessage(std::size_t size) noexcept final {
        this->_io.on(response{size, this->_io.in().begin(),
                              std::move(this->_http_obj.getParsedMessage())});
        this->_http_obj.reset();
    }
};

} // namespace qb::protocol

namespace qb::http {

namespace internal {

template <typename _IO_, bool has_server = _IO_::has_server>
struct side {
    using protocol = qb::protocol::http_server<_IO_>;
    using protocol_view = qb::protocol::http_server_view<_IO_>;
};

template <typename _IO_>
struct side<_IO_, false> {
    using protocol = qb::protocol::http_client<_IO_>;
    using protocol_view = qb::protocol::http_client_view<_IO_>;
};

} // namespace internal

template <typename _IO_>
using protocol = typename internal::side<_IO_>::protocol;

template <typename _IO_>
using protocol_view = typename internal::side<_IO_>::protocol_view;

} // namespace qb::http

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put<qb::http::Request<std::string>>(const qb::http::Request<std::string> &r);

template <>
pipe<char> &pipe<char>::put<qb::http::Response<std::string>>(
    const qb::http::Response<std::string> &r);

template <>
pipe<char> &pipe<char>::put<qb::http::Chunk>(const qb::http::Chunk &c);

} // namespace qb::allocator

#endif // QB_MODULE_HTTP_H_
