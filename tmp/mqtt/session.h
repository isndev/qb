//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>

#include "../../session/types.h"
#include "messages.h"
#include "reader.h"

#ifndef QB_MQTT_SESSION_H
#define QB_MQTT_SESSION_H

namespace qb {
    namespace mqtt {

        template <typename EventData, typename Derived>
        class Session {
        protected:
            using Pipe = qb::allocator::pipe<char>;
            Pipe in_pipe;
            Pipe out_pipe;

            qb::mqtt::Reader reader;
            std::vector<session::ReturnValue (Derived::*)(EventData &)> messages;

        private:
            // uint32_t max_bytes_to_receive;
            uint32_t max_bytes_to_send;
            uint32_t sent_bytes;
        protected:

            // Todo: improve this publish
            bool publish(void const *data, std::size_t const size) {
                if (static_cast<Derived const &>(*this).canPublish() && out_pipe.end() + size < max_bytes_to_send) {
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

            uint32_t sentBytes() const {
                return sent_bytes;
            }

            void resetStats() {
                sent_bytes = 0;
            }

        public:

            Session(uint32_t const queue_limit = 134217728)
                    : max_bytes_to_send(queue_limit ? queue_limit : ~queue_limit)
                    , sent_bytes(0)
            {
                messages.resize(qb::mqtt::MessageType::END);
                reset_auth();
            }

            ~Session() {}

            // Cube events
            bool onInitialize() {
                // init accepted messages
                messages.resize(qb::mqtt::MessageType::END);
                reset_auth();
                return static_cast<Derived &>(*this).onInitialize();
            }

            void onDisconnect(EventData &event) {
                static_cast<Derived &>(*this).onDisconnect(event);
            }

            session::ReturnValue onRead(EventData &event) {
                // Socket read workflow
                const std::size_t expected = reader.expected();
                auto ret = session::ReturnValue::REPOLL;
                if (static_cast<Derived const &>(*this).canRead()) {
                    std::size_t received = 0;
                    char *data = in_pipe.allocate_back(expected);
                    reader.setHeader(in_pipe.data());
                    if (event.receive(data, expected, received)) {
                        in_pipe.free_back(expected - received);
                        reader.read(received);

                        if (reader.isComplete()) {
                            // process message
                            auto type = reader.header().getType();
                            auto callback = &Derived::onDisconnect;
                            if (type < qb::mqtt::MessageType::END) {
                                callback = messages[type];
                            }

                            ret = ((static_cast<Derived *>(this)->*(callback))(event));
                            in_pipe.free_back(reader.readBytes());
                            reader.reset();
                        }
                        if constexpr (Derived::has_keepalive) {
                            static_cast<Derived &>(*this).reset_timer();
                        }
                    } else
                        ret = session::ReturnValue::KO;
                }
                return ret;
            }

            session::ReturnValue onWrite(EventData &event) {
                // Socket write workflow
                auto ret = session::ReturnValue::REPOLL;
                if (static_cast<Derived const &>(*this).canWrite() && out_pipe.begin() != out_pipe.end()) {
                    std::size_t sent = out_pipe.end() - out_pipe.begin();

                    if (event.send(out_pipe.data() + out_pipe.begin(), (std::min)(2048ul, sent), sent)) {
                        sent_bytes += sent;
                        out_pipe.free_front(sent);
                        if (out_pipe.begin() == out_pipe.end()) {
                            out_pipe.reset();
                        }
                    } else
                        ret = session::ReturnValue::KO;
                    if constexpr (Derived::has_keepalive) {
                        static_cast<Derived &>(*this).reset_timer();
                    }
                }
                return ret;
            }

            session::ReturnValue onTimeout(EventData &event) {
                if constexpr (std::is_void<std::void_t<decltype(std::declval<Derived *>()->onTimeout(event))>>::value)
                    return session::ReturnValue::KO;
                else
                    return static_cast<Derived &>(*this).onTimeout(event);
            }
        };

    }
}

#endif //QB_MQTT_SESSION_H
