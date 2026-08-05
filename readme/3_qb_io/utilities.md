# qb-io utilities

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Beyond sockets and the event loop, `qb-io` ships a set of standalone utilities — the canonical time vocabulary, cryptography and JWT, compression, URI parsing, fixed-capacity strings and flat hash maps, UUIDs, JSON, and endian helpers — usable on their own without the actor runtime.

**Prerequisites:** [qb-io module overview](./README.md) — **See also:** [Async I/O system](./async_system.md), [Protocols](./protocols.md), [Lock-free primitives](./../7_reference/lockfree_primitives.md)

---

## Summary

These utilities live in headers under `qb/src/qb/`. Several are header-only with no third-party dependency: the time vocabulary, `qb::string<N>`, the flat-map/icase-map containers, `qb::uuid`, JSON, and endian helpers. The crypto/JWT, compression, and URI slices are *declared* in headers but *defined* in compiled translation units that ship inside the `qb::io` library (`src/qb/io/{crypto*.cpp,compression.cpp,uri.cpp}`), so using them requires linking `qb::io` (and, for crypto/compression, the OpenSSL/zlib dependency) — there is no header-only inline path for these three. Two slices are additionally gated on optional libraries resolved at configure time:

| Slice | Header | Namespace | Build gate |
|---|---|---|---|
| Time vocabulary | `qb/system/time.h` | `qb` | always |
| Number parsing | `qb/system/parse.h` | `qb` | always |
| Crypto + JWT | `qb/io/crypto.h`, `qb/io/crypto_jwt.h` | `qb::crypto`, `qb::jwt` | OpenSSL (`QB_WITH_SSL`) |
| Compression | `qb/io/compression.h` | `qb::compression`, `qb::gzip`, `qb::deflate` | zlib (`QB_WITH_COMPRESSION`) |
| URI | `qb/io/uri.h` | `qb::io` | always |
| Fixed string | `qb/string.h` | `qb` | always |
| Flat maps/sets | `qb/system/container/unordered_map.h`, `unordered_set.h` | `qb` | always |
| UUID | `qb/uuid.h` | `qb`, `uuids` | always (vendored `stduuid`) |
| JSON | `qb/json.h` | `qb` (via `nlohmann`) | always (vendored `nlohmann/json`) |
| Endian | `qb/system/endian.h` | `qb::endian` | always |

`QB_WITH_SSL` and `QB_WITH_COMPRESSION` are user-facing build requests; they resolve to the compile-time defines `QB_HAS_SSL` and `QB_HAS_COMPRESSION` once the dependency is found. If the dependency is absent, the request is forced off and the corresponding header `#error`s when included (`crypto.h` → `"missing OpenSSL Library"`, `compression.h` → `"missing Z Library"`). _(`qb/src/qb/io/crypto.h:33-34`, `qb/src/qb/io/compression.h:37-38`; `docs-overhaul/qb/FACTBOOK.md` build-options table.)_

---

## Time vocabulary (`qb::duration`, `qb::mono_time`, `qb::wall_time`)

The single source of truth for time across qb and its modules, built entirely on `std::chrono`. The model is deliberately minimal: one span type and two distinct instant types. _(`qb/src/qb/system/time.h:82-88`.)_

| Type | Alias | Use for |
|---|---|---|
| `qb::duration` | `std::chrono::nanoseconds` | Every timeout, delay, TTL, interval, and latency value in public APIs. |
| `qb::mono_time` | `std::chrono::steady_clock::time_point` | Deadlines, timers, the event-loop "now", latency, RTT. Immune to NTP/DST adjustments. |
| `qb::wall_time` | `std::chrono::system_clock::time_point` | Dates, expiry, JWT `exp`/`nbf`, TLS validity, logs, wire formats. |

`qb::duration` accepts any finer-or-equal `std::chrono` literal implicitly (`30s`, `100ms`, `5us`) and **rejects a bare integer**, so a seconds-versus-milliseconds unit confusion cannot compile. The two instant types are distinct on purpose: subtracting a wall instant from a monotonic one does not compile, which removes a class of "timeout fired early because the clock stepped" bugs. _(`qb/src/qb/system/time.h:8-20`.)_

> The pre-2.0 PascalCase time aliases no longer exist. Use the three canonical lowercase types above.

### Public surface

```cpp
#include <qb/system/time.h>

namespace qb {

// Current instants
mono_time mono_now() noexcept;            // steady_clock::now()
wall_time wall_now() noexcept;            // system_clock::now()

// Unix-epoch extraction (wall instant -> integer count)
int64_t unix_seconds(wall_time) noexcept;
int64_t unix_millis (wall_time) noexcept;
int64_t unix_micros (wall_time) noexcept;
int64_t unix_nanos  (wall_time) noexcept;

// Unix-epoch construction (integer count -> wall instant)
wall_time wall_from_unix_seconds(int64_t) noexcept;
wall_time wall_from_unix_millis (int64_t) noexcept;

// UTC formatting / parsing (no time-zone database dependency)
std::string                 format_utc(wall_time, std::string_view fmt);
std::string                 to_iso8601(wall_time);          // "YYYY-MM-DDTHH:MM:SSZ"
std::optional<wall_time>    parse_utc(std::string_view, std::string_view fmt) noexcept;
std::optional<wall_time>    from_iso8601(std::string_view) noexcept;

// Performance instrumentation (NOT a clock — uncalibrated, per-core)
uint64_t tsc_ticks() noexcept;

} // namespace qb
```
<!-- src: qb/src/qb/system/time.h -->

The chrono literals (`30s`, `100ms`, `5us`, …) are pulled into `qb` via `inline namespace qb::time_literals`, so call sites can write them without an extra `using`. _(`qb/src/qb/system/time.h:110-114`.)_

`format_utc`/`parse_utc` operate in UTC only and have no time-zone database dependency: formatting uses `strftime`, parsing uses `std::get_time` + `timegm`. `format_utc` returns an empty string on failure; `parse_utc`/`from_iso8601` return `std::nullopt` on any parse error. _(`qb/src/qb/system/time.h:29-30,303-347`.)_

`tsc_ticks()` reads the CPU time-stamp counter. It is monotonic per-core and high-resolution but uncalibrated and not comparable to wall or monotonic clocks — use it only for single-thread micro-benchmark deltas, never as a clock. _(`qb/src/qb/system/time.h:680-684`.)_

### Scoped measurement helpers

```cpp
#include <qb/io.h>                 // qb::io::cout
#include <qb/system/time.h>

void process() {
    qb::ScopedTimer timer([](qb::duration d) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
        qb::io::cout() << "process took " << us << "us\n";
    });
    // ... work ...
    // timer fires the callback with the measured qb::duration on scope exit.
}
```
<!-- src: qb/src/qb/system/time.h:718-763 -->

`ScopedTimer` measures elapsed monotonic time between construction and `stop()`/destruction, invoking the callback with the measured `qb::duration`; `stop()`, `restart()`, and `elapsed()` are available for manual control. It is non-copyable and non-movable. `LogTimer` is a thin wrapper that prints the elapsed microseconds of a scope to `stdout` on destruction. _(`qb/src/qb/system/time.h:714-790`.)_

> **Boundary seam.** The only place a raw `double` touches time is `qb::detail::to_ev_seconds` / `from_ev_seconds`, the conversion between `qb::duration` and qev's `qev_tstamp` (double seconds). Application code never needs these. _(`qb/src/qb/system/time.h:796-809`.)_

---

## Number parsing (`qb::to_number`, `qb::to_number_prefix`)

`qb/system/parse.h` provides locale-independent, non-throwing, allocation-free string→number conversion built on `std::from_chars` — the canonical replacement for `std::stoi`/`std::stol`/`std::stod`/`strtol`, which throw on bad input, depend on the global C locale, and (for `std::stod`) reject subnormals. Both functions take a `std::string_view` and return `std::optional<T>`, where `T` is any non-bool integral or floating-point type; a magnitude outside `T`'s range is reported as `std::nullopt`, never silently wrapped or truncated.

Two contracts are offered:

- **`qb::to_number<T>(sv, base = 10)` — strict.** The *entire* view must be a single canonical number: no surrounding whitespace, no leading `+`, no trailing characters. For integral `T` the `base` (2–36) is honoured exactly like `std::from_chars`, and only a leading `-` is accepted (an unsigned `T` rejects `-`). For floating `T` it parses fixed and scientific notation plus the case-insensitive `inf`/`infinity`/`nan` spellings, and parses subnormals exactly.
- **`qb::to_number_prefix<T>(sv, consumed = nullptr, base = 10)` — lenient.** The `strtol`/`stoi` idiom: skip leading whitespace, accept a leading `+`, parse the longest numeric prefix, and ignore any trailing characters. When `consumed` is non-null it receives the number of bytes parsed (so you can advance a cursor through a larger buffer).

```cpp
#include <qb/system/parse.h>

auto port = qb::to_number<std::uint16_t>("8080");           // std::optional{8080}
auto bad  = qb::to_number<int>("12x");                      // std::nullopt — trailing 'x' rejected
auto hex  = qb::to_number<int>("ff", 16);                   // std::optional{255}
auto rate = qb::to_number<double>("3.5e-2");                // std::optional{0.035}

std::size_t used = 0;
auto lead = qb::to_number_prefix<long>("  42 rest", &used); // std::optional{42}, used == 4
```
<!-- src: qb/src/qb/system/parse.h:108,138 -->

Reach for `to_number` to validate untrusted input — a malformed or out-of-range value is a `std::nullopt`, never an exception or a wrapped integer — and for `to_number_prefix` to scan a number off the front of a buffer. The compile-time trait `qb::detail::is_parsable_number_v<T>` (an internal, `detail`-namespace trait) is what both functions `static_assert` on; the time vocabulary itself uses `to_number_prefix` internally to parse wire timestamps without touching the locale.

---

## Cryptography (`qb::crypto`)

`qb::crypto` is an OpenSSL-backed static toolbox: every entry point is a static member of `class crypto`. Requires an OpenSSL-enabled build (`QB_WITH_SSL` → `QB_HAS_SSL`). _(`qb/src/qb/io/crypto.h:33,85`.)_

### Hashing and encoding

```cpp
#include <qb/io/crypto.h>

// Hex-string digests of a std::string (iterations defaults to 1)
std::string h = qb::crypto::sha256("payload");          // 64-char lowercase hex
std::string m = qb::crypto::md5("payload");
//   also: qb::crypto::sha1, qb::crypto::sha512

// Generic digest over bytes -> raw byte vector
std::vector<unsigned char> data = /* ... */;
auto digest = qb::crypto::hash(data, qb::crypto::DigestAlgorithm::SHA256);

// HMAC over bytes
auto mac = qb::crypto::hmac(data, key, qb::crypto::DigestAlgorithm::SHA256);

// Base64 / Base64URL / hex
std::string b64    = qb::crypto::base64_encode(data.data(), data.size());
auto        raw    = qb::crypto::base64_decode(b64);
std::string b64url = qb::crypto::base64url_encode(data);
std::string hex    = qb::crypto::to_hex_string(std::string(data.begin(), data.end()));
```
<!-- src: qb/tests/io/unit/crypto/crypto-primitives.cpp -->

The `std::string` hashing overloads (`md5`, `sha1`, `sha256`, `sha512`) return a hexadecimal string and take an `iterations` count (default `1`). `DigestAlgorithm` covers `MD5`, `SHA1`, `SHA224`, `SHA256`, `SHA384`, `SHA512`, `BLAKE2B512`, and `BLAKE2S256`. _(`qb/src/qb/io/crypto.h:152,309-393`.)_

### Random data

| Function | Backing | Use |
|---|---|---|
| `generate_random_bytes(size_t)` | OpenSSL `RAND_bytes` (CSPRNG) | Keys, IVs, salts. |
| `generate_secure_random_string(len, range)` | OpenSSL `RAND_bytes` (CSPRNG) | Passwords, tokens, session IDs. |
| `generate_salt(size_t)` | OpenSSL `RAND_bytes` (CSPRNG) | Argon2/PBKDF2 salts. |
| `generate_random_string(len, range)` | `std::mt19937` | Non-cryptographic only (e.g. test fixtures). |

`generate_random_string` is seeded from `std::random_device` but uses a Mersenne Twister and is **not** cryptographically secure; use `generate_secure_random_string` for any security-sensitive value. _(`qb/src/qb/io/crypto.h:175-242,505,917`.)_

### Symmetric encryption (AEAD)

```cpp
#include <qb/io/crypto.h>
using C = qb::crypto;

auto key = C::generate_key(C::SymmetricAlgorithm::AES_256_GCM);
auto iv  = C::generate_iv (C::SymmetricAlgorithm::AES_256_GCM);

std::vector<unsigned char> plaintext = /* ... */;
std::vector<unsigned char> aad       = /* optional additional authenticated data */;

auto ct = C::encrypt(plaintext, key, iv, C::SymmetricAlgorithm::AES_256_GCM, aad);
auto pt = C::decrypt(ct,        key, iv, C::SymmetricAlgorithm::AES_256_GCM, aad);
// pt is EMPTY if the GCM authentication tag fails — treat empty as authentication
// failure, never as "decrypted to nothing".
```
<!-- src: qb/src/qb/io/crypto.h:527-546 -->

`SymmetricAlgorithm` covers AES-CBC and AES-GCM at 128/192/256-bit, plus `CHACHA20_POLY1305`. For AEAD modes, `encrypt` appends and `decrypt` verifies the authentication tag; a failed tag yields an empty result. _(`qb/src/qb/io/crypto.h:149,527-546`; `docs-overhaul/qb/FACTBOOK.md:484`.)_

### Key derivation and password hashing

```cpp
#include <qb/io/crypto.h>
using C = qb::crypto;

// Password hashing (Argon2id by default) -> self-describing encoded string.
std::string stored = C::hash_password("correct horse battery staple");
bool ok            = C::verify_password("correct horse battery staple", stored);

// Lower-level derivation
auto salt = C::generate_salt(16);
auto dk   = C::derive_key("password", salt, /*key_length*/ 32,
                          C::KdfAlgorithm::Argon2);   // or PBKDF2 / HKDF
```
<!-- src: qb/src/qb/io/crypto.h:762-764 -->

`KdfAlgorithm` is `PBKDF2`, `HKDF`, or `Argon2`; `derive_key` defaults to `Argon2` with `iterations = 10000` (used only by PBKDF2) and a default `Argon2Params`. Dedicated entry points exist for each primitive: `pbkdf2`, `hkdf`, `argon2_kdf`. `Argon2Params` defaults to `t_cost = 3`, `m_cost = 1 << 16` KiB, `parallelism = 1`; `Argon2Variant` is `Argon2d`, `Argon2i`, or `Argon2id`. _(`qb/src/qb/io/crypto.h:425,669-690,724,741,762-764`.)_

> **Argon2 is an optional dependency.** The `QB_HAS_ARGON2` macro is set only when the Argon2 library is found, and that probe runs only when OpenSSL is present (`qb/cmake/qbDependencies.cmake:131-140`). When Argon2 is absent, `hash_password` and Argon2-mode `derive_key` **silently fall back to PBKDF2-HMAC-SHA256** (with a different self-describing hash prefix) rather than failing or warning (`qb/src/qb/io/crypto_advanced.cpp:165-182,444-456`). The stored-hash format therefore depends on how the framework was built: a build with SSL but without the Argon2 library stores PBKDF2 hashes even though the API defaults read as "Argon2id". `verify_password` is gated the same way, so it only validates the format its own build produces — a hash written by one build configuration will not verify against a binary built the other way. Confirm `QB_HAS_ARGON2` if you require Argon2id, and keep the build configuration consistent across any services that share a hash store.

### Asymmetric cryptography

`crypto` provides RSA, ECDSA, EdDSA, and X25519 over PEM-encoded keys, plus hybrid schemes:

- **Key generation:** `generate_rsa_keypair(bits = 2048)`, `generate_ec_keypair(curve = "prime256v1")`, `generate_ed25519_keypair()`, `generate_x25519_keypair()` — each returns a `std::pair<std::string, std::string>` of `{private_pem, public_pem}`.
- **Sign / verify:** `rsa_sign`/`rsa_verify`, `ec_sign`/`ec_verify` (default `DigestAlgorithm::SHA256`), `ed25519_sign`/`ed25519_verify`.
- **Key agreement:** `x25519_key_exchange` — two overloads, one taking raw-byte keys and one taking PEM strings, both returning the shared secret as `std::vector<unsigned char>`. There is **no** `ecdh_derive_secret`: X25519 is the only key agreement qb implements, and an ECDH over a `generate_ec_keypair` curve is not exposed.
- **Hybrid encryption:** `ecies_encrypt`/`ecies_decrypt` (`ECIESMode::STANDARD | AES_GCM | CHACHA20`, default `AES_GCM`). These take **raw-byte keys only** — there is no PEM/`std::string` overload and no `DigestAlgorithm` parameter, so a `generate_ec_keypair` PEM will not fit; use `generate_x25519_keypair_bytes()`. `ecies_encrypt` returns `{ephemeral_public_key, ciphertext}` **in that order**, while `ecies_decrypt(ciphertext, ephemeral_public_key, recipient_private_key, …)` takes them the other way round — passing the pair straight through throws `Failed to create public key from raw bytes`. Prefer the AEAD modes: `STANDARD` is AES-256-CBC with no MAC (see the warning on `ECIESMode`).

_(`qb/src/qb/io/crypto.h:594-648,846-901,910-1000`.)_

> **`envelope_encrypt` / `envelope_decrypt` do not exist.** This page used to list them, and so did
> `llm/qb.llm.api.md`, complete with default arguments. Nothing of the sort was ever declared in
> `crypto.h` — a caller gets a compile error, not a link error. What is real is the `EnvelopeFormat`
> enum (`RAW | JSON | BASE64`), which **no function in the tree consumes**; it is a leftover of a
> feature that was never written. For authenticated encryption that binds unencrypted metadata to
> the ciphertext — the job the name suggests — use the pair below instead.

**Authenticated encryption with associated metadata.** `encrypt_with_metadata(plaintext, key, metadata, algorithm = SymmetricAlgorithm::AES_256_GCM)` returns a structured `std::string` carrying IV, AAD and tag; the `metadata` travels in the clear but is authenticated, so tampering with it fails decryption. `decrypt_with_metadata(ciphertext, key, algorithm = SymmetricAlgorithm::AES_256_GCM)` returns `std::optional<std::pair<std::vector<unsigned char>, std::string>>` — `{plaintext, metadata}` on success, and an **empty optional** on any authentication failure, so check it before using either half.

<!-- src: qb/src/qb/io/crypto.h:969-987 -->

```cpp
auto key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
std::vector<unsigned char> plain{'h', 'i'};

std::string sealed = qb::crypto::encrypt_with_metadata(plain, key, "user=42");
if (auto opened = qb::crypto::decrypt_with_metadata(sealed, key)) {
    auto &[data, metadata] = *opened;   // metadata == "user=42", authenticated
}
```

### Constant-time comparison and secure tokens

`constant_time_compare(a, b)` compares two byte vectors without short-circuiting (use it for HMACs and password hashes). `generate_token(payload, key, ttl)` produces an encrypted, authenticated token; `verify_token(token, key)` returns the payload or an empty string on any failure (tampering, malformed input, or expiry). _(`qb/src/qb/io/crypto.h:583,812-825`.)_

```cpp
auto key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);

// ttl is a qb::duration; zero() means the token never expires.
std::string token = qb::crypto::generate_token("session:abc", key, qb::duration{std::chrono::hours(1)});

std::string payload = qb::crypto::verify_token(token, key);  // "" if invalid/expired
```
<!-- src: qb/src/qb/io/crypto.h:777-790 -->

`generate_token` takes its `ttl` as a `qb::duration` (`qb::duration::zero()` disables expiry). The embedded `exp` claim is `duration_cast` to whole seconds and uses wall-clock time (`system_clock`), so sub-second TTL precision is lost and expiry is subject to system clock changes — consistent with the canonical model where expiry is a `wall_time` concept. _(`qb/src/qb/io/crypto_advanced.cpp:235,243-244,249-252`; `docs-overhaul/qb/FACTBOOK.md:481-483`.)_

---

## JSON Web Tokens (`qb::jwt`)

`qb::jwt` implements RFC 7519 over `qb::crypto`, in the same OpenSSL-gated slice. Every entry point is a static member of `class jwt`. _(`qb/src/qb/io/crypto_jwt.h:46`.)_

Supported `Algorithm` values: `HS256/384/512` (HMAC), `RS256/384/512` (RSASSA-PKCS1-v1_5), `ES256/384/512` (ECDSA on P-256/P-384/P-521), and `EdDSA` (Ed25519). _(`qb/src/qb/io/crypto_jwt.h:51-62`.)_

```cpp
#include <qb/io/crypto_jwt.h>
using qb::jwt;

// --- Create with standard claims ---
std::map<std::string, std::string> claims{{"user_id", "12345"}};

jwt::CreateOptions opt;
opt.algorithm = jwt::Algorithm::HS256;
opt.key       = "my-256-bit-secret";

std::string token = jwt::create_token(
    claims,
    /*issuer*/   "test-issuer",
    /*subject*/  "user-12345",
    /*audience*/ "test-audience",
    /*expires_in*/ std::chrono::seconds(3600),
    /*not_before*/ std::chrono::seconds(0),
    /*jti*/      "token-id-123",
    opt);

// --- Verify ---
jwt::VerifyOptions vopt;
vopt.algorithm = jwt::Algorithm::HS256;
vopt.key       = "my-256-bit-secret";

jwt::ValidationResult result = jwt::verify(token, vopt);
if (result.is_valid()) {
    const std::string &uid = result.payload.at("user_id");
} else if (result.error == jwt::ValidationError::TOKEN_EXPIRED) {
    // handle expiry
}
```
<!-- src: qb/tests/io/unit/crypto/crypto-jwt.cpp:91-146 -->

`create_token` takes `expires_in` and `not_before` as `std::chrono::seconds` offsets from "now" (RFC 7519 NumericDate is seconds). `exp` is emitted only when `expires_in.count() > 0` and `nbf` only when `not_before.count() > 0`; passing zero omits the claim. `verify` returns a `ValidationResult` whose `error` is one of `NONE`, `INVALID_FORMAT`, `INVALID_SIGNATURE`, `TOKEN_EXPIRED`, `TOKEN_NOT_ACTIVE`, `INVALID_ISSUER`, `INVALID_AUDIENCE`, `INVALID_SUBJECT`, or `CLAIM_MISMATCH`; `is_valid()` is `error == NONE`. _(`qb/src/qb/io/crypto_jwt.h:67-94,175-192`; `qb/src/qb/io/crypto_jwt.cpp:282`.)_

`VerifyOptions::clock_skew` is a `std::chrono::seconds` tolerance (default `0`) applied to the *current-time* side of the `exp`/`nbf` comparison — not to the token-supplied claim — to absorb clock drift without overflowing on extreme claim values. `verify` accepts `exp`/`nbf` as either a JSON number or a numeric string and fails closed (`INVALID_FORMAT`) on malformed values. `decode(token)` returns the `TokenParts` (header, payload, signature) *without* verification and throws `std::runtime_error` on a malformed token. _(`qb/src/qb/io/crypto_jwt.h:158,194-201`; `qb/src/qb/io/crypto_jwt.cpp:474,478`; `docs-overhaul/qb/FACTBOOK.md:285,486-487`.)_

---

## Compression (`qb::gzip`, `qb::deflate`)

zlib-backed gzip and deflate. Requires `QB_WITH_COMPRESSION` → `QB_HAS_COMPRESSION`. The convenience namespaces `qb::gzip` and `qb::deflate` are `using`-aliases of `qb::compression::gzip` / `qb::compression::deflate`. _(`qb/src/qb/io/compression.h:962-964`.)_

```cpp
#include <qb/io/compression.h>

std::string original = "some data to compress, repeated many times ...";

// One-shot string round trip (level defaults to Z_DEFAULT_COMPRESSION).
std::string compressed   = qb::gzip::compress(original.c_str(), original.size());
std::string decompressed = qb::gzip::uncompress(compressed.c_str(), compressed.size());
// qb::deflate::compress / uncompress are the raw-deflate equivalents.
```
<!-- src: qb/tests/io/unit/compression/compression-codec.cpp:140-163 -->

`compress(const char* data, size_t size, int level = Z_DEFAULT_COMPRESSION)` and `uncompress(const char* data, size_t size)` return a `std::string`. Generic container overloads, `uncompress(Output&, data, size, max = 0)`, accept a `max` output budget: a non-zero `max` rejects inputs that would inflate beyond it (a decompression-bomb guard), throwing `std::runtime_error`. A truncated or incomplete stream also throws `std::runtime_error`. _(`qb/src/qb/io/compression.h:487-563,662-733,838-909`; `qb/tests/io/unit/compression/compression-codec.cpp:182-206`.)_

`qb::gzip::is_compressed(data, size)` heuristically detects a gzip or zlib header. For streaming over large or chunked data, the lower-level `qb::compression` namespace exposes `compress_provider` / `decompress_provider` interfaces with an `operation_hint` (`is_last` / `has_more`) and provider factories. _(`qb/src/qb/io/compression.h:56-247,744-766`.)_

---

## URI parsing (`qb::io::uri`)

RFC 3986 parsing and percent-encoding. Construction runs `parse()` immediately; `is_valid()` reports whether the source parsed into a structurally valid URI. Component accessors return `std::string_view` borrowing from the URI's owned source string. _(`qb/src/qb/io/uri.h:182-575`.)_

```cpp
#include <qb/io/uri.h>

qb::io::uri u{"https://user@www.example.com:8080/a/b?query1=value1&query2=value2#frag"};

if (u.is_valid()) {
    auto scheme = u.scheme();   // "https"  (std::string_view)
    auto host   = u.host();     // "www.example.com"
    auto port   = u.u_port();   // 8080     (uint16_t)
    auto path   = u.path();     // "/a/b"
    auto frag   = u.fragment(); // "frag"

    const std::string &v1 = u.query("query1");  // "value1"
}

// Percent-encoding helpers (static). encode() emits '+' for a space
// (application/x-www-form-urlencoded), and decode() maps '+' back to a space.
std::string enc = qb::io::uri::encode("a b/c");   // "a+b%2Fc"
std::string dec = qb::io::uri::decode(enc);        // "a b/c"
```
<!-- src: qb/tests/io/unit/core/uri-parse.cpp:431-453 -->

`u_port()` parses the port string and returns `0` for a missing, malformed, or out-of-range (`> 65535`) port — it rejects rather than silently truncating (`"99999"` returns `0`, not a wrapped value). `query(name, index = 0)` returns a single decoded value as `std::string const&` (a reference to a static empty string on a miss); `query_or(name, fallback, index = 0)` returns the value **by value** with a custom fallback. `queries()` returns the full `qb::icase_unordered_map<std::vector<std::string>>`, so query keys are case-insensitive and may hold multiple values. `encoded_queries()` returns the raw, undecoded query string. Static helpers `is_valid_scheme`, `is_valid_host`, and `normalize_path` are available for validation and `.`/`..` path resolution. _(`qb/src/qb/io/uri.h:193,476-573`.)_

---

## Executable location and resource resolution (`qb::io::sys`)

Three free functions in `qb::io::sys` (`qb/io/system/file.h`) let a binary locate itself and the resources shipped alongside it, so it runs correctly from any working directory. _(`qb/src/qb/io/system/file.h:359-388`.)_

```cpp
#include <qb/io/system/file.h>

namespace qb::io::sys {

// Absolute path of the running executable, queried from the OS
// (GetModuleFileNameW / /proc/self/exe / _NSGetExecutablePath) — independent
// of argv[0] and the cwd. Empty path if the platform query fails.
std::filesystem::path self_path();

// self_path().parent_path(); empty if self_path() failed.
std::filesystem::path self_dir();

// Resolve a resource path so it is found regardless of the cwd.
std::filesystem::path resolve_resource(const std::filesystem::path &path);

} // namespace qb::io::sys
```
<!-- src: qb/src/qb/io/system/file.h:359-388 -->

`resolve_resource` returns an **absolute** path unchanged. A **relative** path is looked up first against the current working directory (preserving the historical behaviour), then against the executable's own directory (`self_dir()`). The first candidate that exists wins; if neither exists the original path is returned unchanged so diagnostics report exactly what was requested. This is the self-locating-binary pattern: place an asset next to the executable and `resolve_resource("assets/config.json")` finds it whether the process is launched from its own directory or anywhere else. _(`qb/src/qb/io/system/file.cpp:418-438`.)_

The framework routes file paths it loads through `resolve_resource` so the same convenience applies transparently — SSL certificate/key/CA/DH paths (`qb::io::ssl` context helpers), the qbm-http `StaticFilesMiddleware` `root_directory`, and similar resource paths all resolve relative-to-cwd-then-relative-to-binary.

> Filesystem paths in qb-io are `std::filesystem::path`, so Unicode paths are first-class. URL/URI and remote/wire paths deliberately stay `std::string`.

---

## Synchronous file wrappers (`qb::io::sys::file`)

`qb::io::sys::file` (`qb/io/system/file.h`) is a move-only RAII wrapper over a native file descriptor; `file_to_pipe` and `pipe_to_file` bulk-transfer between a file and a `qb::allocator::pipe<char>`. All three take a `std::filesystem::path`. _(`qb/src/qb/io/system/file.h:56-357`.)_

```cpp
#include <qb/io/system/file.h>
#include <qb/system/allocator/pipe.h>

// Direct descriptor access (open/read/write/close). Unicode paths supported.
qb::io::sys::file f(std::filesystem::path{"data.bin"}, O_RDONLY);
if (f.is_open()) { /* f.read(buf, n); */ }

// File -> pipe (the whole file, in chunks).
qb::allocator::pipe<char> buffer;
qb::io::sys::file_to_pipe loader(buffer);
if (loader.open("payload.dat"))
    while (loader.read() > 0 && !loader.eof()) {}

// Pipe -> file (truncating overwrite, O_WRONLY|O_CREAT|O_TRUNC).
qb::io::sys::pipe_to_file writer(buffer);
if (writer.open("out.dat"))
    while (writer.write() > 0 && !writer.eos()) {}
```
<!-- src: qb/src/qb/io/system/file.h -->

`file::open(std::filesystem::path const&, int flags = O_RDWR, int mode = 0644)`, the `file(std::filesystem::path const&, int flags = O_RDWR)` constructor, and `file_to_pipe::open` / `pipe_to_file::open` all take a `std::filesystem::path`. On Windows the path's native (wide) representation is opened with `CreateFileW`, so non-ANSI paths are no longer truncated; the descriptor is also opened with `FILE_SHARE_DELETE` so a file can be unlinked or renamed while held, matching POSIX. `file` is move-only — copy is deleted to prevent a double-close. _(`qb/src/qb/io/system/file.cpp:46-140`.)_

---

## Fixed-capacity string (`qb::string<N>`)

`qb::string<N>` is an inline, `std::array`-backed string with a compile-time maximum capacity `N` (default `30`). It avoids heap allocation, which is faster than `std::string` for small, bounded strings. It is layout-trivial enough to embed directly in events and messages. _(`qb/src/qb/string.h:85-105`.)_

```cpp
#include <qb/string.h>

qb::string<32> name = "actor-42";   // stored inline, no heap allocation
name.append("-worker");
const char *c = name.c_str();
std::size_t n = name.size();        // current length
std::size_t cap = name.capacity();  // == 32 (compile-time max)
```
<!-- src: qb/src/qb/string.h -->

`size()` (alias `length()`) is the current length; `capacity()` and `max_size()` both return the compile-time `N`. Assigning or appending past `N` truncates to `N` rather than reallocating. The internal length field uses the smallest unsigned integer type that can hold `N + 1` (`uint8_t`/`uint16_t`/`size_t`), so small fixed strings stay compact. `qb::string<N>` converts implicitly to both `std::string` and `std::string_view`. _(`qb/src/qb/string.h:90,310-323,509-560`.)_

---

## Flat hash maps and case-insensitive maps

`qb::unordered_map` / `qb::unordered_set` are aliases to high-performance flat hash tables (the `ska` open-addressing implementation), which favor cache locality over the node-based `std::unordered_map`. `qb::unordered_flat_map` / `qb::unordered_flat_set` name the `ska::flat_hash_map` / `ska::flat_hash_set` variant directly. _(`qb/src/qb/system/container/unordered_map.h:39-91`.)_

```cpp
#include <qb/system/container/unordered_map.h>

qb::unordered_map<std::string, int> counts;
counts["alpha"] = 1;

// Case-insensitive string-keyed map: keys are ASCII-lowercased before every op.
qb::icase_unordered_map<int> headers;
headers["Content-Length"] = 42;
int v = headers["content-length"];  // 42 — same entry
```
<!-- src: qb/src/qb/system/container/unordered_map.h -->

`qb::icase_unordered_map<Value>` and the ordered `qb::icase_map<Value>` wrap an underlying map and ASCII-lowercase string keys (via the default `qb::string_to_lower` trait) before every operation — useful for HTTP headers and other case-insensitive lookups. This is exactly the type the URI parser uses for query parameters. _(`qb/src/qb/system/container/unordered_map.h:98-413`.)_

> In a debug build (`NDEBUG` undefined), `qb::unordered_map` falls back to `std::unordered_map` to keep iterator-debugging and sanitizers happy; release builds use the `ska` implementation. _(`qb/src/qb/system/container/unordered_map.h:58-91`.)_

---

## UUID (`qb::uuid`)

`qb::uuid` is a type alias for `uuids::uuid` from the vendored `stduuid` library — a 128-bit RFC 4122 identifier. `qb::generate_random_uuid()` produces a version-4 (random) UUID. _(`qb/src/qb/uuid.h`.)_

```cpp
#include <qb/uuid.h>

qb::uuid id = qb::generate_random_uuid();   // version-4 UUID
```
<!-- src: qb/src/qb/uuid.h -->

UUIDs serialize to and from JSON via the `uuids::to_json` / `uuids::from_json` adapters declared in `qb/json.h`. _(`qb/src/qb/json.h:301-311`.)_

---

## JSON (`qb::json`, `qb::jsonb`)

`qb/json.h` integrates the vendored `nlohmann/json` library. `qb::json` is an alias for `nlohmann::json` (brought in via `using namespace nlohmann` inside `qb`), so the full `nlohmann` API is available. `qb::jsonb` is a distinct wrapper struct around `nlohmann::json` (binary-JSON intent) that forwards most operations to an internal `data` member but is a separate type that can be specialized differently in serialization contexts. _(`qb/src/qb/json.h:94-105,282-290`.)_

```cpp
#include <qb/json.h>

qb::json msg = {
    {"type", "ping"},
    {"seq",  7},
    {"tags", {"a", "b"}},
};

std::string wire = msg.dump();              // serialize
qb::json    back = qb::json::parse(wire);   // parse
int         seq  = back["seq"].get<int>();
```
<!-- src: qb/src/qb/json.h -->

A `qb::allocator::pipe<char>::put<qb::json>` specialization lets JSON be written directly into a pipe buffer, and the JSON wire protocol (`qb::protocol::json`) frames JSON over NUL-terminated messages with a nesting-depth guard. See [Protocols](./protocols.md). _(`qb/src/qb/json.h:284-289`; `docs-overhaul/qb/FACTBOOK.md:118`.)_

---

## Endian helpers (`qb::endian`)

`qb/system/endian.h` builds on C++20 `std::endian` and C++23 `std::byteswap`. Detection is `consteval` (compile-time) and conversion is `constexpr`. _(`qb/src/qb/system/endian.h:37-167`.)_

```cpp
#include <qb/system/endian.h>

static_assert(qb::endian::native_order() == qb::endian::order::little ||
              qb::endian::native_order() == qb::endian::order::big);

uint32_t host = 0x01020304;
uint32_t be   = qb::endian::to_big_endian(host);      // for wire formats
uint32_t back = qb::endian::from_big_endian(be);       // == host
uint32_t sw   = qb::endian::byteswap(host);            // unconditional swap
```
<!-- src: qb/src/qb/system/endian.h -->

`native_order()`, `is_little_endian()`, and `is_big_endian()` are `consteval`. `byteswap<T>` accepts arithmetic and enum types. The `to_big_endian` / `from_big_endian` / `to_little_endian` / `from_little_endian` helpers are no-ops when the requested order already matches the host and a `byteswap` otherwise — the standard idiom for reading and writing fixed-endian wire formats. _(`qb/src/qb/system/endian.h:48-167`.)_

---

## Pitfalls

- **Empty result means authentication failure, not empty plaintext.** `crypto::decrypt`, the AEAD helpers, and `verify_token` return an empty vector/string when the authentication tag or token check fails. Always branch on emptiness before using the result. _(`docs-overhaul/qb/FACTBOOK.md:483-484`.)_
- **`generate_random_string` is not cryptographic.** It is a Mersenne Twister. Use `generate_secure_random_string`, `generate_random_bytes`, or `generate_salt` for any security-sensitive value. _(`qb/src/qb/io/crypto.h:175-242`.)_
- **JWT and token TTLs are wall-clock seconds.** `create_token`'s `expires_in`/`not_before` and `generate_token`'s `ttl` resolve to whole-second `exp`/`nbf` claims evaluated against `system_clock`. Sub-second precision is lost and expiry follows wall-clock changes — by design, since expiry is a `wall_time` concept. _(`docs-overhaul/qb/FACTBOOK.md:481-485`.)_
- **`u_port()` returns `0` on failure.** A missing, malformed, or out-of-range port yields `0`, which is indistinguishable from an explicit `:0`. Check `is_valid()` and the raw `port()` string if `0` is meaningful in your scheme. _(`qb/src/qb/io/uri.h:476-487`.)_
- **`qb::string<N>` truncates silently.** Assigning past `N` characters truncates rather than throwing or reallocating. Size the capacity to the worst case for your data. _(`qb/src/qb/string.h:509-560`.)_
- **URI accessors borrow from the URI.** `scheme()`, `host()`, `path()`, etc. return `std::string_view` into the URI's owned source string; do not let them outlive the `uri` object. _(`qb/src/qb/io/uri.h:185-552`.)_
- **Crypto and compression headers `#error` without their dependency.** Including `crypto.h`/`crypto_jwt.h` without `QB_HAS_SSL`, or `compression.h` without `QB_HAS_COMPRESSION`, fails to compile. Guard optional features behind those defines. _(`qb/src/qb/io/crypto.h:33-34`, `qb/src/qb/io/compression.h:37-38`.)_

---

## See also

- [qb-io module overview](./README.md) — the async I/O library these utilities ship with.
- [Async I/O system](./async_system.md) — where `qb::duration` parameterizes timers, timeouts, and `sleep`.
- [Protocols](./protocols.md) — JSON, text, and binary framings, including the depth-guarded JSON protocol.
- [SSL/TLS transport](./ssl_transport.md) — the other consumer of the OpenSSL-gated slice.
- [Lock-free primitives](./../7_reference/lockfree_primitives.md) — `SpinLock` and the SPSC/MPSC ring buffers.
