@page qb_io_utilities_md QB-IO: Essential Utilities & Helpers
@brief Explore the rich set of utility classes and functions in `qb-io` for common system programming tasks.

# QB-IO: Essential Utilities & Helpers

Beyond its core asynchronous I/O and networking capabilities, `qb-io` offers a robust collection of utilities designed to simplify common system programming tasks, enhance performance, and ensure cross-platform compatibility. These utilities cover areas from data manipulation and cryptography to time management and system information.

## 1. URI Parsing & Manipulation (`qb::io::uri`)

*   **Header:** `qb/io/uri.h`
*   **Purpose:** Provides comprehensive, RFC 3986 compliant parsing and manipulation of Uniform Resource Identifiers (URIs).
*   **Key Features:**
    *   Parses schemes, authority (userinfo, host, port), path, query parameters, and fragments.
    *   Handles IPv4 and IPv6 host representations.
    *   Methods like `scheme()`, `host()`, `u_port()` (for numeric port), `path()`, `queries()` (map of decoded query params), `fragment()`.
    *   Static `uri::encode(string_view)` and `uri::decode(string_view)` for percent-encoding/decoding.
*   **Example:**
    ```cpp
    #include <qb/io/uri.h>
    // ...
    qb::io::uri my_uri("http://user@example.com:8080/path?query=val#section");
    std::cout << "Scheme: " << my_uri.scheme() << ", Host: " << my_uri.host() << std::endl;
    // my_uri.query("query") would return "val"
    ```

## 2. Cryptography (`qb::crypto`, `qb::jwt`)

*   **Requires:** `QB_IO_WITH_SSL=ON` CMake option and linked OpenSSL library.
*   **Headers:** `qb/io/crypto.h`, `qb/io/crypto_jwt.h`
*   **Purpose:** A powerful suite of cryptographic functions.
*   **Key Features:**
    *   **Hashing:** MD5, SHA-1, SHA-2 family (SHA256, SHA384, SHA512), BLAKE2, HMAC variants.
        *   Example: `std::string hash = qb::crypto::sha256("input string");`
    *   **Encoding:** Base64, Base64URL, Hexadecimal.
        *   Example: `std::string b64 = qb::crypto::base64::encode(data_vec);`
    *   **Key Derivation:** PBKDF2, HKDF, Argon2 (Argon2id, Argon2i, Argon2d).
    *   **Symmetric Encryption:** AES (CBC, GCM modes with 128, 192, 256-bit keys), ChaCha20-Poly1305.
    *   **Asymmetric Cryptography:** RSA, ECDSA (P-256, P-384, P-521), EdDSA (Ed25519 for signing), X25519 (for key exchange), ECIES (hybrid encryption).
    *   **JSON Web Tokens (JWT):** Full support for creating, signing (HS*, RS*, ES*, EdDSA algorithms), and verifying JWTs with claim validation.
    *   **Secure Utilities:** Password hashing (`crypto::hash_password`), secure random data/salt generation, constant-time comparison.
*   **(Reference:** Extensive examples in `qb/source/io/tests/system/test-crypto*.cpp` files.**)

## 3. Data Compression (`qb::compression`, `qb::gzip`, `qb::deflate`)

*   **Requires:** `QB_IO_WITH_ZLIB=ON` CMake option and linked Zlib library.
*   **Header:** `qb/io/compression.h`
*   **Purpose:** Efficient data compression and decompression using zlib.
*   **Key Features:**
    *   Supports **Gzip** and **Deflate** algorithms.
    *   Simple functions for direct in-memory operations:
        ```cpp
        #include <qb/io/compression.h>
        // ...
        std::string original = "some data to compress";
        std::string compressed = qb::gzip::compress(original.data(), original.size());
        std::string decompressed = qb::gzip::uncompress(compressed.data(), compressed.size());
        ```
    *   Streaming API (`compress_provider`, `decompress_provider`) for handling large data sets or stream-based compression/decompression.
*   **(Reference:** See `qb/source/io/tests/system/test-compression*.cpp` for usage.**)

## 4. High-Precision Time (`qb::TimePoint`, `qb::Duration`)

*   **Header:** `qb/system/timestamp.h`
*   **Purpose:** Platform-independent, nanosecond-precision time points and durations.
*   **Key Features:**
    *   **`qb::Duration`:** Represents time spans. Create with factory methods (`Duration::from_seconds(5)`) or literals (`5_s`, `100_ms`). Supports arithmetic and unit conversions (e.g., `d.milliseconds()`, `d.seconds_float()`).
    *   **`qb::TimePoint`:** Represents specific moments in time. Create with `TimePoint::now()`, `TimePoint::from_iso8601("...")`. Supports arithmetic with `Duration`. Format to string (`tp.to_iso8601()`, `tp.format("%Y-%m-%d")`).
    *   **Specialized TimePoints:** `UtcTimePoint`, `LocalTimePoint`, `HighResTimePoint` (monotonic), `TscTimePoint` (CPU counter based).
    *   **Utilities:** `ScopedTimer` and `LogTimer` for easy performance measurement of code blocks.
*   **(Reference:** See `qb/source/core/tests/unit/test-timestamp.cpp`. Aliases like `qb::Timestamp` and `qb::Timespan` exist for backward compatibility.**)

## 5. System Information

*   **CPU Details (`qb::CPU`):**
    *   **Header:** `qb/system/cpu.h`
    *   Static methods: `CPU::Architecture()`, `CPU::LogicalCores()`, `CPU::PhysicalCores()`, `CPU::ClockSpeed()`, `CPU::HyperThreading()`.
*   **Endianness (`qb::endian`):**
    *   **Header:** `qb/system/endian.h`
    *   Utilities: `endian::native_order()`, `endian::is_little_endian()`, `endian::byteswap(value)`, `to_big_endian(value)`, `from_little_endian(value)`.

## 6. High-Performance Containers & Allocators

`qb-io` includes several container and allocator types optimized for performance and specific use cases, often avoiding heap allocations or improving cache locality.

*   **`qb::allocator::pipe<T>`:** (`qb/system/allocator/pipe.h`)
    *   An extremely efficient, dynamically resizable buffer. It's the backbone of `qb-io`'s stream input/output buffering. Supports `allocate_back()`, `free_front()`, `reorder()`, etc. The `pipe<char>` specialization offers convenient string-like `put()` and `operator<<` methods.
*   **`qb::string<N>`:** (`qb/string.h`)
    *   A fixed-capacity string (max `N` characters) stored on the stack within a `std::array`. Significantly faster than `std::string` for small strings as it avoids heap allocations.
*   **`qb::unordered_map` / `qb::unordered_set`:** (`qb/system/container/unordered_map.h`, `unordered_set.h`)
    *   Type aliases that point to high-performance hash table implementations (specifically `ska::flat_hash_map` and `ska::flat_hash_set` in release builds) which often outperform `std::unordered_map/set` due to better cache locality.
*   **`qb::icase_unordered_map`:** (`qb/system/container/unordered_map.h`)
    *   A case-insensitive version of `qb::unordered_map` specifically for string keys.

## 7. Lock-Free Primitives (`qb::lockfree`)

*   **Headers:** `qb/system/lockfree/*.h`
*   **Purpose:** Provides low-level, lock-free data structures primarily for internal framework use, crucial for performance in concurrent scenarios.
*   **Components:**
    *   `SpinLock`: A lightweight spinlock for very short critical sections.
    *   `spsc::ringbuffer`: Single-Producer, Single-Consumer lock-free queue.
    *   `mpsc::ringbuffer`: Multiple-Producer, Single-Consumer lock-free queue (used for inter-core actor communication).
*   **(See:** `[Reference: Lock-Free Primitives](./../7_reference/lockfree_primitives.md)` for more details.**)

## 8. UUID Generation (`qb::uuid`)

*   **Header:** `qb/uuid.h`
*   **Purpose:** Provides RFC 4122 compliant Universally Unique Identifiers.
*   **Usage:** `qb::uuid my_id = qb::generate_random_uuid();` (Relies on an embedded `stduuid` library version).

These utilities collectively make `qb-io` a comprehensive library for building robust, high-performance C++ applications, extending well beyond basic asynchronous I/O.

**(Next:** Explore specific module documentation, like `[QB-Core Module Overview](./../4_qb_core/README.md)` or dive into the `[Developer Guides](./../6_guides/README.md)` for practical application patterns.**) 