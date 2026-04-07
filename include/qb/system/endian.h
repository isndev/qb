/**
 * @file qb/system/endian.h
 * @brief Endianness detection and byte swapping utilities
 *
 * This file provides utilities for detecting the system's native endianness
 * and converting values between different byte orders (little endian and big endian).
 * It leverages C++20 std::endian and C++23 std::byteswap when available, with
 * portable fallbacks for other compilers.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * @ingroup System
 */

#ifndef QB_SYSTEM_ENDIAN_H
#define QB_SYSTEM_ENDIAN_H
#include <bit>
#include <cstdint>
#include <type_traits>

namespace qb::endian {

/**
 * @brief Enumeration of byte order types (maps to std::endian)
 */
enum class order {
    little  = static_cast<int>(std::endian::little),
    big     = static_cast<int>(std::endian::big),
    native  = static_cast<int>(std::endian::native),
    unknown = -1
};

/**
 * @brief Determines the system's native byte order
 * @return The native byte order of the system
 */
[[nodiscard]] consteval order
native_order() noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return order::little;
    else if constexpr (std::endian::native == std::endian::big)
        return order::big;
    else
        return order::unknown;
}

/**
 * @brief Checks if the system is little-endian
 * @return true if system is little-endian, false otherwise
 */
[[nodiscard]] consteval bool
is_little_endian() noexcept {
    return std::endian::native == std::endian::little;
}

/**
 * @brief Checks if the system is big-endian
 * @return true if system is big-endian, false otherwise
 */
[[nodiscard]] consteval bool
is_big_endian() noexcept {
    return std::endian::native == std::endian::big;
}

/**
 * @brief Swaps the byte order of a value
 *
 * Uses C++23 std::byteswap for integral types.
 * Falls back to a manual reversal for enum types.
 *
 * @tparam T The type of value to byte-swap (must be arithmetic or enum and trivially copyable)
 * @param value The value to byte-swap
 * @return The byte-swapped value
 */
template <typename T>
[[nodiscard]] constexpr T
byteswap(T value) noexcept {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>,
                  "byteswap only supports arithmetic or enum types");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    if constexpr (std::is_integral_v<T>) {
        return std::byteswap(value);
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        return static_cast<T>(std::byteswap(static_cast<U>(value)));
    } else {
        T              result;
        const uint8_t *src = reinterpret_cast<const uint8_t *>(&value);
        uint8_t       *dst = reinterpret_cast<uint8_t *>(&result);
        for (std::size_t i = 0; i < sizeof(T); ++i)
            dst[i] = src[sizeof(T) - 1 - i];
        return result;
    }
}

/**
 * @brief Converts a value from native endianness to big-endian
 *
 * @tparam T The type of value to convert
 * @param value The value to convert
 * @return The value in big-endian byte order
 */
template <typename T>
[[nodiscard]] constexpr T
to_big_endian(T value) noexcept {
    if constexpr (is_little_endian())
        return byteswap(value);
    else
        return value;
}

/**
 * @brief Converts a value from big-endian to native endianness
 *
 * @tparam T The type of value to convert
 * @param value The big-endian value to convert
 * @return The value in native byte order
 */
template <typename T>
[[nodiscard]] constexpr T
from_big_endian(T value) noexcept {
    if constexpr (is_little_endian())
        return byteswap(value);
    else
        return value;
}

/**
 * @brief Converts a value from native endianness to little-endian
 *
 * @tparam T The type of value to convert
 * @param value The value to convert
 * @return The value in little-endian byte order
 */
template <typename T>
[[nodiscard]] constexpr T
to_little_endian(T value) noexcept {
    if constexpr (is_big_endian())
        return byteswap(value);
    else
        return value;
}

/**
 * @brief Converts a value from little-endian to native endianness
 *
 * @tparam T The type of value to convert
 * @param value The little-endian value to convert
 * @return The value in native byte order
 */
template <typename T>
[[nodiscard]] constexpr T
from_little_endian(T value) noexcept {
    if constexpr (is_big_endian())
        return byteswap(value);
    else
        return value;
}

} // namespace qb::endian

#endif // QB_SYSTEM_ENDIAN_H
