//
// Created by isnDev on 9/17/2019.
//

#ifndef QB_IO_ASYNC_SESSION_H
#define QB_IO_ASYNC_SESSION_H

#include "io.h"

namespace qb {
    namespace io {
        namespace async {

            template<typename _Derived, typename _Prot, typename _Server = void>
            class session
                    : public io<_Derived, _Prot> {
                using base_t = io<_Derived, _Prot>;
            protected:
                _Server &_server;
            public:
                constexpr static const bool has_server = true;

                session(listener &handler, _Server &server)
                        : base_t(handler), _server(server) {}

                _Server &server() {
                    return _server;
                }


                bool disconnected() const {
                    std::cout << "session disconnected" << std::endl;
                    return true;
                }
            };

            template<typename _Derived, typename _Prot>
            class session<_Derived, _Prot, void>
                    : public io<_Derived, _Prot> {
                using base_t = io<_Derived, _Prot>;
            public:

                session(listener &handler)
                        : base_t(handler) {}

                bool disconnected() const {
                    std::cout << "session disconnected" << std::endl;
                    return true;
                }
            };

        }
    }
}

#endif //QB_IO_ASYNC_SESSION_H
