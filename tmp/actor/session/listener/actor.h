//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>
#include <exception>

#include "network/include/cube/network/tcp.h"
#include "engine/actor/session/actor.h"

#include "events.h"

#ifndef CUBE_SESSION_LISTENER_ACTOR_H
#define CUBE_SESSION_LISTENER_ACTOR_H

namespace cube {
    namespace session {
        namespace listener {

            // example trait to implement
            struct ExampleTrait {

                bool onInitialize();
                void onConnect(network::SocketTCP event);
            };

            template <typename Derived>
            class Actor
                    : public session::Actor<Actor<Derived>> {
            public:
                constexpr static const service::iopoll::Type type = service::iopoll::READ;
                constexpr static const bool has_keepalive = false;
            private:
                network::Listener listener;
                const uint8_t io_core_id;

            protected:
                inline network::Listener &getListener() { return listener; }
            public:
                Actor() = delete;
                Actor(uint8_t core, unsigned short port, network::ip ip = network::ip::Any)
                    : io_core_id(core)
                {
                    listener.listen(port, ip);
                    listener.setBlocking(false);
                    if (listener.isBlocking())
                        throw std::runtime_error("failed to set blocking socket listener");
                }

                bool onInitialize() {
                    if (!listener.good())
                        return false;
                    auto &e = push<event::Subscribe>(getServiceId<service::iopoll::Tag>(io_core_id));
                    e.setEvents(EPOLLIN | EPOLLONESHOT);
                    e.setHandle(this->getListener().raw());
                    return static_cast<Derived &>(*this).onInitialize();
                }

                cube::session::ReturnValue onRead(event::Ready &event) {
                    network::SocketTCP socket;

                    if (listener.accept(socket) == network::Socket::Done) {
                        static_cast<Derived &>(*this).onConnect(socket.raw());
                        LOG_INFO << "Accepted new connection";
                    } else {
                        LOG_WARN << "Failed to accept new connection" << listener.raw();
                    }

                    return ReturnValue::REPOLL;
                }

                cube::session::ReturnValue onDisconnect(event::Ready &) {
                    LOG_CRIT << "Actor listener is down";
                    this->kill();
                    return ReturnValue::KO;
                }
            };

        }
    }
}

#endif //CUBE_SESSION_MQTT_ACTOR_H
