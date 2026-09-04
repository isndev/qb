# Containers

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.1.0 (C++20 default, C++23 supported) — ef7d3ea7

The four container families qb adds to the standard library: the node-stable hash map and set the engine holds references into, their flat open-addressing counterparts, the case-insensitive wrappers HTTP headers and query strings use, and `qb::string<N>` — the inline fixed-capacity string that makes an event relocatable.

**Prerequisites:** none — **See also:** [Foundations overview](./README.md) · [The pipe](./buffers.md) · [Core invariants](../7_reference/core_invariants.md) · [Encoding and conversion](./encoding.md)

## Two hash maps, and the one property that separates them

```cpp
#include <qb/system/container/unordered_map.h>
#include <qb/system/container/unordered_set.h>

namespace qb {
template <class K, class V, class H = std::hash<K>, class E = std::equal_to<K>, class A = …>
using unordered_map      = ska::unordered_map<K, V, H, E, A>;       // node-based
template <class K, class V, …>
using unordered_flat_map = ska::flat_hash_map<K, V, H, E, A>;       // open addressing

template <class K, …> using unordered_set      = ska::unordered_set<K, H, E, A>;
template <class K, …> using unordered_flat_set = ska::flat_hash_set<K, H, E, A>;
}
```
<!-- src: qb/src/qb/system/container/unordered_map.h:49-50, :100-101 -->
<!-- src: qb/src/qb/system/container/unordered_set.h:45-46, :74-75 -->

Both are from the vendored `ska_hash` library and both are drop-in for their `std::` equivalents. The difference is where the value lives, and it is the only thing you need to decide between them.

| | `qb::unordered_map` / `unordered_set` | `qb::unordered_flat_map` / `unordered_flat_set` |
|---|---|---|
| Layout | chained buckets, value in an individually allocated entry | open addressing with robin-hood probing, value inline in the table |
| Rehash invalidates | iterators only | **iterators, references and pointers** |
| Locality | one indirection per lookup | none |
| Use when | you hold a reference, pointer or `&map[k]` across an insertion | you only ever look up and copy out |

`qb::unordered_map` gives the same stability contract `std::unordered_map` does: **a rehash invalidates iterators but not references or pointers to elements** (`qb/src/qb/system/container/unordered_map.h:53-60`). That is not a nice-to-have here — the engine depends on it. `VirtualCore::ActorMap` (`src/qb/core/VirtualCore.h:158`), `Main::_registered_services` (`src/qb/core/Main.h:204`) and the event router's `_registered_events` (`src/qb/system/event/router.h:446`) are all held across insertions.

Measured on this checkout: a reference taken from a `qb::unordered_map<std::string, int>` before 10 000 further insertions — many rehashes — still reads the right value afterwards.

### What that changes about the bugs you can write

This is worth being precise about, because "node-based" is often read as "safe", and it is safe against exactly one thing.

- **Reference use-after-free across a rehash: cannot happen.** The node is not moved.
- **Iterator invalidation across an insert: still happens.** Holding an iterator (or a range-`for`) across an insertion into the same map is undefined behaviour in both variants.
- **Reentrancy: still happens.** If a value's destructor, or a callback invoked while you hold a reference into the map, erases *that* entry, the node is freed and the reference dangles. Node stability protects you from the container reorganising itself; it protects you from nothing you do yourself.

So when auditing engine-adjacent code that indexes a `qb::unordered_map`, the questions are "does anything insert while this iterator is live?" and "can this callback erase the entry I am holding?" — not "can this reference move?".

### The alias is unconditional, and must stay that way

`qb::unordered_map` resolves to `ska::unordered_map` in **every** build configuration. It must never be made to depend on `NDEBUG`, `CMAKE_BUILD_TYPE`, or any other build macro, and the header says so as a `@warning` rather than as a comment (`qb/src/qb/system/container/unordered_map.h:62-80`).

The reason is that this is a **public type used as a data member of public classes** — `qb::VirtualCore`, `qb::Main`, `qb::router::*`, and qbm's own headers. A macro-selected implementation would make `sizeof(qb::unordered_map<int,int>)` differ between two translation units (measured: 32 against 40), so a consumer compiled one way against a library compiled the other reads one map's storage through the other's layout. The observed symptom is a runtime abort far from the cause — `std::overflow_error: __next_prime overflow` — with no diagnostic from the compiler or the linker, because each translation unit is internally consistent and vague linkage merges the bodies silently.

The invariant has a configure-time guard: qb publishes the implementation as a token, `set(QB_ABI_UNORDERED_MAP "ska" CACHE INTERNAL ...)` (`qb/cmake/qbConfig.cmake:605`), each installed module records the token it was built against (its `qbm-<mod>Config.cmake` is generated from `qb/cmake/qbmModuleConfig.cmake.in`), and a mismatch fails at `find_package()` rather than at run time. That is the same family of protection as the [link-time ABI fingerprint](./abi_and_build_fingerprint.md), one layer up in the build system.

### Custom keys

`qb::hash_combine` folds several values into one hash, so a struct key needs no hand-rolled mixing:

```cpp
#include <qb/utility/functional.h>

struct MyKey {
    int         id;
    std::string name;
    bool operator==(const MyKey &) const = default;
};

template <>
struct std::hash<MyKey> {
    std::size_t operator()(const MyKey &k) const { return qb::hash_combine(k.id, k.name); }
};
```
<!-- src: qb/src/qb/utility/functional.h:74-79 -->

It is `constexpr` and `noexcept`, and folds each value with the usual `0x9e3779b9` mix (`qb/src/qb/utility/functional.h:42-44`). It is a *combiner*, not a cryptographic or DoS-resistant hash; a map keyed on attacker-controlled data needs a keyed hash, not this.

## Case-insensitive maps

HTTP header names are case-insensitive, and so are URI query keys in qb. Rather than lowercasing at every call site, `icase_basic_map` wraps any map type and normalises the key on the way in and out:

```cpp
namespace qb {
template <class Map, class Trait = string_to_lower> class icase_basic_map;

template <class Value, class Trait = string_to_lower>
using icase_map           = icase_basic_map<std::map<std::string, Value>, Trait>;
template <class Value, class Trait = string_to_lower>
using icase_unordered_map = icase_basic_map<qb::unordered_map<std::string, Value>, Trait>;
}
```
<!-- src: qb/src/qb/system/container/unordered_map.h:228-229, :414-424 -->

```cpp
qb::icase_unordered_map<int> headers;
headers["Content-Length"] = 42;
headers["content-length"];        // 42 — the same entry
headers.has("CONTENT-LENGTH");    // true
```

Four things to know about it:

- **The stored key is the lowercased one.** `_Trait::convert` runs before every `emplace`, `try_emplace`, `at`, `operator[]`, `find` and `erase` — the first at `qb/src/qb/system/container/unordered_map.h:267-270`, the last at `:376-380`, so iterating the map yields lowercase keys — not the casing you inserted. If you must reproduce the original spelling on the wire, store it in the value.
- **The conversion is ASCII-only.** `string_to_lower::charToLower` maps `'A'`–`'Z'` and leaves every other byte alone (`qb/src/qb/system/container/unordered_map.h:116-119`). That is correct for HTTP field names and wrong for arbitrary Unicode, deliberately.
- **`has(key)` is the membership test** (`qb/src/qb/system/container/unordered_map.h:363-367`). The base map's `count` and `contains` are not among the re-exported members — deliberately, and the point sharpened in 3.0 when the fork gained a `contains()`: there is still no way to reach a lookup that skips the key conversion, which is the whole reason the inheritance is private.
- **Inheritance is private**, and only a hand-picked set of base members is re-exported: `begin`, `cbegin`, `cend`, `clear`, `empty`, `end`, `erase`, `size` (`qb/src/qb/system/container/unordered_map.h:383-390`). Anything not on that list is intentionally unreachable, because reaching it would skip the key conversion.

`convert_key(k)` is available as a static helper when you need the normalised form outside a map operation (`qb/src/qb/system/container/unordered_map.h:401-405`).

## `qb::string<N>` — the reason the first example in the README is not `std::string`

```cpp
template <std::size_t _Size = 30>
class string : public std::array<char, _Size + 1> { … };
```
<!-- src: qb/src/qb/string.h:85-86 -->

An inline, `std::array`-backed string with a compile-time capacity and a length field. No heap allocation, no pointer, and — the property that matters most — **no pointer into itself**.

That last point is why events use it. qb relocates an event with raw `memcpy` and abandons the source without running a destructor there, so an event payload must be trivially *relocatable*: no member may hold a pointer into its own storage. A short `std::string` on libstdc++ violates that (its `_M_p` addresses its own internal buffer), which corrupts on Linux and is structurally invisible under libc++ on macOS. `qb::string<N>` cannot: measured on this checkout, `std::is_trivially_copyable_v<qb::string<64>>` and `std::is_trivially_destructible_v<qb::string<64>>` are both true. The full relocation contract is on [Core invariants](../7_reference/core_invariants.md#events-must-be-trivially-relocatable).

```cpp
#include <qb/string.h>

qb::string<32> name = "actor-42";   // stored inline, no allocation
name.append("-worker");             // "actor-42-worker", size() == 15
name.c_str();                       // NUL-terminated, always
name.size();                        // current length
name.capacity();                    // 32 — the compile-time maximum, == max_size()
```

### Layout

The length field uses the smallest unsigned type that can hold `N + 1`: `uint8_t`, `uint16_t`, or `std::size_t` (`qb/src/qb/string.h:52-72`, `:90`). Measured sizes on this checkout:

| Type | `sizeof` | Why |
|---|---|---|
| `qb::string<8>` | 10 | 9 bytes of array + `uint8_t` |
| `qb::string<30>` | 32 | 31 + `uint8_t` |
| `qb::string<64>` | 66 | 65 + `uint8_t` |
| `qb::string<300>` | 304 | 301 + `uint16_t` + padding |
| `qb::string<70000>` | 70016 | 70001 + `std::size_t` + padding |

The `+ 1` is the NUL terminator, which is always written and never counted by `size()`. `capacity()` and `max_size()` both return `N` — the usable character count, excluding the terminator (`qb/src/qb/string.h:547-560`).

### It truncates, it never throws and it never grows

Every mutating entry point clamps to the free room rather than failing:

```cpp
qb::string<8> s = "abcdefghIJKL";   // 12 characters into a capacity of 8
s.c_str();                          // "abcdefgh"
s.size();                           // 8
```

`assign` clamps the copy length with `std::min(size, _Size)` (`qb/src/qb/string.h:199-207`).

Both `append` overloads clamp on the *free room* rather than on `_size + len` — deliberately, because the sum wraps for a hostile length and the wrapped difference would overrun the fixed buffer (`qb/src/qb/string.h:789-802`, `:822-834`).

`push_back` is a no-op at capacity (`qb/src/qb/string.h:841-848`).

There is no signal. Size the capacity to your worst case, and if truncation would be a correctness bug, check the length before assigning.

### The rest of the surface

Familiar `std::string` shape: `at` (bounds-checked, throws `std::out_of_range`), `operator[]` (unchecked), `front`, `back`, `data`, `c_str`, iterators, `substr`, `compare`, `find`/`rfind`, `starts_with`/`ends_with`/`contains`, `operator+`/`+=`, and stream `<<`/`>>`. Two implicit conversions make it interoperate with the standard library without a cast: to `std::string` and to `std::string_view` (`qb/src/qb/string.h:314-325`).

`operator+` on two `qb::string`s returns a `string<std::max(_Size1, _Size2)>` (`qb/src/qb/string.h:1155-1161`) — so concatenating two full strings of the same capacity truncates. Build with `append` into a big enough target when the result must be complete.

One implementation detail is worth knowing because it is a real hazard elsewhere: `find(char)` uses `std::memchr`, bounded by `_size`, rather than `strchr`. `strchr` is unbounded and needs a terminator, so it can scan past the live characters into the rest of the fixed buffer *before* the result is range-checked — the over-read has already happened by then (`qb/src/qb/string.h:700-712`).

## `qb::ring_buffer<T, N, Overwrite>`

```cpp
#include <qb/system/container/ring_buffer.h>

template <typename T, size_t N, bool Overwrite = true>
class ring_buffer;
```
<!-- src: qb/src/qb/system/container/ring_buffer.h:47-48 -->

A fixed-capacity circular FIFO with `push_back` / `pop_front` / `front` / `back` / `operator[]`, forward iterators, `empty()`, `full()` and `capacity()` (`qb/src/qb/system/container/ring_buffer.h:224-238`). There is **no** `size()`: the live element count comes from the iterators, `std::distance(cbegin(), cend())`. Storage is inline — `N` slots of raw aligned bytes with placement construction — so a full buffer performs no allocation. `N > 0` is a `static_assert` (`qb/src/qb/system/container/ring_buffer.h:229`).

The `Overwrite` parameter is the whole policy: at capacity, `true` (the default) drops the oldest element to make room, `false` makes `push_back` a no-op (`qb/src/qb/system/container/ring_buffer.h:283-288`). Pick deliberately — a sliding-window metric wants `true`, a bounded work queue almost never does.

**It is an offered utility, not engine machinery.** Nothing in `qb/src`, the tests or the modules instantiates it; it is here for your code. Its one demonstrator is `examples/02-io/11-logging-and-metrics.cpp`, which keeps a rolling latency window in a `qb::ring_buffer<std::uint64_t, 64>` — `Overwrite = true`, so the oldest sample is evicted at capacity — and counts its live samples through the iterators for exactly the reason above. Do not read it as a description of how the engine buffers anything — the engine's queues are the [pipe](./buffers.md) and the [lock-free rings](./concurrency_primitives.md), which are different types with different contracts. It is also **not** thread-safe: single-threaded use, or your own lock.

## Pitfalls

- **`qb::string<N>` truncates silently.** Assigning or appending past `N` clamps; nothing throws and nothing reports it (`qb/src/qb/string.h:199-207`). This is the one that bites, because a path or a name that fits in your tests will not fit in production.
- **`operator+` truncates too**, at `std::max` of the two capacities (`qb/src/qb/string.h:1155-1161`).
- **An `icase_*` map stores the lowercased key.** Iterating it will not give you back the casing you inserted (`qb/src/qb/system/container/unordered_map.h:267-271`).
- **`icase_*` lowercasing is ASCII-only** (`qb/src/qb/system/container/unordered_map.h:116-119`). Correct for HTTP field names; not a Unicode case-folding.
- **Node stability is not reentrancy safety.** A reference into a `qb::unordered_map` survives a rehash and does *not* survive an `erase` of that entry from a callback you invoked while holding it.
- **`unordered_flat_map` invalidates references on rehash.** Reach for it only when you copy values out; if you are unsure which you have, you want `qb::unordered_map`.
- **Do not make the `unordered_map` alias conditional on a build macro**, for any reason, including debugger pretty-printers (`qb/src/qb/system/container/unordered_map.h:62-80`). It is the layout of a public type.
- **`ring_buffer` with `Overwrite = true` loses data by design.** The default is the lossy one.
- **None of these containers is thread-safe.** Every one of them belongs to exactly one thread, which in qb means one `VirtualCore`.

## See also

- [Foundations overview](./README.md) — the rest of the layer.
- [The pipe](./buffers.md) — the buffer these sit beside, and where `qb::string<N>` gets written when an event is serialised.
- [Core invariants](../7_reference/core_invariants.md) — the relocation contract that makes `qb::string<N>` the right choice inside an event.
- [ABI and the build fingerprint](./abi_and_build_fingerprint.md) — why a public type's identity must not depend on a build macro.
- [Encoding and conversion](./encoding.md) — `qb::to_number` for turning a header value into a number.
