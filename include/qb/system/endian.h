/**
 * @file qb/system/endian.h
 * @brief Endianness detection and byte swapping utilities
 *
 * This file provides utilities for detecting the system's native endianness
 * and converting values between different byte orders (little endian and big endian).
 * It includes functions for runtime and compile-time endianness detection,
 * as well as safe byte-swapping operations that work with arithmetic and enum types.
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
#include <cstdint>
#include <cstring>
#include <type_traits>

// Compile-time endianness detection (GCC/Clang)
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ENDIAN_NATIVE_LITTLE
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define ENDIAN_NATIVE_BIG
#endif
#endif

namespace qb::endian {

/**
 * @brief Enumeration of byte order types
 */
enum class order {
    little, ///< Little-endian (least significant byte first)
    big,    ///< Big-endian (most significant byte first)
    unknown ///< Unknown endianness
};

/**
 * @brief Determines the system's native byte order at runtime
 *
 * Uses compiler macros for detection when available, with a fallback to
 * runtime detection using a union.
 *
 * @return The native byte order of the system
 */
constexpr order
native_order() {
#if defined(ENDIAN_NATIVE_LITTLE)
    return order::little;
#elif defined(ENDIAN_NATIVE_BIG)
    return order::big;
#else
    union {
        uint32_t i;
        uint8_t  c[4];
    } u = {0x01020304};

    return (u.c[0] == 0x04) ? order::little : order::big;
#endif
}

/**
 * @brief Checks if the system is little-endian at compile time when possible
 *
 * Uses compiler macros for detection when available, with a fallback to
 * runtime detection.
 *
 * @return true if system is little-endian, false otherwise
 */
constexpr bool
is_little_endian() {
#if defined(ENDIAN_NATIVE_LITTLE)
    return true;
#elif defined(ENDIAN_NATIVE_BIG)
    return false;
#else
    return native_order() == order::little;
#endif
}

/**
 * @brief Checks if the system is big-endian
 *
 * @return true if system is big-endian, false otherwise
 */
constexpr bool
is_big_endian() {
    return !is_little_endian();
}

/**
 * @brief Swaps the byte order of a value
 *
 * Reverses the bytes of any trivially copyable type. This function is safe
 * to use with any arithmetic or enum type.
 *
 * @tparam T The type of value to byte-swap (must be arithmetic or enum and trivially
 * copyable)
 * @param value The value to byte-swap
 * @return The byte-swapped value
 */
template <typename T>
inline T
byteswap(T value) {
    static_assert(std::is_arithmetic<T>::value || std::is_enum<T>::value,
                  "byteswap only supports arithmetic or enum types");
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

    T              result;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(&value);
    uint8_t       *dst = reinterpret_cast<uint8_t *>(&result);

    for (size_t i = 0; i < sizeof(T); ++i)
        dst[i] = src[sizeof(T) - 1 - i];

    return result;
}

/**
 * @brief Converts a value from native endianness to big-endian
 *
 * @tparam T The type of value to convert
 * @param value The value to convert
 * @return The value in big-endian byte order
 */
template <typename T>
inline T
to_big_endian(T value) {
    return is_little_endian() ? byteswap(value) : value;
}

/**
 * @brief Converts a value from big-endian to native endianness
 *
 * @tparam T The type of value to convert
 * @param value The big-endian value to convert
 * @return The value in native byte order
 */
template <typename T>
inline T
from_big_endian(T value) {
    return is_little_endian() ? byteswap(value) : value;
}

/**
 * @brief Converts a value from native endianness to little-endian
 *
 * @tparam T The type of value to convert
 * @param value The value to convert
 * @return The value in little-endian byte order
 */
template <typename T>
inline T
to_little_endian(T value) {
    return is_big_endian() ? byteswap(value) : value;
}

/**
 * @brief Converts a value from little-endian to native endianness
 *
 * @tparam T The type of value to convert
 * @param value The little-endian value to convert
 * @return The value in native byte order
 */
template <typename T>
inline T
from_little_endian(T value) {
    return is_big_endian() ? byteswap(value) : value;
}

} // namespace qb::endian

#endif // QB_SYSTEM_ENDIAN_H
