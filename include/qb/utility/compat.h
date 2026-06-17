/**
 * @file qb/utility/compat.h
 * @brief Small C++20/C++23 compatibility layer for qb and qbm modules.
 */
#pragma once

#include <bit>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(__has_include)
#  if __has_include(<expected>)
#    include <expected>
#  endif
#endif

#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#  define QB_COMPAT_HAS_STD_EXPECTED 1
#else
#  define QB_COMPAT_HAS_STD_EXPECTED 0
#endif

namespace qb {

template <typename E>
[[nodiscard]] constexpr std::underlying_type_t<E> to_underlying(E value) noexcept {
    return static_cast<std::underlying_type_t<E>>(value);
}

template <typename T>
[[nodiscard]] constexpr T byteswap(T value) noexcept {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                  "qb::byteswap only supports integral and enum types");

    if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        return static_cast<T>(qb::byteswap(static_cast<U>(value)));
    } else {
#if defined(__cpp_lib_byteswap) && __cpp_lib_byteswap >= 202110L
        return std::byteswap(value);
#else
        using U = std::make_unsigned_t<T>;
        U input  = static_cast<U>(value);
        U output = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            output = static_cast<U>((output << CHAR_BIT) | (input & static_cast<U>(0xffu)));
            input  = static_cast<U>(input >> CHAR_BIT);
        }
        return static_cast<T>(output);
#endif
    }
}

#if QB_COMPAT_HAS_STD_EXPECTED

using qb::expected;
using std::unexpected;

#else

template <typename E>
class unexpected {
public:
    template <typename G = E>
    constexpr explicit unexpected(G &&error)
        : _error(std::forward<G>(error)) {}

    [[nodiscard]] constexpr E &error() & noexcept { return _error; }
    [[nodiscard]] constexpr E const &error() const & noexcept { return _error; }
    [[nodiscard]] constexpr E &&error() && noexcept { return std::move(_error); }

private:
    E _error;
};

template <typename E>
unexpected(E) -> unexpected<E>;

template <typename T>
struct expected_value {
    T value;
};

template <typename E>
struct expected_error {
    E error;
};

template <typename T, typename E>
class expected {
public:
    constexpr expected()
        requires std::is_default_constructible_v<T>
        : _storage(expected_value<T>{}) {}

    constexpr expected(T const &value)
        : _storage(expected_value<T>{value}) {}

    constexpr expected(T &&value)
        : _storage(expected_value<T>{std::move(value)}) {}

    template <typename G>
    constexpr expected(unexpected<G> const &error)
        : _storage(expected_error<E>{E(error.error())}) {}

    template <typename G>
    constexpr expected(unexpected<G> &&error)
        : _storage(expected_error<E>{E(std::move(error).error())}) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return std::holds_alternative<expected_value<T>>(_storage);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] constexpr T &value() & {
        if (!has_value()) {
            throw std::logic_error("bad expected access");
        }
        return std::get<expected_value<T>>(_storage).value;
    }

    [[nodiscard]] constexpr T const &value() const & {
        if (!has_value()) {
            throw std::logic_error("bad expected access");
        }
        return std::get<expected_value<T>>(_storage).value;
    }

    [[nodiscard]] constexpr T &&value() && {
        if (!has_value()) {
            throw std::logic_error("bad expected access");
        }
        return std::move(std::get<expected_value<T>>(_storage).value);
    }

    [[nodiscard]] constexpr E &error() & {
        return std::get<expected_error<E>>(_storage).error;
    }

    [[nodiscard]] constexpr E const &error() const & {
        return std::get<expected_error<E>>(_storage).error;
    }

    [[nodiscard]] constexpr E &&error() && {
        return std::move(std::get<expected_error<E>>(_storage).error);
    }

    [[nodiscard]] constexpr T &operator*() & { return value(); }
    [[nodiscard]] constexpr T const &operator*() const & { return value(); }
    [[nodiscard]] constexpr T &&operator*() && { return std::move(*this).value(); }

    [[nodiscard]] constexpr T *operator->() { return &value(); }
    [[nodiscard]] constexpr T const *operator->() const { return &value(); }

private:
    std::variant<expected_value<T>, expected_error<E>> _storage;
};

#endif

template <typename T>
[[nodiscard]] inline std::string format_message(std::string_view prefix, T const &value) {
    std::ostringstream out;
    out << prefix << value;
    return out.str();
}

[[noreturn]] inline void unreachable() noexcept {
#if __has_builtin(__builtin_unreachable)
    __builtin_unreachable();
#elif defined(_MSC_VER)
    __assume(false);
#endif
    std::abort();
}

} // namespace qb
