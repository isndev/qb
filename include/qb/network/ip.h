/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#include            <iostream>
#include            <string>
#include            <qb/utility/build_macros.h>

#ifndef             QB_NETWORK_IP_H_
#define             QB_NETWORK_IP_H_

namespace           qb {
    namespace       network {

        /*!
         * @class ip ip.h qb/network/ip.h
         * @ingroup Network
         */
        class QB_API ip {
            uint32_t     _address;

            void resolve(const std::string& address);
            friend QB_API bool operator <(const ip& left, const ip& right);
        public:
            ip();
            ip(const std::string& address);
            ip(const char* address);
            ip(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3);
            explicit ip(uint32_t address);

            std::string toString() const;
            uint32_t toInteger() const;

            static const ip None;
            static const ip Any;
            static const ip LocalHost;


        };

        QB_API bool operator ==(const ip& left, const ip& right);
        QB_API bool operator !=(const ip& left, const ip& right);
        QB_API bool operator <(const ip& left, const ip& right);
        QB_API bool operator >(const ip& left, const ip& right);
        QB_API bool operator <=(const ip& left, const ip& right);
        QB_API bool operator >=(const ip& left, const ip& right);
        QB_API std::istream& operator >>(std::istream& stream, ip& address);
        QB_API std::ostream& operator <<(std::ostream& stream, const ip& address);

    } // namespace network
} // namespace qb

#endif // QB_NETWORK_IP_H_