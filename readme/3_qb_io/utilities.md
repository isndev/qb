# QB-IO: Utilities

Beyond the core async system, transports, and protocols, `qb-io` provides a rich set of utilities for common system programming tasks.

## URI Parsing (`qb::io::uri`)

*   **Header:** `qb/include/qb/io/uri.h`
*   **Purpose:** Provides RFC 3986 compliant URI parsing and manipulation.
*   **How to Use:**
    *   Construct from a string: `qb::io::uri my_uri("scheme://user:pass@host:123/path?q=1#frag");`
    *   Access components: `my_uri.scheme()`, `my_uri.host()`, `my_uri.port()` (string_view), `my_uri.u_port()` (uint16_t), `my_uri.path()`, `my_uri.encoded_queries()`, `my_uri.fragment()`.
    *   Access decoded query parameters: `my_uri.queries()` returns a map; `my_uri.query("key")` gets the first value for a key.
    *   Check address family: `my_uri.af()` (e.g., `AF_INET`, `AF_INET6`).
    *   Static methods `uri::encode()` and `uri::decode()` handle percent-encoding.

## Cryptography (`qb::crypto`, `qb::jwt`)

*   **Requires:** `QB_IO_WITH_SSL=ON` build option.
*   **Headers:** `qb/include/qb/io/crypto.h`, `qb/include/qb/io/crypto_jwt.h`
*   **Purpose:** A comprehensive suite of cryptographic functions based on OpenSSL.
*   **How to Use (Examples):**
    *   **Hashing:** `std::string hash = qb::crypto::sha256("my data");`
    *   **Base64:** `std::string encoded = qb::crypto::base64::encode(input);` `std::string decoded = qb::crypto::base64::decode(encoded);`
    *   **Symmetric Encryption (AES-GCM):**
        ```cpp
        auto key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
        auto iv = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
        std::vector<unsigned char> plaintext = ...;
        std::vector<unsigned char> ciphertext = qb::crypto::encrypt(plaintext, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
        std::vector<unsigned char> decrypted = qb::crypto::decrypt(ciphertext, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
        ```
    *   **Password Hashing:** `std::string hash = qb::crypto::hash_password("password");` `bool ok = qb::crypto::verify_password("password", hash);`
    *   **JWT:**
        ```cpp
        qb::jwt::CreateOptions create_opts;
        create_opts.algorithm = qb::jwt::Algorithm::HS256;
        create_opts.key = "your-secret-key";
        std::map<std::string, std::string> payload = {{"user_id", "123"}, {"role", "admin"}};
        std::string token = qb::jwt::create_token(payload, "my_issuer", "user123", "my_audience", std::chrono::hours(1), {}, "jti1", create_opts);

        qb::jwt::VerifyOptions verify_opts;
        verify_opts.algorithm = qb::jwt::Algorithm::HS256;
        verify_opts.key = "your-secret-key";
        verify_opts.verify_issuer = true; verify_opts.issuer = "my_issuer";
        auto result = qb::jwt::verify(token, verify_opts);
        if (result.is_valid()) { /* Use result.payload */ }
        ```
*   **Features:** Covers hashing (MD5, SHA*, HMAC), key derivation (PBKDF2, HKDF, Argon2), encoding (Base64, Hex), symmetric (AES, ChaCha20) and asymmetric (RSA, EC, Ed/X25519) crypto, JWT, secure tokens, password hashing.

**(Ref:** `test-crypto*.cpp` files for detailed usage examples.**)

## Compression (`qb::compression`, `qb::gzip`, `qb::deflate`)

*   **Requires:** `QB_IO_WITH_ZLIB=ON` build option.
*   **Header:** `qb/include/qb/io/compression.h`
*   **Purpose:** Data compression/decompression using zlib.
*   **How to Use:**
    *   Simple functions: `qb::gzip::compress(data, size)`, `qb::gzip::uncompress(data, size)` (similarly for `qb::deflate`). These work with `std::string`, `std::vector`, or `qb::allocator::pipe`.
    *   Streaming API: For large data, use `compress_provider`/`decompress_provider` via factories (`qb::compression::builtin::make_*compressor`).

```cpp
#include <qb/io/compression.h>
#include <string>

std::string original_data = "...";
std::string compressed = qb::gzip::compress(original_data.data(), original_data.size());
std::string decompressed = qb::gzip::uncompress(compressed.data(), compressed.size());
```
**(Ref:** `test-compression*.cpp`**)

## High-Precision Time (`qb::Timestamp`, `qb::Duration`)

*   **Header:** `qb/include/qb/system/timestamp.h`
*   **Purpose:** Platform-independent time points and durations with nanosecond precision.
*   **How to Use:**
    *   `qb::Duration`: Create using factory methods (`Duration::from_seconds(5)`) or literals (`5_s`). Perform arithmetic (`+`, `-`, `*`, `/`). Access units (`d.seconds()`, `d.milliseconds_float()`).
    *   `qb::TimePoint`: Create using factory methods (`TimePoint::from_seconds(...)`, `TimePoint::now()`). Perform arithmetic with `Duration` (`tp + 5_s`). Format (`tp.to_iso8601()`, `tp.format("%Y-%m-%d")`). Parse (`TimePoint::parse(...)`, `TimePoint::from_iso8601(...)`).
    *   Specialized types: `UtcTimePoint`, `LocalTimePoint`, `HighResTimePoint` (based on steady clock), `TscTimePoint` (CPU counter).
    *   Timers: `ScopedTimer`, `LogTimer` for measuring execution time.

**(Ref:** `test-timestamp.cpp`**)

## System Information

*   **CPU (`qb::CPU`):** (`qb/include/qb/system/cpu.h`) Static methods `CPU::Architecture()`, `CPU::LogicalCores()`, `CPU::PhysicalCores()`, `CPU::ClockSpeed()`, `CPU::HyperThreading()`.
*   **Endianness (`qb::endian`):** (`qb/include/qb/system/endian.h`) `endian::native_order()`, `endian::is_little_endian()`, `endian::byteswap(value)`, `to/from_big/little_endian(value)`.

## High-Performance Containers & Allocators

*   **`qb::allocator::pipe<T>`:** (`qb/include/qb/system/allocator/pipe.h`) Efficient, resizable buffer optimized for stream-like access (used heavily in `qb-io` streams). Provides `allocate_back()`, `allocate()`, `free_front()`, `free_back()`, `begin()`, `end()`, `size()`, `clear()`, `reorder()`. Specialization `pipe<char>` has string-like `put`/`<<` operators.
*   **`qb::string<N>`:** (`qb/include/qb/string.h`) Fixed-capacity string stored in `std::array<char, N+1>`. Faster for small strings than `std::string` due to no heap allocation.
*   **`qb::unordered_map/set`:** (`qb/system/container/*.h`) Aliases for high-performance hash containers (`ska::flat_hash_map/set` in release builds) offering better cache locality.
*   **`qb::icase_unordered_map`:** Case-insensitive version of `qb::unordered_map` for string keys.
*   **Lock-Free (`qb::lockfree`):** (`qb/system/lockfree/*.h`) Includes `SpinLock`, SPSC/MPSC queues (`spsc::ringbuffer`, `mpsc::ringbuffer`). Primarily for internal framework use (e.g., inter-core mailboxes). See `[Reference: Lock-Free Primitives](./../7_reference/lockfree_primitives.md)`.

## UUID (`qb::uuid`)

*   **Header:** `qb/include/qb/uuid.h`
*   **Purpose:** Represents RFC 4122 Universally Unique Identifiers.
*   **How to Use:** `qb::uuid my_id = qb::generate_random_uuid();` (Uses underlying `stduuid` library). 