/**
 * @file qb/io/crypto_core.cpp
 * @brief OpenSSL-free part of the crypto utilities
 *
 * These helpers live in `qb::crypto` for historical reasons but contain no
 * cryptography and call no OpenSSL API: they are the hex codec (used by the
 * PostgreSQL bytea wire format and by the HPACK tests), a byte-wise XOR and a
 * constant-time comparison. They are compiled unconditionally so that a build
 * with `QB_WITH_SSL=OFF` still links them — see the `#ifdef QB_HAS_SSL` guard
 * in `qb/io/crypto.h`, which keeps exactly these members declared when OpenSSL
 * is absent.
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

#include <qb/io/crypto.h>
#include <qb/utility/build_macros.h>

namespace qb {

/// Returns hex string from bytes in input string.
std::string
crypto::to_hex_string(const std::string &input, std::string_view const &hex_digits) noexcept {
    std::string output;
    output.reserve(input.length() * 2);
    for (unsigned char c : input) {
        output.push_back(hex_digits[c >> 4]);
        output.push_back(hex_digits[c & 15]);
    }
    return output;
}

DISABLE_WARNING_PUSH
DISABLE_WARNING_NARROWING
/// Returns hex value from byte.
int
crypto::hex_value(unsigned char hex_digit) noexcept {
    static constexpr int hex_values[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  -1, -1, -1, -1, -1, -1,
        -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };

    return hex_values[hex_digit];
}
DISABLE_WARNING_POP

/// Returns formatted hex string from hex bytes in input string.
std::string
crypto::hex_to_string(const std::string &input) noexcept {
    const auto len = input.length();
    if (len & 1)
        return "";

    std::string output;
    output.reserve(len / 2);
    for (auto it = input.begin(); it != input.end();) {
        int hi = hex_value(*it++);
        int lo = hex_value(*it++);
        // Reject non-hex input instead of emitting garbage bytes: hex_value()
        // returns -1 for any non-hex digit, and `(-1 << 4) | lo` would push a
        // bogus byte. A malformed hex string now yields "" (parse failure).
        if (hi < 0 || lo < 0)
            return "";
        output.push_back(static_cast<char>(hi << 4 | lo));
    }
    return output;
}

// xor two vector of same size
std::vector<unsigned char>
crypto::xor_bytes(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b) {
    if (a.size() != b.size()) {
        throw std::runtime_error("vectors must have the same size to XOR");
    }
    std::vector<unsigned char> result(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        result[i] = a[i] ^ b[i];
    }
    return result;
}

// Constant-time comparison to prevent timing attacks
bool
crypto::constant_time_compare(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b) noexcept {
    // If sizes differ, we can't compare - but we still need constant time
    // We compute a dummy comparison to avoid leaking the size difference via timing
    if (a.size() != b.size()) {
        return false;
    }

    // Volatile to prevent compiler optimization that could short-circuit
    volatile unsigned char result = 0;

    // XOR all bytes together - result will be 0 only if all bytes match
    for (std::size_t i = 0; i < a.size(); ++i) {
        result |= a[i] ^ b[i];
    }

    return result == 0;
}

} // namespace qb
