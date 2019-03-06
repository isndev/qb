//
// Created by isndev on 7/22/18.
//

#include "header.h"

#ifndef QB_MQTT_PING_H
#define QB_MQTT_PING_H

namespace qb {
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
} // namespace qb

#endif //QB_MQTT_PING_H
