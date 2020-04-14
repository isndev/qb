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

#ifndef QB_MODULE_WS_H_
#define QB_MODULE_WS_H_

#ifndef QB_IO_WITH_SSL
#    error "websocket protocol requires OpenSSL crypto library"
#endif

#include "../http/http.h"
#include <qb/io/crypto.h>
#include <random>

namespace qb::http::ws {
enum opcode : unsigned char { Text = 129, Binary = 130, Close = 136 };

struct Message {
    unsigned char fin_rsv_opcode = 0;
    bool masked = true;
    qb::allocator::pipe<char> _data;

    [[nodiscard]] std::size_t
    size() const noexcept {
        return _data.size();
    }

    template <typename T>
    Message &
    operator<<(T const &data) {
        _data << data;
        return *this;
    }

    void
    reset() {
        fin_rsv_opcode = 0;
        _data.reset();
    }
};

struct MessageText : public Message {
    MessageText() {
        fin_rsv_opcode = opcode::Text;
    }
};

struct MessageBinary : public Message {
    MessageBinary() {
        fin_rsv_opcode = opcode::Binary;
    }
};

enum CloseStatus : int {
    Normal = 1000,
    GoingAway = 1001,
    ProtocolError = 1002,
    DataNotAccepted = 1003,
    zReserved1 = 1004,
    zReserved2 = 1005,
    zReserved3 = 1006,
    DataNotConsistent = 1007,
    PolicyViolation = 1008,
    MessageTooBig = 1009,
    MissingExtension = 1010,
    UnexpectedReason = 1011,
    zReserved4 = 1012
};

struct MessageClose : Message {
    MessageClose() = delete;
    explicit MessageClose(int status = CloseStatus::Normal,
                          std::string const &reason = "closed normally") {
        fin_rsv_opcode = opcode::Close;
        _data << static_cast<unsigned char>(status >> 8)
              << static_cast<unsigned char>(status % 256) << reason;
    }
};

std::string generateKey() noexcept;

} // namespace qb::http::ws

namespace qb::http {
struct WebSocketRequest : public Request<> {
    WebSocketRequest() = delete;
    explicit WebSocketRequest(std::string const &key) {
        headers["Upgrade"].push_back("websocket");
        headers["Connection"].push_back("Upgrade");
        headers["Sec-WebSocket-Key"].push_back(key);
        headers["Sec-WebSocket-Version"].push_back("13");
    }
};
} // namespace qb::http

namespace qb::protocol {

namespace ws_internal {

template <typename _IO_>
class base : public io::async::AProtocol<_IO_> {
    std::size_t _parsed = 0;
    std::size_t _expected_size = 0;
    unsigned char fin_rsv_opcode = 0;
    qb::http::ws::Message _message;

public:
    // shared events
    struct ping {
        const std::size_t size;
        const char *data;
        qb::http::ws::Message &ws;
    };
    struct pong {
        const std::size_t size;
        const char *data;
        qb::http::ws::Message &ws;
    };

    struct message {
        const std::size_t size;
        const char *data;
        qb::http::ws::Message &ws;
    };
    // !shared events

    base() = delete;
    base(_IO_ &io)
        : io::async::AProtocol<_IO_>(io) {}

    std::size_t
    getMessageSize() noexcept final {
        if (!this->ok())
            return 0;

        auto &buffer = this->_io.in();
        const auto buffer_size = buffer.size();
        auto first_bytes = reinterpret_cast<const unsigned char *>(buffer.cbegin());
        if (!_parsed) {
            if (buffer_size < 2u)
                return 0;
            fin_rsv_opcode = first_bytes[0];
            _message.masked = (first_bytes[1] >= 128u);
            // only server side
            if constexpr (_IO_::has_server) {
                if (!_message.masked) {
                    // close if client has sent unmasked message
                    _message.reset();
                    int status = 1002;
                    _message.fin_rsv_opcode = 136u;
                    _message._data << static_cast<unsigned char>(status >> 8)
                                   << static_cast<unsigned char>(status % 256)
                                   << "message from client not masked";
                    this->_io << _message;
                    this->not_ok();
                    return 0u;
                }
            }

            _parsed += 2u;
        }
        if (!_expected_size) {
            std::size_t length = (first_bytes[1] & 127u);
            // 2 or 8 next bytes is the size of content
            std::size_t num_bytes = length == 126u ? 2u : (length == 127u ? 8u : 0u);
            if (num_bytes) {
                if (buffer_size < (num_bytes + 2u))
                    return 0u;
                // position after 2 firt bytes
                auto length_bytes =
                    reinterpret_cast<const unsigned char *>(buffer.cbegin() + 2u);
                length = 0u;
                for (std::size_t c = 0u; c < num_bytes; c++)
                    length += static_cast<std::size_t>(length_bytes[c])
                              << (8u * (num_bytes - 1u - c));
            }
            _expected_size = length;
            _parsed += num_bytes;
        }

        const auto full_size =
            _expected_size + _parsed + (first_bytes[1] >= 128u ? 4u : 0u);
        if (buffer_size < full_size)
            return 0;

        return full_size;
    }

    void
    onMessage(std::size_t size) noexcept final {
        if (!this->ok())
            return;

        auto &buffer = this->_io.in();

        // If fragmented message
        if ((fin_rsv_opcode & 0x80u) == 0 || (fin_rsv_opcode & 0x0fu) == 0) {
            if (!_message.size()) {
                _message.fin_rsv_opcode = fin_rsv_opcode | 0x80u;
            }
        } else
            _message.fin_rsv_opcode = fin_rsv_opcode;

        if (_message.masked) {
            // Read mask
            auto mask =
                reinterpret_cast<const unsigned char *>(buffer.cbegin() + _parsed);
            auto begin_buffer_data = buffer.begin() + _parsed + 4;
            auto begin_data = _message._data.allocate_back(_expected_size);
            for (auto i = 0u; i < _expected_size; ++i)
                begin_data[i] = begin_buffer_data[i] ^ mask[i % 4];
        } else {
            std::memcpy(_message._data.allocate_back(_expected_size),
                        buffer.begin() + _parsed, _expected_size);
        }

        // reply in condition
        if constexpr (_IO_::has_server)
            _message.masked = false;
        else
            _message.masked = true;

        // If connection close
        if ((fin_rsv_opcode & 0x0f) == 8) {
            _message.fin_rsv_opcode = 136u;
            this->_io << _message;
            this->not_ok();
        }
        // If ping
        else if ((fin_rsv_opcode & 0x0f) == 9) {
            if constexpr (has_method_on<_IO_, void, ping>::value) {
                this->_io.on(ping{_message.size(), _message._data.cbegin(), _message});
            }
            // Send pong
            _message.fin_rsv_opcode = fin_rsv_opcode + 1;
            this->_io << _message;
        }
        // If pong
        else if ((fin_rsv_opcode & 0x0f) == 10) {
            if constexpr (has_method_on<_IO_, void, pong>::value) {
                this->_io.on(pong{_message.size(), _message._data.cbegin(), _message});
            }
        }
        // If fragmented message and not final fragment
        else if ((fin_rsv_opcode & 0x80) == 0) {
            // next message no reset
        } else {
            this->_io.on(message{_message.size(), _message._data.cbegin(), _message});
            // next message + reset
        }

        if ((fin_rsv_opcode & 0x80) != 0)
            _message.reset();
        _expected_size = _parsed = fin_rsv_opcode = 0;
    }

    void
    reset() noexcept final {
        _message.reset();
        _expected_size = _parsed = fin_rsv_opcode = 0;
    }
};

} // namespace ws_internal

template <typename _IO_>
class ws_server : public ws_internal::base<_IO_> {
    std::string endpoint;

public:
    // server side event
    struct sending_http_response {
        qb::http::Response<> &response;
    };
    // !server side event

    ws_server() = delete;
    template <typename HttpRequest>
    ws_server(_IO_ &io, HttpRequest const &http)
        : ws_internal::base<_IO_>(io) {
        static auto ws_magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        if (http.upgrade) {
            std::string ws_key(http.header("Sec-WebSocket-Key"));
            if (!ws_key.empty()) {
                ws_key += ws_magic_string;
                qb::http::Response res;
                res.status_code = HTTP_STATUS_SWITCHING_PROTOCOLS;
                res.status = "Web Socket Protocol Handshake";
                res.headers["Upgrade"].push_back("websocket");
                res.headers["Connection"].push_back("Upgrade");
                res.headers["Sec-WebSocket-Accept"].push_back(
                    crypto::Base64::encode(crypto::sha1(ws_key)));

                if constexpr (has_method_on<_IO_, void, sending_http_response>::value) {
                    this->_io.on(sending_http_response{res});
                }

                this->_io << res;
                endpoint = http.path;
                return;
            }
            // error
        }
        this->not_ok();
    }
};

template <typename _IO_>
class ws_client : public ws_internal::base<_IO_> {
public:
    ws_client() = delete;
    template <typename HttpResponse>
    ws_client(_IO_ &io, HttpResponse const &http, std::string const &key)
        : ws_internal::base<_IO_>(io) {
        static const auto ws_magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        if (http.upgrade) {
            if (http.status_code == HTTP_STATUS_SWITCHING_PROTOCOLS) {
                const auto &res_key = http.header("Sec-WebSocket-Accept");
                if (!res_key.empty()) {
                    if (crypto::Base64::decode(std::string(res_key)) ==
                        crypto::sha1(key + ws_magic_string)) {
                        return;
                    }
                }
            }
            // error
        }
        this->not_ok();
    }
};

} // namespace qb::protocol

namespace qb::http::ws {

namespace internal {

template <typename _IO_, bool has_server = _IO_::has_server>
struct side {
    using protocol = qb::protocol::ws_server<_IO_>;
};

template <typename _IO_>
struct side<_IO_, false> {
    using protocol = qb::protocol::ws_client<_IO_>;
};

} // namespace internal

template <typename _IO_>
using protocol = typename internal::side<_IO_>::protocol;

} // namespace qb::http::ws

namespace qb::allocator {

template <>
pipe<char> &pipe<char>::put<qb::http::ws::Message>(const qb::http::ws::Message &msg);

template <>
pipe<char> &
pipe<char>::put<qb::http::ws::MessageText>(const qb::http::ws::MessageText &msg);

template <>
pipe<char> &
pipe<char>::put<qb::http::ws::MessageBinary>(const qb::http::ws::MessageBinary &msg);

template <>
pipe<char> &
pipe<char>::put<qb::http::ws::MessageClose>(const qb::http::ws::MessageClose &msg);

template <>
pipe<char> &
pipe<char>::put<qb::http::WebSocketRequest>(const qb::http::WebSocketRequest &msg);

} // namespace qb::allocator

#endif // QB_MODULE_WS_H_
