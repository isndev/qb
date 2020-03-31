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

#ifndef QB_IO_ASYNC_UDP_SERVER_H
#define QB_IO_ASYNC_UDP_SERVER_H

#include "../io.h"
#include "../../transport/udp.h"

namespace qb {
    namespace io {
        namespace async {
            namespace udp {

                template<typename _Derived, typename _Session>
                class server : public io<server<_Derived, _Session>, transport::udp> {
                public:
                    using base_t = io<server<_Derived, _Session>, transport::udp>;
                    using session_map_t = qb::unordered_map<transport::udp::identity, _Session, transport::udp::identity::hasher>;
                private:
                    session_map_t _sessions;
                public:
                    server() = default;

                    session_map_t &sessions() { return _sessions; }

                    void on(transport::udp::message_type message, std::size_t size) {
                        auto it = _sessions.find(message.ident);
                        if (it == _sessions.end())
                        {
                            it = _sessions.emplace(message.ident, static_cast<_Derived &>(*this)).first;
                            it->second.ident() = message.ident;
                        }
//                            return; // drop the message

                        memcpy(it->second.buffer().allocate_back(size), message.data, size);
                        auto ret = 0;
                        while ((ret = it->second.getMessageSize()) > 0) {
                            it->second.on(it->second.getMessage(ret), ret);
                            it->second.flush(ret);
                        }
                    }

                    void stream(char const *message, std::size_t size) {
                        for (auto &session : sessions())
                            session.second.publish(message, size);
                    }

                    bool disconnected() const {
                        throw std::runtime_error("Server had been disconnected");
                        return true;
                    }

                    void disconnected(transport::udp::identity ident) {
                        _sessions.erase(ident);
                    }
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_UDP_SERVER_H
