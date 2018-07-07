#include            <iostream>
#include            <string>
#include            <utils/build_macros.h>

#ifndef             CUBE_NETWORK_IPv4_H_
#define             CUBE_NETWORK_IPv4_H_

namespace           cube {
    namespace       network {

        class CUBE_API ip {
            uint32_t     _address;

            void resolve(const std::string& address);
            friend CUBE_API bool operator <(const ip& left, const ip& right);
        public:
            ip();
            ip(const std::string& address);
            ip(const char* address);
            ip(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3);
            explicit ip(uint32_t address);

            std::string toString() const;
            uint32_t toInteger() const;

            //static ip getLocalAddress();
            //static ip getPublicAddress();

            static const ip None;
            static const ip Any;
            static const ip LocalHost;


        };

        CUBE_API bool operator ==(const ip& left, const ip& right);
        CUBE_API bool operator !=(const ip& left, const ip& right);
        CUBE_API bool operator <(const ip& left, const ip& right);
        CUBE_API bool operator >(const ip& left, const ip& right);
        CUBE_API bool operator <=(const ip& left, const ip& right);
        CUBE_API bool operator >=(const ip& left, const ip& right);
        CUBE_API std::istream& operator >>(std::istream& stream, ip& address);
        CUBE_API std::ostream& operator <<(std::ostream& stream, const ip& address);

    } //namespace network
} //namespace cube

#endif