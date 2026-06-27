/**
 * @file qb/system/parse.h
 * @brief Locale-independent, non-throwing string-to-number parsing.
 *
 * Single seam for turning text into integers and floating-point values across qb
 * and its modules. Built entirely on `std::from_chars`, which is the modern,
 * correct replacement for the `std::stoi` / `std::stod` / `std::strtol` family:
 *
 *  - **Non-throwing.** Failure is an empty `std::optional`, not an exception, so
 *    there is no `try`/`catch` ceremony and no raw `std::invalid_argument` /
 *    `std::out_of_range` leaking out of a parser into unrelated code.
 *  - **Locale-independent.** `std::stod` honours the global C locale, so a
 *    process that set a comma-decimal locale would mis-parse "1.5". `from_chars`
 *    always uses the C locale ('.' decimal), which is what every wire format wants.
 *  - **Correct on subnormals.** `std::stod` / `std::stof` throw `out_of_range`
 *    on a representable subnormal underflow (e.g. DBL_TRUE_MIN ~4.9e-324) even
 *    though the value is exact; `from_chars` parses it precisely.
 *  - **Range-checked.** A magnitude past the type range is reported, never
 *    silently wrapped or truncated.
 *  - **Allocation-free.** Takes a `std::string_view`; no temporary `std::string`.
 *
 * Two contracts are offered:
 *  - ::qb::to_number — STRICT: the whole string must be one canonical number
 *    (no surrounding whitespace, no leading '+', no trailing characters).
 *  - ::qb::to_number_prefix — LENIENT: the `std::stoi`/`std::strtol` idiom —
 *    skip leading whitespace, accept a leading '+', parse the longest numeric
 *    prefix, ignore trailing characters; reports how many bytes were consumed.
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
 * @ingroup System
 */

#ifndef QB_SYSTEM_PARSE_H
#define QB_SYSTEM_PARSE_H
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

namespace qb {

namespace detail {

/// A type qb::to_number can parse: any integral type except bool (which is a
/// domain keyword like "true"/"t", not a number), or any floating-point type.
template <class T>
inline constexpr bool is_parsable_number_v =
    (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>) ||
    std::is_floating_point_v<T>;

/// Strip leading C-locale whitespace (the exact set std::strtol skips), without
/// touching the global locale. Returns a sub-view of the same buffer.
[[nodiscard]] inline std::string_view
ltrim_ascii_ws(std::string_view s) noexcept {
    std::size_t i = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' && c != '\r')
            break;
    }
    return s.substr(i);
}

/// from_chars dispatch: integral honours `base`, floating ignores it.
template <class T>
[[nodiscard]] inline std::from_chars_result
from_chars_dispatch(const char *first, const char *last, T &value, int base) noexcept {
    if constexpr (std::is_integral_v<T>)
        return std::from_chars(first, last, value, base);
    else
        return std::from_chars(first, last, value);
}

} // namespace detail

/**
 * @brief Strict, locale-independent, non-throwing string-to-number conversion.
 *
 * The ENTIRE string must be a single canonical number: no surrounding
 * whitespace, no leading '+', no trailing characters. Any deviation, or a value
 * outside the range of @p T, yields `std::nullopt`.
 *
 *  - Integral @p T: an optional @p base (2..36) is honoured exactly like
 *    `std::from_chars`; only a leading '-' sign is accepted (an unsigned @p T
 *    rejects '-').
 *  - Floating @p T: parses fixed and scientific notation plus the
 *    case-insensitive `inf`/`infinity`/`nan` spellings. Subnormals parse exactly
 *    (unlike `std::stod`, which throws on them). A magnitude past the type range
 *    returns `std::nullopt`.
 *
 * @tparam T   Non-bool integral or floating-point type.
 * @param  s    Text to parse.
 * @param  base Integer base (integral @p T only; ignored for floating @p T).
 * @return The parsed value, or `std::nullopt` on any malformed/out-of-range input.
 */
template <class T>
[[nodiscard]] std::optional<T>
to_number(std::string_view s, int base = 10) noexcept {
    static_assert(detail::is_parsable_number_v<T>,
                  "qb::to_number<T>: T must be a non-bool integral or "
                  "floating-point type");
    T          value{};
    const char *first = s.data();
    const char *last  = s.data() + s.size();
    const auto  r     = detail::from_chars_dispatch(first, last, value, base);
    if (r.ec != std::errc{} || r.ptr != last)
        return std::nullopt;
    return value;
}

/**
 * @brief Lenient prefix conversion — the `std::stoi` / `std::strtol` idiom.
 *
 * Skips leading whitespace, accepts a leading '+', parses the longest valid
 * numeric prefix, and IGNORES any trailing characters. This is the faithful,
 * non-throwing replacement for `std::stoi` / `std::stoll` / `std::strtod` at a
 * call site that intentionally tolerates trailing data (e.g. "12abc" -> 12).
 *
 * @tparam T        Non-bool integral or floating-point type.
 * @param  s        Text to parse.
 * @param  consumed Optional out-param; receives the number of bytes read from
 *                  the START of @p s, including any skipped whitespace and sign.
 * @param  base     Integer base (integral @p T only; ignored for floating @p T).
 * @return The parsed value, or `std::nullopt` when no number could be read at
 *         all, or the parsed magnitude is out of range. Never throws.
 */
template <class T>
[[nodiscard]] std::optional<T>
to_number_prefix(std::string_view s, std::size_t *consumed = nullptr,
                 int base = 10) noexcept {
    static_assert(detail::is_parsable_number_v<T>,
                  "qb::to_number_prefix<T>: T must be a non-bool integral or "
                  "floating-point type");
    const char      *origin = s.data();
    std::string_view body   = detail::ltrim_ascii_ws(s);
    // std::from_chars rejects a leading '+' (for both integral and floating
    // types); the std::sto* family accepts it. Skip it so a positive sign is
    // tolerated identically.
    if (!body.empty() && body.front() == '+')
        body.remove_prefix(1);
    T          value{};
    const char *first = body.data();
    const char *last  = body.data() + body.size();
    const auto  r     = detail::from_chars_dispatch(first, last, value, base);
    if (r.ec == std::errc::invalid_argument || r.ec == std::errc::result_out_of_range)
        return std::nullopt;
    if (consumed != nullptr)
        *consumed = static_cast<std::size_t>(r.ptr - origin);
    return value;
}

} // namespace qb

#endif // QB_SYSTEM_PARSE_H
