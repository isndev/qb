//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>

#include "../actor.h"
#include "../../../modules/mqtt/messages.h"
#include "../../../modules/mqtt/reader.h"

#include "events.h"

#ifndef CUBE_SESSION_MQTT_ACTOR_H
#define CUBE_SESSION_MQTT_ACTOR_H
#define MAX_ZERO_QOS_BYTES 134217728
#define MAX_ONE_QOS_BYTES 66083840

namespace cube {
    namespace session {
        namespace mqtt {

            template <typename Handler, typename Derived>
            class Actor
                    : public cube::session::Actor<Handler, Actor<Handler, Derived>> {
            public:
                constexpr static const cube::session::Type type = cube::session::Type::READWRITE;
            protected:
                using Pipe = cube::allocator::pipe<char>;
                Pipe in_pipe;
                Pipe out_pipe;

                cube::mqtt::Reader reader;
                std::vector<void (Derived::*)(event::Ready &)> messages;

                // uint64_t  timer;
                // std::size_t counter;
                // void log_drop() {
                //     auto now = this->time();
                //     if (now >= timer) {
                //         LOG_INFO << "Drop Message " << counter;
                //         counter = 0;
                //         timer = now + cube::Timespan::seconds(1).nanoseconds();
                //     }
                // }

                // Todo: improve this publish
                bool publish(void const *data, std::size_t const size) {
                    if (out_pipe.end() + size < MAX_ZERO_QOS_BYTES) {
                        std::memcpy(out_pipe.allocate_back(size), static_cast<char const *>(data), size);
                        return true;
                    }
                    return false;
                }

                void publish(std::string const &str) {
                    publish(str.c_str(), str.size());
                }

                void reset_auth() {
                    for (auto &it : messages)
                        it = &Derived::onDisconnect;
                }

            public:

                Actor() {
                    messages.resize(cube::mqtt::MessageType::END);
                    reset_auth();
                }

                ~Actor() {}

                // Cube events
                bool onInitialize() {
                    // init accepted messages
                    messages.resize(cube::mqtt::MessageType::END);
                    reset_auth();
                    return static_cast<Derived &>(*this).onInitialize();
                }

                void onDisconnect(event::Ready &event) {
                    static_cast<Derived &>(*this).onDisconnect(event);
                }

                bool onRead(event::Ready &event) {
                    // Socket read workflow
                    const std::size_t expected = reader.expected();
                    std::size_t received = 0;
                    char *data = in_pipe.allocate_back(expected);
                    reader.setHeader(in_pipe.data());

                    if (event.tcp().receive(data, expected, received) == cube::network::Socket::Done) {
                        in_pipe.free_back(expected - received);
                        reader.read(received);

                        if (reader.isComplete()) {
                            // process message
                            auto type = reader.header().getType();
                            auto callback = &Derived::onDisconnect;
                            if (type < cube::mqtt::MessageType::END) {
                                callback = messages[type];
                            }

                            ((static_cast<Derived *>(this)->*(callback))(event));
                            in_pipe.free_back(reader.readBytes());
                            reader.reset();
                            if (callback == &Derived::onDisconnect)
                                return false;
                        } else
                            this->repoll(event);
                    }
                    else {
                        LOG_INFO << "EPOLLIN failed actorId:" << event.getOwner() << " ErrorCode:"
                                 << cube::network::Socket::getErrorStatus();
                        return false;
                    }
                    return true;
                }

                bool onWrite(event::Ready &event) {
                    // Socket write workflow
                    if (out_pipe.begin() != out_pipe.end()) {
                        std::size_t sent = out_pipe.end() - out_pipe.begin();

                        if (event.tcp().send(out_pipe.data() + out_pipe.begin(), (std::min)(2048ul, sent), sent) ==
                            cube::network::Socket::Done) {
                            if (sent) {
                                out_pipe.free_front(sent);
                                if (out_pipe.begin() == out_pipe.end()) {
                                    out_pipe.reset();
                                }
                            }
                        } else {
                            LOG_INFO << "EPOLLOUT failed Session:" << this->id();
                            return false;
                        }
                    }
                    return true;
                }
            };

        }
    }
}

#endif //CUBE_SESSION_MQTT_ACTOR_H
