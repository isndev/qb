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

#ifndef QB_IO_ASYNC_TCP_SERVER_H
#define QB_IO_ASYNC_TCP_SERVER_H

#include "../io.h"
#include "../../prot/accept.h"

namespace qb {
    namespace io {
        namespace async {
            namespace tcp {

                template<typename _Derived, typename _Session>
                class server : public input<server<_Derived, _Session>, prot::accept> {
                public:
                    using base_t = input<server<_Derived, _Session>, prot::accept>;
                    using session_map_t = std::unordered_map<uint64_t, _Session>;
                private:
                    session_map_t _sessions;
                public:
                    server() = default;

                    session_map_t &sessions() { return _sessions; }

                    void on(prot::accept::message_type new_io, std::size_t size) {
                        const auto &it = sessions().emplace(
                                std::piecewise_construct,
                                std::forward_as_tuple(new_io.ident()),
                                std::forward_as_tuple(std::ref(static_cast<_Derived &>(*this)))
                        );
                        it.first->second.in().set(new_io.ident());
                        it.first->second.start();
                        static_cast<_Derived &>(*this).on(it.first->second);
                    }

                    void stream(char const *message, std::size_t size) {
                        for (auto &session : sessions())
                            session.second.publish(message, size);
                    }

                    bool disconnected() const {
                        throw std::runtime_error("Server had been disconnected");
                        return true;
                    }

                    void disconnected(int ident) {
                        _sessions.erase(ident);
                    }
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_TCP_SERVER_H
