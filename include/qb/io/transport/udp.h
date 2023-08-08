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

#ifndef QB_IO_TRANSPORT_UDP_H_
#define QB_IO_TRANSPORT_UDP_H_
#include "../stream.h"
#include "../udp/socket.h"
#include <qb/utility/functional.h>

namespace qb::io::transport {

class udp : public stream<io::udp::socket> {
    using base_t = stream<io::udp::socket>;

public:
    constexpr static const bool has_reset_on_pending_read = true;

    struct identity : public qb::io::endpoint {
        identity() = default;
        identity(identity const &) = default;
        identity(qb::io::endpoint const &ep)
            : qb::io::endpoint(ep) {}

        struct hasher {
            std::size_t
            operator()(const identity &id) const noexcept {
                return std::hash<std::string_view>{}(
                    std::string_view(reinterpret_cast<const char *>(&id), id.len()));
            }
        };

        bool
        operator!=(identity const &rhs) const noexcept {
            return std::string_view(reinterpret_cast<const char *>(this), len()) !=
                   std::string_view(reinterpret_cast<const char *>(&rhs), rhs.len());
        }
    };

    class ProxyOut {
        udp &proxy;

    public:
        ProxyOut(udp &prx)
            : proxy(prx) {}

        template <typename T>
        auto &
        operator<<(T &&data) {
            auto &out_buffer = static_cast<udp::base_t &>(proxy).out();
            const auto start_size = out_buffer.size();
            out_buffer << std::forward<T>(data);
            auto p = reinterpret_cast<udp::pushed_message *>(out_buffer.begin() +
                                                             proxy._last_pushed_offset);
            p->size += (out_buffer.size() - start_size);
            return *this;
        }

        std::size_t
        size() {
            return static_cast<udp::base_t &>(proxy).pendingWrite();
        }
    };

    friend ProxyOut;

private:
    ProxyOut _out{*this};
    udp::identity _remote_source;
    udp::identity _remote_dest;

    struct pushed_message {
        udp::identity ident;
        int size = 0;
        int offset = 0;
    };

    int _last_pushed_offset = -1;

public:
    const udp::identity &
    getSource() const noexcept {
        return _remote_source;
    }

    void
    setDestination(udp::identity const &to) noexcept {
        if (to != _remote_dest) {
            _remote_dest = to;
            _last_pushed_offset = -1;
        }
    }

    auto &
    out() {
        if (_last_pushed_offset < 0) {
            _last_pushed_offset = static_cast<int>(_out_buffer.size());
            auto &m = _out_buffer.allocate_back<pushed_message>();
            m.ident = _remote_dest;
            m.size = 0;
        }

        return _out;
    }

    int
    read() noexcept {
        const auto ret =
            transport().read(_in_buffer.allocate_back(io::udp::socket::MaxDatagramSize),
                             io::udp::socket::MaxDatagramSize, _remote_source);
        if (qb::likely(ret > 0))
            _in_buffer.free_back(io::udp::socket::MaxDatagramSize - ret);
        setDestination(_remote_source);
        return ret;
    }

    void
    eof() noexcept {
        _in_buffer.reset();
    }

    int
    write() noexcept {
        if (!_out_buffer.size())
            return 0;

        auto &msg = *reinterpret_cast<pushed_message *>(_out_buffer.begin());
        auto begin = _out_buffer.begin() + sizeof(pushed_message) + msg.offset;

        const auto ret = transport().write(
            begin,
            std::min(msg.size - msg.offset,
                     static_cast<int>(io::udp::socket::MaxDatagramSize)),
            msg.ident);
        if (qb::likely(ret > 0)) {
            msg.offset += ret;

            if (msg.offset == msg.size) {
                _out_buffer.free_front(msg.size + sizeof(pushed_message));
                if (_out_buffer.size()) {
                    _out_buffer.reorder();
                    if (_last_pushed_offset > 0) {
                        _last_pushed_offset = -1;
                    }
                } else
                    _out_buffer.reset();
            }
        }
        return ret;
    }

    char *
    publish(char const *data, std::size_t size) noexcept {
        return publish_to(_remote_dest, data, size);
    }

    char *
    publish_to(udp::identity const &to, char const *data, std::size_t size) noexcept {
        auto &m = _out_buffer.allocate_back<pushed_message>();
        m.ident = to;
        m.size = static_cast<int>(size);

        return static_cast<char *>(
            std::memcpy(_out_buffer.allocate_back(size), data, size));
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_UDP_H_
