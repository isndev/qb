/**
 * @file qb/io/protocol/varint.h
 * @brief Length-prefixed framing on a base-128 varint -- protobuf's and MQTT's prefix -- and the varint codec.
 *
 * Two things live here (Huly QB-80). The codec, `qb::protocol::varint`: `decode` reads one base-128 varint
 * (seven bits a byte, least significant group first, the high bit of a byte saying another follows) under
 * a POLICY that bounds it, `encode` writes one, and the two policies are the two wire formats that use the
 * scheme: protobuf's (ten bytes at most, any 64-bit value, an over-long encoding accepted) and MQTT's
 * Variable Byte Integer (four bytes at most, 268 435 455 at most, the minimal encoding required). The framer,
 * `qb::protocol::varint_prefixed`, frames a stream of `[lead bytes][varint length][payload]` messages on it,
 * and `varint_binary` delivers each as a `message`. A fixed number of lead bytes before the varint -- one for
 * an MQTT control packet, its type and flags -- belongs to the frame, so the whole packet reaches the handler.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup IO
 */

#ifndef QB_IO_PROT_VARINT_H_
#define QB_IO_PROT_VARINT_H_
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <qb/system/allocator/pipe.h>
#include "../config.h"
#include "../async/protocol.h"

namespace qb::protocol::varint {

/// The most bytes a base-128 varint of a 64-bit value takes: nine groups of seven bits and the top bit alone.
inline constexpr std::size_t max_encoded_bytes = 10;

/**
 * @struct protobuf_policy
 * @ingroup Protocol
 * @brief Protobuf's varint: up to ten bytes, any 64-bit value, and an over-long encoding (trailing zero groups,
 *        such as `0x80 0x00` for zero) accepted, as protobuf's parsers accept it. One point is stricter than
 *        protobuf's C++ parser, which drops the high bits of a tenth byte: here a tenth group above 1 -- a value
 *        past 64 bits -- is an `overflow`, the rule of Go's `binary.Uvarint`.
 */
struct protobuf_policy {
    static constexpr std::size_t   max_bytes = max_encoded_bytes;
    static constexpr std::uint64_t max_value = std::numeric_limits<std::uint64_t>::max();
    static constexpr bool          canonical = false;
};

/**
 * @struct mqtt_policy
 * @ingroup Protocol
 * @brief MQTT's Variable Byte Integer: at most four bytes and at most 268 435 455 (MQTT 3.1.1 §2.2.3, 5.0
 *        §1.5.5), and the minimal encoding required (MQTT 5.0 §1.5.5, [MQTT-1.5.5-1]) -- a multi-byte encoding
 *        whose last group is zero is a protocol error. MQTT 3.1.1 states no such rule normatively: a 3.1.1 peer
 *        that pads its length is refused here, and a policy `{4, 268'435'455, false}` (`max_bytes`, `max_value`,
 *        `canonical`) is the lenient form.
 */
struct mqtt_policy {
    static constexpr std::size_t   max_bytes = 4;
    static constexpr std::uint64_t max_value = 268'435'455u;
    static constexpr bool          canonical = true;
};

/// The default payload bound of a framer with `LeadBytes` lead bytes under `Policy`: the largest payload whose whole
/// frame -- the lead bytes, the longest varint the policy allows, the payload -- stays within the io's default message
/// cap `QB_MAX_MESSAGE_SIZE`, or the policy's maximum when that is smaller (an MQTT framer never defaults past
/// 268 435 455, whatever cap a build chose). A frame the framer accepts is then one the io accepts too (Huly QB-80).
template <typename Policy, std::size_t LeadBytes = 0>
[[nodiscard]] constexpr std::size_t
default_max_payload() noexcept {
    constexpr std::size_t cap  = QB_MAX_MESSAGE_SIZE;
    constexpr std::size_t room = (cap > LeadBytes && cap - LeadBytes > Policy::max_bytes) ? cap - LeadBytes - Policy::max_bytes : 0u;
    return static_cast<std::size_t>(static_cast<std::uint64_t>(room) < Policy::max_value ? static_cast<std::uint64_t>(room)
                                                                                         : Policy::max_value);
}

/// What `decode` found.
enum class decode_status : std::uint8_t {
    complete,      ///< a whole varint: `value` and `consumed` are its
    incomplete,    ///< the bytes end inside it: wait for more (`consumed` = the bytes read so far)
    overlong,      ///< `Policy::max_bytes` bytes all announce another: not a varint of this policy
    overflow,      ///< past 64 bits (a tenth group over 1) or past `Policy::max_value`
    non_canonical, ///< a trailing zero group where the policy requires the minimal encoding
};

struct decode_result {
    decode_status status;
    std::uint64_t value;    ///< meaningful when `status == complete`
    std::size_t   consumed; ///< the bytes of the varint when complete; the bytes examined otherwise
};

/// The bytes `encode(value)` writes.
[[nodiscard]] constexpr std::size_t
encoded_size(std::uint64_t value) noexcept {
    std::size_t n = 1;
    while (value >= 0x80u) {
        value >>= 7;
        ++n;
    }
    return n;
}

/**
 * @brief Decode one base-128 varint from `[begin, end)` under `Policy`. Reads at most `Policy::max_bytes`
 *        bytes, never past `end`, allocates nothing, never throws.
 * @details The tenth byte of a ten-byte policy has two verdicts: a group over 1 -- a value past 64 bits -- is an
 *          `overflow` at once, whether or not the byte announces another; a group of 0 or 1 that announces another
 *          is `overlong`, the eleventh byte never read. Both refuse; only `decode_result::status` tells them apart
 *          (Go's `binary.Uvarint` calls every continuing tenth byte an overflow).
 */
template <typename Policy = protobuf_policy>
[[nodiscard]] constexpr decode_result
decode(const char *begin, const char *end) noexcept {
    static_assert(Policy::max_bytes >= 1 && Policy::max_bytes <= max_encoded_bytes, "a varint takes 1 to 10 bytes");
    // A policy whose maximum its byte count cannot spell would refuse, as over-long, what encode_header() writes.
    static_assert(encoded_size(Policy::max_value) <= Policy::max_bytes, "the policy's maximum must fit its byte count");
    std::uint64_t value = 0;
    std::size_t   i     = 0;
    for (; i < Policy::max_bytes; ++i) {
        if (begin + i == end)
            return {decode_status::incomplete, 0, i};
        const auto          byte  = static_cast<std::uint8_t>(begin[i]);
        const std::uint64_t group = byte & 0x7Fu;
        if (i == max_encoded_bytes - 1 && group > 1u)
            return {decode_status::overflow, 0, i + 1}; // the tenth group holds the top bit alone
        value |= group << (7 * i);
        if (!(byte & 0x80u)) {
            if constexpr (Policy::canonical) {
                if (i > 0 && group == 0)
                    return {decode_status::non_canonical, value, i + 1};
            }
            if (value > Policy::max_value)
                return {decode_status::overflow, value, i + 1};
            return {decode_status::complete, value, i + 1};
        }
    }
    return {decode_status::overlong, 0, i};
}

/**
 * @brief Encode `value` as a minimal base-128 varint into `out`, which holds at least `encoded_size(value)`
 *        bytes (`max_encoded_bytes` always suffices).
 * @return The bytes written.
 */
constexpr std::size_t
encode(std::uint64_t value, char *out) noexcept {
    std::size_t n = 0;
    while (value >= 0x80u) {
        out[n++] = static_cast<char>(static_cast<std::uint8_t>(value & 0x7Fu) | 0x80u);
        value >>= 7;
    }
    out[n++] = static_cast<char>(static_cast<std::uint8_t>(value));
    return n;
}

/**
 * @struct header
 * @ingroup Protocol
 * @brief An encoded varint, ready to write before a payload: `*io << h.view() << payload`.
 */
struct header {
    std::array<char, max_encoded_bytes> bytes{};
    std::size_t                         size{0};

    [[nodiscard]] std::string_view
    view() const noexcept {
        return {bytes.data(), size};
    }
    [[nodiscard]] const char *
    data() const noexcept {
        return bytes.data();
    }
};

/**
 * @brief The minimal encoding of `value` under `Policy`.
 * @throws std::length_error when `value` exceeds `Policy::max_value` (MQTT's 268 435 455, say).
 */
template <typename Policy = protobuf_policy>
[[nodiscard]] constexpr header
encode_header(std::uint64_t value) {
    static_assert(encoded_size(Policy::max_value) <= Policy::max_bytes, "the policy's maximum must fit its byte count");
    if (value > Policy::max_value)
        throw std::length_error("qb::protocol::varint: value exceeds the policy's maximum");
    header h;
    h.size = encode(value, h.bytes.data());
    return h;
}

} // namespace qb::protocol::varint

namespace qb::protocol {

/**
 * @class varint_prefixed
 * @ingroup Protocol
 * @brief Framing of `[lead bytes][varint length][payload]` messages: protobuf's length-delimited stream, MQTT's
 *        control packets with `_LeadBytes = 1` (Huly QB-80).
 *
 * Inspection only, the rule of `qb/readme/3_qb_io/protocols.md` for a new protocol: `getMessageSize()` consumes
 * nothing and calls nothing back; it reports the WHOLE frame -- lead bytes, varint and payload -- once the input
 * holds it, so an empty payload is a frame of at least one byte and is delivered (MQTT's `PINGREQ`, an empty
 * protobuf message), where `size_as_header` refuses a zero. The decoded prefix is cached while the payload
 * arrives, so a payload that comes byte by byte costs one decode. The protocol is marked `not_ok()` on an
 * over-long, overflowing or (under a canonical policy) non-minimal varint -- the stream is not this protocol's --
 * and, as soon as the prefix is whole and before a byte of the payload is waited for, on a payload over
 * `_MaxPayload` and on a frame over the I/O component's `max_message_size()` (the cap it runs with: the
 * `QB_MAX_MESSAGE_SIZE` default or what `set_max_message_size()` set), when the component has one. The io itself
 * refuses an oversized frame only once all of it is buffered.
 *
 * @tparam _IO_        The I/O component (needs `in()`; its `max_message_size()` is read when it has one).
 * @tparam _Policy     `varint::protobuf_policy` (the default) or `varint::mqtt_policy`.
 * @tparam _LeadBytes  Fixed bytes before the varint, part of the frame (1 for MQTT: the packet type and flags).
 * @tparam _MaxPayload The largest payload accepted; a longer one is a protocol error, refused before its bytes
 *                     are waited for. The default, `varint::default_max_payload<_Policy, _LeadBytes>()`, leaves the
 *                     header room under `QB_MAX_MESSAGE_SIZE`.
 */
template <typename _IO_, typename _Policy = varint::protobuf_policy, std::size_t _LeadBytes = 0,
          std::size_t _MaxPayload = varint::default_max_payload<_Policy, _LeadBytes>()>
class varint_prefixed : public io::async::AProtocol<_IO_> {
    std::size_t _header       = 0u; /**< lead + varint bytes of the frame being assembled, once decoded; 0 before */
    std::size_t _payload      = 0u; /**< its payload length, once decoded */
    std::size_t _frame_header = 0u; /**< the header bytes of the frame last reported, for `onMessage()` */

public:
    using policy                             = _Policy;
    static constexpr std::size_t lead_bytes  = _LeadBytes;
    static constexpr std::size_t max_payload = _MaxPayload;
    static_assert(_MaxPayload <= _Policy::max_value, "the payload bound exceeds what the policy's varint can say");
    // The frame size is header + payload in a size_t: a bound near SIZE_MAX would let a peer wrap it to a small
    // number, pass the io's cap, and hand the handler a payload of SIZE_MAX bytes. Refused at compile time, in an
    // order that cannot wrap itself (an absurd _LeadBytes first).
    static_assert(_LeadBytes <= std::numeric_limits<std::size_t>::max() - varint::max_encoded_bytes
                      && _MaxPayload <= std::numeric_limits<std::size_t>::max() - varint::max_encoded_bytes - _LeadBytes,
                  "the payload bound leaves no room for the frame's header in a size_t");

    varint_prefixed()          = delete;
    virtual ~varint_prefixed() = default;

    /**
     * @brief Constructor with I/O reference
     * @param io Reference to the I/O object that uses this protocol
     */
    varint_prefixed(_IO_ &io) noexcept
        : io::async::AProtocol<_IO_>(io) {}

    /// The header bytes (lead + varint) of the frame `getMessageSize()` last reported: for `onMessage()`, which
    /// runs right after that report; it is the previous frame's between a report and the next.
    [[nodiscard]] std::size_t
    header_size() const noexcept {
        return _frame_header;
    }

    /// The payload length of the frame last reported (the frame without its header); same validity as `header_size()`.
    [[nodiscard]] std::size_t
    shiftSize(std::size_t const frame) const noexcept {
        return frame >= _frame_header ? frame - _frame_header : 0;
    }

    /**
     * @brief The size of the next complete frame, header included, or 0.
     */
    std::size_t
    getMessageSize() noexcept final {
        const auto &buffer = this->_io.in();
        const auto  size   = buffer.size();
        if (!_header) {
            if (size <= _LeadBytes)
                return 0; // not a byte of the varint yet
            const char *begin = buffer.cbegin();
            const auto  r     = varint::decode<_Policy>(begin + _LeadBytes, begin + size);
            switch (r.status) {
                case varint::decode_status::incomplete:
                    return 0;
                case varint::decode_status::complete:
                    break;
                default:
                    this->not_ok(); // over-long, overflowing or non-minimal: not a varint of this policy
                    return 0;
            }
            if (r.value > _MaxPayload) {
                this->not_ok();
                return 0;
            }
            const std::size_t header  = _LeadBytes + r.consumed;
            const std::size_t payload = static_cast<std::size_t>(r.value);
            // The io refuses a frame over its cap once the frame is REPORTED, all of it buffered: refuse it here, at
            // the prefix, against the cap the component runs with (no wrap: both terms are bounded above).
            if constexpr (requires(_IO_ const &io) { io.max_message_size(); }) {
                if (header + payload > this->_io.max_message_size()) {
                    this->not_ok();
                    return 0;
                }
            }
            _header  = header;
            _payload = payload;
        }
        if (size >= _header + _payload) {
            _frame_header    = _header;
            const auto frame = _header + _payload;
            _header = _payload = 0u;
            return frame;
        }
        return 0;
    }

    /**
     * @brief Resets the protocol state
     */
    void
    reset() noexcept final {
        _header = _payload = _frame_header = 0u;
    }

    /**
     * @brief The varint prefix of an outgoing payload of `payload` bytes: `*io << lead << Header(n).view() << bytes`.
     * @throws std::length_error when `payload` exceeds `_MaxPayload` or the policy's maximum.
     */
    [[nodiscard]] static varint::header
    Header(std::size_t payload) {
        if (payload > _MaxPayload)
            throw std::length_error("qb::protocol::varint_prefixed: payload size exceeds the protocol's bound");
        return varint::encode_header<_Policy>(payload);
    }
};

/**
 * @class varint_binary
 * @ingroup Protocol
 * @brief `varint_prefixed` that delivers each frame as a `message` to the I/O component's `on(message&&)`.
 *
 * `message::data` / `size` are the payload; `frame` / `header_size` the whole frame and its header, for a
 * protocol whose lead bytes carry meaning (an MQTT packet's type and flags are `frame[0]`). All of it points
 * into the input buffer and is valid for the duration of the `on(...)` call.
 */
template <typename _IO_, typename _Policy = varint::protobuf_policy, std::size_t _LeadBytes = 0,
          std::size_t _MaxPayload = varint::default_max_payload<_Policy, _LeadBytes>()>
class varint_binary : public varint_prefixed<_IO_, _Policy, _LeadBytes, _MaxPayload> {
public:
    using varint_prefixed<_IO_, _Policy, _LeadBytes, _MaxPayload>::varint_prefixed;

    /**
     * @struct message
     * @brief One frame: its payload, and the frame it came in.
     */
    struct message {
        const std::size_t size;        /**< Payload size */
        const char       *data;        /**< Pointer to the payload */
        const char       *frame;       /**< Pointer to the frame's first byte (the lead bytes, then the varint) */
        const std::size_t header_size; /**< Lead bytes + varint bytes */
    };

    /**
     * @brief Process a received frame
     * @param size Frame size, header included
     */
    void
    onMessage(std::size_t size) noexcept final {
        const char *frame  = this->_io.in().cbegin();
        const auto  header = this->header_size();
        this->_io.on(message{this->shiftSize(size), frame + header, frame, header});
    }
};

} // namespace qb::protocol

#endif // QB_IO_PROT_VARINT_H_
