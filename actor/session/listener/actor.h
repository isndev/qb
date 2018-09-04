//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>

#include "../../../network/Listener.h"
#include "../actor.h"

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

            template <typename Handler, typename Derived>
            class Actor
                    : public session::Actor<Handler, Actor<Handler, Derived>> {
            public:
                constexpr static const session::Type type = session::Type::READ;
            private:
                network::Listener listener;

            protected:
                inline network::Listener &getListener() { return listener; }
            public:
                Actor() = delete;
                Actor(unsigned short port, network::ip ip = network::ip::Any) {
                    LOG_INFO << "Start listening on port " << port;

                    listener.listen(port, ip);
                    listener.setBlocking(false);
                }

                bool onInitialize() {
                    if (!listener.good())
                        return false;

                    this->template registerEvent<event::Ready>(*this);
                    return static_cast<Derived &>(*this).onInitialize();
                }

                bool onRead(event::Ready &event) {
                    network::SocketTCP socket;

                    if (listener.accept(socket) == network::Socket::Done) {
                        static_cast<Derived &>(*this).onConnect(socket.raw());
                        LOG_INFO << "Accepted new connection";
                    } else {
                        LOG_WARN << "Failed to accept new connection";
                    }

                    if (event.repoll()) {
                        LOG_CRIT << "Failed to repoll new connection";
                        return false;
                    }
                    return true;
                }

                void onDisconnect(event::Ready &event) {

                }
            };

        }
    }
}

#endif //CUBE_SESSION_MQTT_ACTOR_H
