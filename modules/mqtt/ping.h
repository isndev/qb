//
// Created by isndev on 7/22/18.
//

#include "header.h"

#ifndef CUBE_MQTT_PING_H
#define CUBE_MQTT_PING_H

namespace cube {
    namespace mqtt {

        class PingReq : public FixedHeader {
        public:
            PingReq() {
                setType(MessageType::PINGREQ);
            }
        } __attribute__ ((__packed__));

        class PingResp : public FixedHeader {
        public:
            PingResp() {
                setType(MessageType::PINQRESP);
            }
        } __attribute__ ((__packed__));

    } // namespace mqtt
} // namespace cube

#endif //CUBE_MQTT_PING_H
