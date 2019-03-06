//
// Created by isndev on 7/14/18.
//

#include "header.h"

#ifndef QB_MQTT_READER_H
#define QB_MQTT_READER_H

namespace qb {
    namespace mqtt {

        constexpr std::size_t MAX_MESSAGE_SIZE = 65520;

        class Reader {
            FixedHeader const *_header = nullptr;
            uint32_t read_offset = 0;
            uint32_t payload_offset = 0;
            uint32_t remaining_bytes = 2;

        public:
            Reader() = default;

            inline void setHeader(void *buffer) {
                _header = reinterpret_cast<FixedHeader const *>(buffer);
            }

            inline FixedHeader const &header() const {
                return *_header;
            }

            template<typename T>
            constexpr inline const T *getMessage() const {
                if constexpr (std::is_base_of<mqtt::FixedHeader, T>::value)
                    return reinterpret_cast<const T *> (_header);
                else
                    return reinterpret_cast<const T *> (reinterpret_cast<const uint8_t *>(_header) + payload_offset);
            }

            uint32_t getMessageSize() const { return remaining_bytes - payload_offset; }

            inline uint32_t expected() const {
                return remaining_bytes - read_offset;
            }

            inline uint32_t remaining() const {
                return remaining_bytes;
            }

            inline uint32_t getOffset() const {
                return payload_offset;
            }

            inline uint32_t readBytes() const {
                return read_offset;
            }

            inline bool isComplete() const {
                return read_offset == remaining_bytes;
            }

            inline void read(std::size_t const received) {
                read_offset += received;
                if (remaining_bytes <= 2) {
                    if (read_offset >= 2) {
                        if (_header->remaining_length & 128) {
                            remaining_bytes += (_header->remaining_length & 127) + 128;
                        } else {
                            remaining_bytes += _header->remaining_length & 127;
                            payload_offset = 2;
                        }
                    }
                } else if (isComplete()) {
                    payload_offset = _header->decodeSize(read_offset - 1, remaining_bytes) + 1;
                    remaining_bytes += payload_offset;
                    if (remaining_bytes > MAX_MESSAGE_SIZE) {
                        const_cast<FixedHeader *>(_header)->setType(mqtt::DISCONNECT);
                        read_offset = remaining_bytes = 2;
                    }
                }
            }

            inline void reset() {
                read_offset = 0;
                payload_offset = 0;
                remaining_bytes = sizeof(FixedHeader);
            }

        };

    } // namespace mqtt
} // namespace qb

#endif //QB_MQTT_READER_H
