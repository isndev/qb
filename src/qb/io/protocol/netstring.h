/**
 * @file qb/io/protocol/netstring.h
 * @brief Netstring framing: `[len]":"[payload]","`, the length in decimal (Huly QB-80).
 *
 * D. J. Bernstein's netstrings (1997): a payload of `len` bytes is sent as the decimal digits of `len`, a
 * colon, the bytes, a comma -- `5:hello,` -- so a reader knows how much to wait for before it has read a byte of
 * the payload, and the comma proves the length was right. No leading zero (`0` alone is the empty netstring,
 * `0:,`), and at most nine digits by default, the bound of Bernstein's reference implementation (999 999 999).
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

#ifndef QB_IO_PROT_NETSTRING_H_
#define QB_IO_PROT_NETSTRING_H_
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <qb/system/allocator/pipe.h>
#include "../config.h"
#include "../async/protocol.h"

namespace qb::protocol {

namespace detail {
/// 10^n, for the n <= digits10 a netstring's length may have: it fits a size_t (a constexpr function, inline).
[[nodiscard]] constexpr std::size_t
netstring_pow10(std::size_t n) noexcept {
    std::size_t p = 1u;
    while (n-- > 0u)
        p *= 10u;
    return p;
}
} // namespace detail

/**
 * @class netstring
 * @ingroup Protocol
 * @brief Netstring-framed messages, delivered as `message{size, data, text}` (Huly QB-80).
 *
 * Inspection only (`qb/readme/3_qb_io/protocols.md`, the three hooks): `getMessageSize()` consumes nothing and
 * reports the whole netstring -- digits, colon, payload, comma -- once the input holds it; the parsed header is
 * cached while the payload arrives. `0:,` is a frame of three bytes and is delivered empty. The protocol is marked
 * `not_ok()` on anything that is not a netstring: a byte other than a digit before the colon, a leading zero,
 * more than `_MaxDigits` digits, a colon with no digit before it, a length over `max_payload` -- each at the byte
 * that shows it, the colon not waited for -- a frame over the I/O component's `max_message_size()` (when it has
 * one; at the colon, before a byte of the payload is waited for), or a byte other than the comma where the comma
 * belongs (checked when the whole frame is there). The io itself refuses an oversized frame only once all of it
 * is buffered.
 *
 * @tparam _IO_         The I/O component (needs `in()`; its `max_message_size()` is read when it has one).
 * @tparam _StringTrait `std::string` (a copy) or `const std::string_view` (a view into the input buffer, valid
 *                      for the duration of the `on(...)` call), as `text::basic_text` offers.
 * @tparam _MaxDigits   The most digits a length may have (9: Bernstein's 999 999 999); at most
 *                      `std::numeric_limits<std::size_t>::digits10` (19 on a 64-bit `size_t`, 9 on a 32-bit one).
 * @tparam _MaxPayload  The largest payload accepted, which `_MaxDigits` caps further: `max_payload` is the smaller
 *                      of the two, the bound the decoder and `Header()` share. The default leaves the header and the
 *                      comma room under `QB_MAX_MESSAGE_SIZE`.
 */
template <typename _IO_, typename _StringTrait = std::string, std::size_t _MaxDigits = 9,
          std::size_t _MaxPayload = (QB_MAX_MESSAGE_SIZE > _MaxDigits + 2u ? QB_MAX_MESSAGE_SIZE - _MaxDigits - 2u : 0u)>
class netstring : public io::async::AProtocol<_IO_> {
    // The length is accumulated in a size_t: more digits than it holds without wrapping (9 on 32-bit, 19 on
    // 64-bit) would let a peer wrap the frame size; so would a payload bound with no room for the header and
    // the comma. Both are refused at compile time.
    static_assert(_MaxDigits >= 1 && _MaxDigits <= static_cast<std::size_t>(std::numeric_limits<std::size_t>::digits10),
                  "a netstring length must fit a size_t without wrapping");
    static_assert(_MaxPayload <= std::numeric_limits<std::size_t>::max() - _MaxDigits - 2u,
                  "the payload bound leaves no room for the frame's header and comma in a size_t");
    std::size_t _header       = 0u; /**< digits + colon of the frame being assembled, once parsed; 0 before */
    std::size_t _payload      = 0u; /**< its payload length, once parsed */
    std::size_t _frame_header = 0u; /**< the header bytes of the frame last reported, for `onMessage()` */

public:
    static constexpr char        end        = ',';        /**< The byte that ends every netstring */
    static constexpr char        colon      = ':';        /**< The byte that ends the length */
    static constexpr std::size_t max_digits = _MaxDigits; /**< Digits a length may have */
    /// Payload bytes a frame may carry: `_MaxPayload`, or the largest length `_MaxDigits` digits write when that is
    /// smaller -- the one bound the decoder and `Header()` share, so `Header(n)` writes only what the decoder takes.
    static constexpr std::size_t max_payload =
        _MaxPayload < detail::netstring_pow10(_MaxDigits) - 1u ? _MaxPayload : detail::netstring_pow10(_MaxDigits) - 1u;

    /**
     * @struct message
     * @brief One netstring: its payload, as bytes and as `_StringTrait`.
     */
    struct message {
        const std::size_t size; /**< Payload size */
        const char       *data; /**< Pointer to the payload in the input buffer */
        _StringTrait      text; /**< The payload as text */
    };

    /**
     * @struct header
     * @brief The prefix of an outgoing netstring: the decimal length and the colon, ready to write.
     */
    struct header {
        std::array<char, 21> bytes{}; /**< 20 digits of a 64-bit length, the colon */
        std::size_t          size{0};

        [[nodiscard]] std::string_view
        view() const noexcept {
            return {bytes.data(), size};
        }
        [[nodiscard]] const char *
        data() const noexcept {
            return bytes.data();
        }
    };

    netstring()          = delete;
    virtual ~netstring() = default;

    /**
     * @brief Constructor with I/O reference
     * @param io Reference to the I/O object that uses this protocol
     */
    netstring(_IO_ &io) noexcept
        : io::async::AProtocol<_IO_>(io) {}

    /// The header bytes (digits + colon) of the frame `getMessageSize()` last reported: for `onMessage()`, which
    /// runs right after that report; it is the previous frame's between a report and the next.
    [[nodiscard]] std::size_t
    header_size() const noexcept {
        return _frame_header;
    }

    /// The payload length of the frame last reported (without its header and its comma); same validity as `header_size()`.
    [[nodiscard]] std::size_t
    shiftSize(std::size_t const frame) const noexcept {
        return frame > _frame_header ? frame - _frame_header - 1u : 0u;
    }

    /**
     * @brief The size of the next complete netstring, header and comma included, or 0.
     */
    std::size_t
    getMessageSize() noexcept final {
        const auto &buffer = this->_io.in();
        const auto  size   = buffer.size();
        const char *begin  = buffer.cbegin();
        if (!_header) {
            std::size_t digits = 0u;
            std::size_t len    = 0u;
            for (; digits < size; ++digits) {
                const char c = begin[digits];
                if (c == colon)
                    break;
                if (c < '0' || c > '9') { // not a netstring
                    this->not_ok();
                    return 0;
                }
                if (digits == 1u && begin[0] == '0') { // a leading zero: only `0` itself is written with one
                    this->not_ok();
                    return 0;
                }
                // A length longer than the format allows. Tested BEFORE the digit is added: at _MaxDigits ==
                // digits10 one more digit could wrap `len` past SIZE_MAX back under the bound.
                if (digits == _MaxDigits) {
                    this->not_ok();
                    return 0;
                }
                len = len * 10u + static_cast<std::size_t>(c - '0');
                if (len > max_payload) { // `len` only grows: refused at this digit, the colon not waited for
                    this->not_ok();
                    return 0;
                }
            }
            if (digits == size)
                return 0;       // the colon is not here yet
            if (digits == 0u) { // a colon with no length before it
                this->not_ok();
                return 0;
            }
            // The io refuses a frame over its cap once the frame is REPORTED, all of it buffered: refuse it here,
            // at the colon, against the cap the component runs with (no wrap: the static assertions bound it).
            if constexpr (requires(_IO_ const &io) { io.max_message_size(); }) {
                if (digits + 1u + len + 1u > this->_io.max_message_size()) {
                    this->not_ok();
                    return 0;
                }
            }
            _header  = digits + 1u;
            _payload = len;
        }
        const auto frame = _header + _payload + 1u;
        if (size < frame)
            return 0;
        if (begin[frame - 1u] != end) { // the length lied, or this is not a netstring
            this->not_ok();
            return 0;
        }
        _frame_header = _header;
        _header = _payload = 0u;
        return frame;
    }

    /**
     * @brief Process a received netstring
     * @param size Frame size, header and comma included
     */
    void
    onMessage(std::size_t size) noexcept final {
        const char *data = this->_io.in().cbegin() + _frame_header;
        const auto  n    = shiftSize(size);
        this->_io.on(message{n, data, {data, n}});
    }

    /**
     * @brief Resets the protocol state
     */
    void
    reset() noexcept final {
        _header = _payload = _frame_header = 0u;
    }

    /**
     * @brief The prefix of an outgoing payload of `payload` bytes: `*io << Header(n).view() << bytes << end`.
     * @throws std::length_error when `payload` exceeds `max_payload` -- the decoder's own bound, `_MaxDigits` included.
     */
    [[nodiscard]] static header
    Header(std::size_t payload) {
        if (payload > max_payload)
            throw std::length_error("qb::protocol::netstring: payload size exceeds the protocol's bound");
        header h;
        const auto [ptr, ec] = std::to_chars(h.bytes.data(), h.bytes.data() + h.bytes.size() - 1u, payload);
        (void) ec; // 20 digits always fit a 64-bit length
        *ptr   = colon;
        h.size = static_cast<std::size_t>(ptr + 1 - h.bytes.data());
        return h;
    }

    /**
     * @brief Write a whole netstring -- prefix, payload, comma -- into `out`.
     */
    static void
    put(qb::allocator::pipe<char> &out, std::string_view payload) {
        out << Header(payload.size()).view() << payload << end;
    }
};

/**
 * @typedef netstring_view
 * @ingroup Protocol
 * @brief Netstrings whose `message::text` is a zero-copy `std::string_view` into the input buffer.
 */
template <typename _IO_, std::size_t _MaxDigits = 9,
          std::size_t _MaxPayload = (QB_MAX_MESSAGE_SIZE > _MaxDigits + 2u ? QB_MAX_MESSAGE_SIZE - _MaxDigits - 2u : 0u)>
using netstring_view = netstring<_IO_, const std::string_view, _MaxDigits, _MaxPayload>;

} // namespace qb::protocol

#endif // QB_IO_PROT_NETSTRING_H_
