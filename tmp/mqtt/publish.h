//
// Created by isndev on 7/22/18.
//

#include "header.h"
#include <string>

#ifndef QB_MQTT_PUBLISH_H
#define QB_MQTT_PUBLISH_H

namespace qb {
    namespace mqtt {

        class Publish {
            uint16_t topic_size;

        public:
            Publish(uint16_t const topic_size)
                    : topic_size(short_mqtt(topic_size)) {
            }

            uint16_t getTopicSize() const {
                return short_mqtt(topic_size);
            }

            void encodeTopic(std::string const &topic) {
                memcpy(reinterpret_cast<char *>(this) + sizeof(topic_size), topic.c_str(), getTopicSize());
            }

            std::string decodeTopic(std::size_t const remaining_bytes) const {
                if (sizeof(topic_size) + getTopicSize() <= remaining_bytes) {
                    return {std::basic_string(reinterpret_cast<const char *>(this) + sizeof(topic_size), getTopicSize(),
                                              std::allocator<char>())};
                }
                return {};
            }

        } __attribute__ ((__packed__));

        class PubAck : public FixedHeader {
            uint16_t packetId;

        public:
            PubAck() = delete;

            PubAck(uint16_t id) : packetId(short_mqtt(id)) {
                setType(MessageType::PUBACK);
                remaining_length = 2;
            }

            uint16_t getPacketId() const {
                return short_mqtt(packetId);
            }
        } __attribute__ ((__packed__));

        class PubRec : public FixedHeader {
            uint16_t packetId;

        public:
            PubRec() = delete;

            PubRec(uint16_t id) : packetId(short_mqtt(id)) {
                setType(MessageType::PUBREC);
                remaining_length = 2;
            }

            uint16_t getPacketId() const {
                return short_mqtt(packetId);
            }
        } __attribute__ ((__packed__));

        class PubRel : public FixedHeader {
            uint16_t packetId;

        public:
            PubRel() = delete;

            PubRel(uint16_t id) : packetId(short_mqtt(id)) {
                setType(MessageType::PUBREL);
                remaining_length = 2;
            }

            uint16_t getPacketId() const {
                return short_mqtt(packetId);
            }
        } __attribute__ ((__packed__));

        class PubComp : public FixedHeader {
            uint16_t packetId;

        public:
            PubComp() = delete;

            PubComp(uint16_t id) : packetId(short_mqtt(id)) {
                setType(MessageType::PUBCOMP);
                remaining_length = 2;
            }

            uint16_t getPacketId() const {
                return short_mqtt(packetId);
            }
        } __attribute__ ((__packed__));

    } // namespace mqtt
} // namespace qb

#endif //QB_MQTT_PUBLISH_H
