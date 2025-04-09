/**
 * @file json.h
 * @brief JSON protocol implementation
 *
 * This file contains the implementation of the JSON protocol for the IO system.
 * It provides a protocol for sending and receiving JSON messages.
 */

#ifndef QB_IO_PROTOCOL_JSON_H
#define QB_IO_PROTOCOL_JSON_H
#include "base.h"
#include "nlohmann/json.hpp"

namespace qb {
namespace protocol {

template <typename IO_>
class json : public base::byte_terminated<IO_, '\0'> {
public:
    json() = delete;
    explicit json(IO_ &io) noexcept
        : base::byte_terminated<IO_, '\0'>(io) {}

    struct message {
        const std::size_t size;
        const char       *data;
        nlohmann::json    json;
    };

    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data   = this->_io.in().cbegin();

        this->_io.on(message{
            parsed, data,
            nlohmann::json::parse(std::string_view(data, parsed), nullptr, false)});
    }
};

template <typename IO_>
class json_packed : public base::byte_terminated<IO_, '\0'> {
public:
    json_packed() = delete;
    explicit json_packed(IO_ &io) noexcept
        : base::byte_terminated<IO_, '\0'>(io) {}

    struct message {
        const std::size_t size;
        const char       *data;
        nlohmann::json    json;
    };

    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data   = this->_io.in().cbegin();
        this->_io.on(message{
            parsed, data, nlohmann::json::from_msgpack(std::string_view(data, parsed))});
    }
};

} // namespace protocol
} // namespace qb
#endif // QB_IO_PROTOCOL_JSON_H