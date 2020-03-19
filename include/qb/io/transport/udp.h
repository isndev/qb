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

#ifndef             QB_IO_TRANSPORT_UDP_H_
# define            QB_IO_TRANSPORT_UDP_H_
# include <qb/utility/functional.h>
# include <qb/system/allocator/pipe.h>
# include "../udp/socket.h"

namespace qb {
    namespace io {
        namespace transport {

            class udp {
            public:
                struct identity {
                    struct hasher {
                        std::size_t operator() (const identity& id) const {
                            return hash_combine(id._ip.toInteger(),
                                                id._port);
                        }
                    };

                    ip _ip;
                    uint16_t _port;

                    bool operator==(identity const &rhs) const {
                        return _ip == rhs._ip && _port == rhs._port;
                    }
                };

                struct message_type {
                    udp::identity ident;
                    const char *data;
                };

            protected:
                io::udp::socket _io;
                qb::allocator::pipe<char> _in_buffer;
                qb::allocator::pipe<char> _out_buffer;
            private:
                message_type _message;

                struct pushed_message {
                    udp::identity ident;
                    int size;
                };

                pushed_message _pushed_message;
            public:

                // in section
                io::udp::socket &in() {
                    return _io;
                }

                auto &buffer() {
                    return _in_buffer;
                }

                int read() {
                    _in_buffer.reset();
                    const auto ret = _io.read(
                            _in_buffer.allocate_back(io::udp::socket::MaxDatagramSize)
                            , io::udp::socket::MaxDatagramSize
                            , _message.ident._ip
                            , _message.ident._port);
                    if (qb::likely(ret > 0))
                        _in_buffer.free_back(io::udp::socket::MaxDatagramSize - ret);
                    return ret;
                }

                void flush(std::size_t size) {
                    _in_buffer.free_front(size);
                    if (!_in_buffer.size())
                        _in_buffer.reset();
                }
                // out sections
                io::udp::socket &out() {
                    return _io;
                }

                std::size_t pendingWrite() const {
                    return _out_buffer.size();
                }

                int write() {
                    if (!_pushed_message.size) {
                        _pushed_message = *reinterpret_cast<pushed_message *>(_out_buffer.data() + _out_buffer.begin());
                        _out_buffer.free_front(sizeof(pushed_message));
                    }

                    const auto ret = this->_io.write(
                            _out_buffer.data() + _out_buffer.begin(),
                            std::min(_pushed_message.size, static_cast<int>(io::udp::socket::MaxDatagramSize)),
                            _pushed_message.ident._ip,
                            _pushed_message.ident._port
                    );
                    if (qb::likely(ret > 0)) {
                        _pushed_message.size -= ret;
                        _out_buffer.reset(_out_buffer.begin() + ret);
                    }
                    return ret;
                }

                char *publish(udp::identity const &to, char const *data, std::size_t size) {
                    auto &m = _out_buffer.allocate_back<pushed_message>();
                    m.ident = to;
                    m.size = size;

                    return static_cast<char *>(std::memcpy(_out_buffer.allocate_back(size), data, size));
                }

                void close() {
                    _io.close();
                }

                int getMessageSize() {
                    _message.data = _in_buffer.data() + _in_buffer.begin();
                    return _in_buffer.size();
                }

                message_type getMessage(int) {
                    return _message;
                }
            };

        } // namespace transport
    } // namespace io
} // namespace qb

#endif // QB_IO_TRANSPORT_UDP_H_
