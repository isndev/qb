//
// Created by isndev on 7/22/18.
//

#ifndef QB_MQTT_HEADER_H
#define QB_MQTT_HEADER_H

namespace qb {
    namespace mqtt {

        enum MessageType : uint8_t {
            BEGIN = 0,
            CONNECT,
            CONNACK,
            PUBLISH,
            PUBACK,
            PUBREC,
            PUBREL,
            PUBCOMP,
            SUBSCRIBE,
            SUBACK,
            UNSUBSCRIBE,
            UNSUBACK,
            PINGREQ,
            PINQRESP,
            DISCONNECT,
            END
        };

        uint16_t short_mqtt(uint16_t item) {
            auto data = reinterpret_cast<uint8_t *>(&item);
            std::swap(data[0], data[1]);
            return item;
        }

        class Control {
        protected:
            uint8_t control = 0;
        public:
            Control() = default;

            inline uint8_t getType() const { return control >> 4; }

            inline uint8_t getQos() const {
                const uint8_t copy = control << 5;
                return copy >> 6;
            }

            inline bool isDup() const { return control & 4; }

            inline bool isRetain() const { return control & 1; }

            inline void setType(uint8_t type) { control |= (type << 4); }

            inline void setQos(uint8_t qos) { control = (control & ~(3 << 1)) | (qos << 1); }

            inline void setDup() { control |= (1 << 3); }

            inline void setRetain() { control |= 1; }
        } __attribute__ ((__packed__));

        struct FixedHeader : public Control {
            uint8_t remaining_length;

            FixedHeader() { reset(); }

            FixedHeader(MessageType const type) {
                setType(type);
            }

            void reset() {
                control = 0;
                remaining_length = 0;
            }

            int encodeSize(uint32_t size) {
                uint8_t *bytes = &remaining_length;
                uint8_t byte = 0;
                int i = 0;

                do {
                    byte = size % 128;
                    size /= 128;
                    if (size > 0)
                        byte |= 128;
                    bytes[i++] = byte;
                } while (size > 0);
                return i;
            }

            int decodeSize(std::size_t max, uint32_t &size) const {
                uint8_t const *bytes = &remaining_length;
                int multiplier = 1;
                int i = 0;
                size = 0;

                do {
                    if (i >= max)
                        return i;
                    size += (bytes[i] & 127) * multiplier;
                    multiplier *= 128;
                } while (bytes[i++] & 128);
                return i;
            }
        } __attribute__ ((__packed__));

        inline uint16_t decodePacketId(const void *buffer, std::size_t const size) {
            return short_mqtt(*reinterpret_cast<const uint16_t *>(reinterpret_cast<const char *>(buffer) + size));
        }

    } // namespace mqtt
} // namespace qb

#endif //QB_MQTT_HEADER_H
