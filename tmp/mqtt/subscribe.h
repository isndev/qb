//
// Created by isndev on 7/22/18.
//

#include "header.h"

#ifndef QB_MQTT_SUBSCRIBE_H
#define QB_MQTT_SUBSCRIBE_H

namespace qb {
    namespace mqtt {

        struct Topic {
            const uint16_t size;
            const char *topic;
            const uint8_t qos;

            Topic(const uint16_t size, const char *topic, uint8_t const qos)
                    : size(size), topic(topic), qos(qos) {
            }

            std::string name() const {
                return {topic, size};
            }

        };

        class Subscribe {
            uint16_t packetId;

        public:
            Subscribe(uint16_t const id)
                    : packetId(short_mqtt(id)) {
            }

            uint16_t getPacketId() const {
                return short_mqtt(packetId);
            }

            template<typename Func>
            std::size_t decodeTopics(Func const &func, std::size_t const remaining_bytes) {
                std::size_t decoded = 2;
                std::size_t nb_topics = 0;
                while (decoded < remaining_bytes) {
                    if (decoded + 2 <= remaining_bytes) {
                        uint16_t topic_size = short_mqtt(
                                *reinterpret_cast<const uint16_t *>(reinterpret_cast<const char *>(this) + decoded));
                        decoded += 2;
                        if (decoded + topic_size + 1 <= remaining_bytes) {
                            func(Topic(topic_size,
                                       reinterpret_cast<const char *>(this) + decoded,
                                       *reinterpret_cast<const uint8_t *>(reinterpret_cast<const char *>(this) +
                                                                          decoded + topic_size)));
                            decoded += topic_size + 1;
                        } else
                            return nb_topics;
                    } else
                        return nb_topics;
                    ++nb_topics;
                }
                return nb_topics;
            }

//        void encodeTopics(std::vector<std::string> const &topics) {
//            std::size_t offset = 0;
//            for (auto &topic : topics) {
//                *reinterpret_cast<uint16_t *>(reinterpret_cast<char *>(this) + offset) = short_mqtt(topic.size());
//                memcpy(reinterpret_cast<char *>(this) + offset + sizeof(uint16_t), topic.c_str(), topic.size());
//                *reinterpret_cast<uint16_t *>(reinterpret_cast<char *>(this) + offset) = short_mqtt(topic.size());
//            }
//
//
//            memcpy(reinterpret_cast<char *>(this) + sizeof(packetId), topic.c_str(), getTopicSize());
//        }
//

        } __attribute__ ((__packed__));


        class SubAck {
            const uint16_t packetId;
        public:
            SubAck(uint16_t packetId)
                    : packetId(short_mqtt(packetId)) {}

        } __attribute__ ((__packed__));


        class Unsubscribe {
            uint16_t packetId;

        public:
            Unsubscribe(uint16_t const id)
                    : packetId(short_mqtt(id)) {
            }

            uint16_t getPacketId() const {
                return short_mqtt(packetId);
            }

            template<typename Func>
            std::size_t decodeTopics(Func const &func, std::size_t const remaining_bytes) {
                std::size_t decoded = 2;
                std::size_t nb_topics = 0;
                while (decoded < remaining_bytes) {
                    if (decoded + 2 <= remaining_bytes) {
                        uint16_t topic_size = short_mqtt(
                                *reinterpret_cast<const uint16_t *>(reinterpret_cast<const char *>(this) + decoded));
                        decoded += 2;
                        if (decoded + topic_size <= remaining_bytes) {
                            func(Topic(topic_size,
                                       reinterpret_cast<const char *>(this) + decoded,
                                       0));
                            decoded += topic_size;
                        } else
                            return nb_topics;
                    } else
                        return nb_topics;
                    ++nb_topics;
                }
                return nb_topics;
            }
        };

        class UnsubAck
                : public FixedHeader {
            const uint16_t packetId;
        public:
            UnsubAck(uint16_t const packetId)
                    : packetId(short_mqtt(packetId)) {
                setType(MessageType::UNSUBACK);
                remaining_length = 2;
            }

            uint16_t getPacketId() const {
                return short_mqtt(packetId);
            }
        } __attribute__ ((__packed__));

    } // namespace mqtt
} // namespace qb

#endif //QB_MQTT_SUBSCRIBE_H
