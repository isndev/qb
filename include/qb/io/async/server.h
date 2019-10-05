//
// Created by isnDev on 9/17/2019.
//

#ifndef QB_IO_ASYNC_SERVER_H
#define QB_IO_ASYNC_SERVER_H

#include "io.h"

namespace qb {
    namespace io {
        namespace async {

            template<typename _Derived, typename _Session, typename _Prot>
            class server : public input<server<_Derived, _Session, _Prot>, _Prot> {
            public:
                using base_t = input<server<_Derived, _Session, _Prot>, _Prot>;
                using session_map_t = std::unordered_map<uint64_t, _Session>;
            private:
                session_map_t _sessions;
            public:
                server() = default;

                session_map_t &sessions() { return _sessions; }

                void on(typename _Prot::message_type new_io, std::size_t size) {
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

#endif //QB_IO_ASYNC_SERVER_H
