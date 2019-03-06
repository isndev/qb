//
// Created by isndev on 7/22/18.
//

#include "header.h"

#ifndef QB_MQTT_CONNECT_H
#define QB_MQTT_CONNECT_H

namespace qb {
    namespace mqtt {

        enum ConnectStatus : uint8_t {
            ACCEPTED = 0,
            REJECTEDPROTOCOL,
            REJECTEDIDENTIFIER,
            REJECTEDUNAVAILABLESERVER,
            REJECTEDCREDENTIAL,
            REJECTEDNOTAUTHORIZED
        };

        class ConnectFlags {
        protected:
            uint8_t flags;
        public:
            ConnectFlags() = default;

            // read
            inline bool hasUsername() const { return flags & 128; }

            inline bool hasPassword() const { return flags & 64; }

            inline bool hasWillRetain() const { return flags & 32; }

            inline uint8_t getWillQos() const {
                const uint8_t copy = flags << 3;
                return copy >> 6;
            }

            inline bool hasWillWillFlag() const { return flags & 4; }

            inline bool hasCleanSession() const { return flags & 2; }

            inline uint8_t raw() const { return flags; }


            // write
            inline void setWillRetain() { flags |= 32; }

            inline void setWillQos(uint8_t qos) { flags |= qos << 3; }

            inline void setWillWillFlag() { flags |= 4; }

            inline void setCleanSession() { flags |= 2; }

        protected:
            inline void setUsername() { flags |= 128; }

            inline void setPassword() { flags |= 64; }
        } __attribute__ ((__packed__));

        // VERSION MQTT 3.1.11
        class Connect {
            constexpr static const char *const protocol_name = "MQTT";

            uint16_t size;
            char name[4];
            uint8_t level;
            ConnectFlags flags;
            uint16_t keep_alive;
            uint16_t client_id_size;
        public:
            Connect(uint16_t c_size, uint16_t keep_alive = 60)
                    : size(short_mqtt(4)), level(4), keep_alive(short_mqtt(keep_alive)),
                      client_id_size(short_mqtt(c_size)) {
                std::memcpy(name, protocol_name, 4);
            }

            bool isValid(uint32_t const msg_size) const {
                return msg_size > sizeof(Connect) &&
                       short_mqtt(size) == 4 &&
                       level == 4 &&
                       !strncmp(name, protocol_name, 4);
            };

            void setKeepAlive(uint16_t const toset) {
                keep_alive = short_mqtt(toset);
            }

            ConnectFlags &Flags() {
                return flags;
            }

            ConnectFlags getFlags() const { return flags; }

            uint16_t getKeepAlive() const { return short_mqtt(keep_alive); }

            int encodeClientId(std::string const &client_id) {
                const auto c_size = (std::min)(23ul, client_id.size());
                memcpy(reinterpret_cast<char *>(this) + sizeof(Connect), client_id.c_str(), c_size);
                return c_size;
            }

        } __attribute__ ((__packed__));

        class ConnAck
                : public FixedHeader {
            uint8_t flags;
            uint8_t code;

        public:
            ConnAck() : flags(0), code(0) {
                setType(MessageType::CONNACK);
                remaining_length = 2;
            }

            inline bool sessionPresent() const { return flags & 1; }

            inline uint8_t getCode() const { return code; }

            inline void setSessionPresent() {
                flags |= 1;
            }

            inline void setCode(ConnectStatus const status) {
                code = status;
            }
        } __attribute__ ((__packed__));

        class Disconnect
                : public FixedHeader {
        public:
            Disconnect() {
                setType(mqtt::MessageType::DISCONNECT);
            }
        };

    } // namespace mqtt
} // namespace qb

#endif //QB_MQTT_CONNECT_H
