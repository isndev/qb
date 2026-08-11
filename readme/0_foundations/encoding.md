# Encoding and conversion

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported) — ef7d3ea7

The three conversions every wire format needs: text to number, host byte order to network byte order, and a unique identifier. Each is one small header with one strong opinion behind it — no exceptions on bad input, no dependence on the global locale, no silent wrap.

**Prerequisites:** none — **See also:** [Foundations overview](./README.md) · [The time vocabulary](./time.md) · [Containers](./containers.md) · [qb-io utilities](../3_qb_io/utilities.md)

## Numbers from text

```cpp
#include <qb/system/parse.h>

namespace qb {
template <class T> std::optional<T> to_number       (std::string_view s, int base = 10) noexcept;
template <class T> std::optional<T> to_number_prefix(std::string_view s,
                                                     std::size_t *consumed = nullptr,
                                                     int base = 10) noexcept;
}
```
<!-- src: qb/src/qb/system/parse.h:108-159 -->

Both are built on `std::from_chars`, take a `std::string_view`, allocate nothing, and return `std::optional<T>` where `T` is any non-`bool` integral or floating-point type. A `static_assert` on `qb::detail::is_parsable_number_v<T>` rejects everything else, `bool` included — a boolean on a wire is a keyword like `"true"`/`"t"`, not a number, and parsing it as one is a domain decision that does not belong here (`qb/src/qb/system/parse.h:59-61`).

This is the framework's one seam for the job. It exists because the `std::stoi` / `std::stod` / `strtol` family is wrong for a server in four separate ways, and every one of them has produced a real defect somewhere:

| `std::sto*` behaviour | Consequence in a parser | What `qb::to_number` does |
|---|---|---|
| throws `std::invalid_argument` / `std::out_of_range` | a malformed byte from the network unwinds through unrelated frames, or costs a `try`/`catch` on the hot path | returns `std::nullopt` |
| honours the global C locale | a process that set a comma-decimal locale mis-parses `"1.5"` — including in a library it did not write | always the C locale: `.` is the decimal point |
| `std::stod`/`std::stof` throw `out_of_range` on a **representable** subnormal | a legitimate value near `DBL_TRUE_MIN` (~4.9e-324) is rejected as garbage | parses it exactly |
| `strtol` clamps and sets `errno`; the `sto*` family throws | out-of-range is easy to miss and easy to mistake for a valid clamp | out-of-range is `std::nullopt`, never a wrapped or truncated value |

<!-- src: qb/src/qb/system/parse.h:6-21 -->

### Strict versus lenient — pick by whether trailing data is legal

**`to_number<T>` is strict.** The *entire* view must be one canonical number: no surrounding whitespace, no leading `+`, nothing after the digits.

```cpp
qb::to_number<std::uint16_t>("8080");     // 8080
qb::to_number<int>("12x");                // nullopt — trailing 'x'
qb::to_number<int>(" 42");                // nullopt — leading space
qb::to_number<int>("+42");                // nullopt — leading '+'
qb::to_number<int>("ff", 16);             // 255      — base 2..36 for integral T
qb::to_number<double>("3.5e-2");          // 0.035
qb::to_number<unsigned>("-1");            // nullopt — an unsigned T rejects '-'
qb::to_number<std::uint8_t>("300");       // nullopt — out of range, not 44
qb::to_number<double>("4.9e-324");        // parses — a subnormal, exactly
qb::to_number<double>("inf");             // parses — also "infinity", "nan", any case
```

The strictness is the feature: this is what you point at untrusted input. `"300"` into a `uint8_t` is `std::nullopt` rather than `44`, and `" 42"` fails rather than quietly tolerating a smuggled leading byte. The check is `r.ec != std::errc{} || r.ptr != last` — the parse must succeed *and* consume everything (`qb/src/qb/system/parse.h:117-118`).

**`to_number_prefix<T>` is lenient** — the `strtol` idiom, faithfully: skip leading whitespace, accept a leading `+`, take the longest numeric prefix, ignore the rest.

```cpp
std::size_t used = 0;
qb::to_number_prefix<long>("  42 rest", &used);   // 42,  used == 4
qb::to_number_prefix<int>("+7abc", &used);        //  7,  used == 2
qb::to_number_prefix<int>("abc");                 // nullopt — no number at all
```

`consumed` counts from the **start of the input**, including the skipped whitespace and the sign — which is what makes it usable as a cursor through a larger buffer. That is exactly how the date and time-of-day parsers on [the time page](./time.md#the-component-codecs-underneath) walk `"YYYY-MM-DD"` without `sscanf`: `qb::detail::scan_int_field` is a thin wrapper over it (`qb/src/qb/system/time.h:215-226`).

`std::from_chars` itself rejects a leading `+` for both integral and floating types, so `to_number_prefix` skips one explicitly to match the `sto*` family it replaces (`qb/src/qb/system/parse.h:148-149`).

Every result above was compiled and executed against this checkout.

## Byte order

```cpp
#include <qb/system/endian.h>

namespace qb::endian {
enum class order { little, big, native, unknown };

consteval order native_order()     noexcept;
consteval bool  is_little_endian() noexcept;
consteval bool  is_big_endian()    noexcept;

template <class T> constexpr T byteswap(T value) noexcept;

template <class T> constexpr T to_big_endian     (T value) noexcept;
template <class T> constexpr T from_big_endian   (T value) noexcept;
template <class T> constexpr T to_little_endian  (T value) noexcept;
template <class T> constexpr T from_little_endian(T value) noexcept;
}
```
<!-- src: qb/src/qb/system/endian.h:38-169 -->

The detection functions are `consteval`, so they are answered by the compiler and cannot appear in a runtime branch — which is the point: a byte-order test that survives to run time is a byte-order test the optimiser could not remove. The four converters are `constexpr` and compile to either nothing or a single swap instruction, because the choice is made with `if constexpr` at `qb/src/qb/system/endian.h:117-120`.

```cpp
std::uint32_t host = 0x01020304u;

auto be   = qb::endian::to_big_endian(host);       // for the wire
auto back = qb::endian::from_big_endian(be);       // == host
auto sw   = qb::endian::byteswap(host);            // 0x04030201, unconditional

enum class Kind : std::uint16_t { ping = 0x1234 };
qb::endian::byteswap(Kind::ping);                   // Kind{0x3412}
```

Three properties worth knowing:

- **`to_*` and `from_*` are the same function.** A byte-order conversion is its own inverse; the two names exist so a call site reads as a direction rather than as an operation. Use them in pairs and the round trip is exact — verified here for `uint32_t` and for `float`.
- **`byteswap` accepts arithmetic *and* enum types.** An enum is swapped through its underlying type and handed back as the enum (`qb/src/qb/system/endian.h:94-96`), which matters because wire protocol tags are usually scoped enums.
- **A non-integral arithmetic type falls to a portable reverse-copy** (`qb/src/qb/system/endian.h:97-104`). `float` and `double` therefore work, but the result is only meaningful if both ends agree on the floating-point representation — a much stronger assumption than agreeing on byte order. Prefer sending a fixed-width integer.

Underneath, `qb::endian::byteswap` delegates the integral case to `qb::byteswap`, which uses `std::byteswap` when the standard library advertises it and a portable per-byte loop otherwise (`qb/src/qb/utility/compat.h:217-238`). Two `static_assert`s at the entry point reject anything that is neither arithmetic nor enum, and anything not trivially copyable (`qb/src/qb/system/endian.h:89-90`).

## Identifiers

```cpp
#include <qb/uuid.h>

namespace qb {
using uuid = ::uuids::uuid;      // 128-bit, RFC 4122
uuid generate_random_uuid();     // version 4 (random)
}
```
<!-- src: qb/src/qb/uuid.h:45-57 -->

`qb::uuid` aliases the vendored `stduuid` type, so its full comparison, formatting and parsing surface is available under the `uuids` namespace. Two things about the wrapper are qb's own and worth stating:

- **`generate_random_uuid()` is defined in the compiled library**, not inline. A translation unit that includes only `<qb/uuid.h>` still has to link `qb::io`. That is also why this header pulls in `<qb/utility/abi.h>`: it is a documented top-level entry point, so it carries the [link-time configuration fingerprint](./abi_and_build_fingerprint.md) like every other one, with no exception to explain (`qb/src/qb/uuid.h:31-35`).
- **JSON serialisation is provided.** `uuids::to_json` / `from_json` adapters are declared in `qb/json.h`, so a `qb::uuid` round-trips through `qb::json` without a manual conversion (`qb/src/qb/json.h:301-311`).

```cpp
qb::uuid id = qb::generate_random_uuid();
```

**It is an identifier, never a secret.** Version 4 sets 122 of the 128 bits at random, and the generator qb draws them from is a `thread_local std::mt19937` seeded from `std::random_device` (`qb/src/qb/io/io.cpp:42-52`). Mersenne Twister is fast and thread-safe here by construction — the generator is per thread, so concurrent calls do not share state — but it is **not** cryptographically secure: its internal state is recoverable from a sequence of outputs, which makes future UUIDs predictable. Use a UUID to name a thing. For a value that authorises something — a session token, a reset link, an API key — use `qb::crypto::generate_secure_random_string`, `generate_random_bytes` or `generate_token`, which draw from OpenSSL's CSPRNG; see [qb-io utilities](../3_qb_io/utilities.md#cryptography-qbcrypto).

## Pitfalls

- **`to_number` returns `std::nullopt` for "out of range", and so does "malformed".** They are the same answer. If your error message needs to distinguish a typo from an overflow, check the shape of the input yourself first (`qb/src/qb/system/parse.h:117-118`).
- **`to_number` rejects a leading `+` and any surrounding whitespace.** That is deliberate for untrusted input, and it is the most common reason a call that "obviously should work" returns `std::nullopt`. Reach for `to_number_prefix` when the input legitimately carries either (`qb/src/qb/system/parse.h:144-149`).
- **`to_number<bool>` does not compile.** Deliberately (`qb/src/qb/system/parse.h:59-61`).
- **`consumed` includes skipped whitespace and the sign**, so it is an offset from the start of the view, not a count of digits (`qb/src/qb/system/parse.h:156-157`).
- **The endian detectors are `consteval`.** `if (qb::endian::is_little_endian())` is fine — it is a constant — but you cannot take their address, pass them as a callback, or call them on a runtime value.
- **Byte-swapping a `float` swaps bytes, not representations.** It works, and it only means something if both ends share an IEEE-754 layout (`qb/src/qb/system/endian.h:97-104`).
- **A UUID is not a secret.** `generate_random_uuid()` draws from a Mersenne Twister, not a CSPRNG (`qb/src/qb/io/io.cpp:42-52`), so a sequence of UUIDs makes the next one predictable. Never use one as a token.

## See also

- [Foundations overview](./README.md) — the rest of the layer below the event loop.
- [The time vocabulary](./time.md) — the date and time-of-day parsers built on `to_number_prefix`.
- [Containers](./containers.md) — `qb::string<N>`, the other value type an event can carry.
- [qb-io utilities](../3_qb_io/utilities.md) — URI parsing, cryptography, compression and JSON, the batteries that sit one layer up.
