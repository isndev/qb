/**
 * @file qb/io/tcp/ssl/context.h
 * @brief World-class, value-semantic TLS context for qb-io secure sockets.
 *
 * `qb::io::ssl::Context` owns and configures an OpenSSL `SSL_CTX` behind a safe, RAII,
 * reference-counted value type. Copying a `Context` shares the SAME underlying `SSL_CTX`
 * (the OpenSSL reference count is managed internally), so a server or a client pool can hand
 * copies to many sockets without ever touching `SSL_CTX_free` — the double-free / leak class
 * that plagues hand-rolled OpenSSL ownership becomes structurally impossible.
 *
 * The type is **secure by default** (`client()` pins TLS 1.2+, loads the system trust store,
 * and verifies the peer) and **fails closed** (a construction/config error yields a falsy
 * `Context` whose `error()` explains why; it never silently degrades to an insecure context).
 *
 * Typed callbacks (`on_verify`, `on_keylog`, `on_sni`) replace OpenSSL's raw C function
 * pointers + `void*` args: the `std::function`s live on state attached to the `SSL_CTX` itself
 * (via `SSL_CTX` ex-data), so they are reachable from every `SSL` minted from the context and
 * die exactly when the context does — no dangling `void*` arg, no stable-address hacks.
 *
 * Three escape hatches keep the power user in control: `Context::adopt`/`Context::share` wrap a
 * raw `SSL_CTX*` with explicit ownership semantics, `Context::native()` borrows the raw handle
 * for any OpenSSL call this API does not wrap, and the whole `qb::io::ssl::` free-function layer
 * plus `socket::init(SSL*)` remain available for fully hand-built configurations.
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
 * @ingroup SSL
 */

#ifndef QB_IO_TCP_SSL_CONTEXT_H_
#define QB_IO_TCP_SSL_CONTEXT_H_
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <openssl/ssl.h>
#include <qb/utility/build_macros.h>

namespace qb::io::ssl {

struct Certificate; ///< Full definition in <qb/io/tcp/ssl/socket.h>.

/**
 * @enum TlsVersion
 * @ingroup SSL
 * @brief Concrete, pinnable TLS protocol versions for `Context::min_version` / `max_version`.
 */
enum class TlsVersion {
    v1_2, ///< TLS 1.2 (`TLS1_2_VERSION`).
    v1_3  ///< TLS 1.3 (`TLS1_3_VERSION`).
};

/**
 * @enum VerifyMode
 * @ingroup SSL
 * @brief Peer-certificate verification policy for `Context::verify`.
 */
enum class VerifyMode {
    none,        ///< No verification (`SSL_VERIFY_NONE`). Insecure — trusted channels only.
    peer,        ///< Verify the peer's certificate chain (`SSL_VERIFY_PEER`). Client default.
    peer_require ///< Verify AND fail if the peer sends no certificate (mTLS: `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`).
};

/**
 * @class VerifyContext
 * @ingroup SSL
 * @brief Typed, borrowing view over OpenSSL's `X509_STORE_CTX` passed to an `on_verify` callback.
 * @details Lets a custom verification callback inspect the failing certificate, error code and
 *          chain depth without touching the raw X.509 API. `native()` is the escape hatch.
 */
class QB_API VerifyContext {
    X509_STORE_CTX *_raw;

public:
    /** @brief Wrap the raw `X509_STORE_CTX` OpenSSL hands to the verify callback. */
    explicit VerifyContext(X509_STORE_CTX *raw) noexcept
        : _raw(raw) {}

    /** @brief Depth of the certificate currently being verified (0 = leaf). */
    [[nodiscard]] int depth() const noexcept;
    /** @brief Current verification error code (`X509_V_ERR_*`; `X509_V_OK` if none). */
    [[nodiscard]] int error() const noexcept;
    /** @brief Human-readable string for the current verification error. */
    [[nodiscard]] std::string error_string() const;
    /** @brief The certificate currently being verified, as a `qb::io::ssl::Certificate`. */
    [[nodiscard]] Certificate current_certificate() const;
    /** @brief Borrow the raw `X509_STORE_CTX` for any OpenSSL call this view does not wrap. */
    [[nodiscard]] X509_STORE_CTX *
    native() const noexcept {
        return _raw;
    }
};

/**
 * @class Context
 * @ingroup SSL
 * @brief RAII, value-semantic, reference-counted, secure-by-default TLS context.
 *
 * Build one with `client()` / `server()` (or wrap a raw ctx with `adopt()` / `share()`),
 * configure it fluently, then hand it to a `tcp::ssl::socket` / `tcp::ssl::listener`. Copies
 * share the same `SSL_CTX`; the context is freed exactly once, when the last copy and the last
 * `SSL` minted from it are gone. There is no user-visible `SSL_CTX_free`.
 *
 * @code
 * // Server: one shared context, safe across every accepted connection.
 * auto ctx = ssl::Context::server("cert.pem", "key.pem").alpn({"h2", "http/1.1"});
 * tcp::ssl::listener lis{ctx};
 *
 * // Client: secure by default.
 * tcp::ssl::socket s{ssl::Context::client().alpn({"h2"})};
 * s.sni("example.com").connect(ep);
 * @endcode
 */
class QB_API Context {
public:
    /** @brief Construct an empty (falsy) context. `ok()` is false; `native()` is null. */
    Context() noexcept = default;

    // --- factories ---

    /**
     * @brief Secure-by-default client context: TLS 1.2+, system trust store, `VerifyMode::peer`.
     * @return A configured client `Context` (falsy on OpenSSL allocation failure).
     */
    static Context client();

    /**
     * @brief Server context loading a PEM certificate + private key (checked). TLS 1.2+, `VerifyMode::none`.
     * @param cert Path to the PEM certificate (resolved against cwd then the executable dir).
     * @param key  Path to the PEM private key.
     * @return A configured server `Context`, or a falsy one (with `error()`) if loading/validation fails.
     */
    static Context server(std::filesystem::path cert, std::filesystem::path key);

    /**
     * @brief ESCAPE HATCH (TRANSFER): wrap an already-built raw `SSL_CTX`, taking over the caller's
     *        single reference (no up-ref). The caller must NOT `SSL_CTX_free` it afterwards.
     * @param raw A raw `SSL_CTX*` whose ownership passes to the returned `Context`.
     * @note This matches the historical `listener::init(SSL_CTX*)` / socket ownership contract. A raw
     *       context with no qb state attached is treated as **client-role** by a later `alpn()` — for a
     *       server context, set ALPN before wrapping (or via the raw `qb::io::ssl::` helpers / `native()`).
     */
    static Context adopt(SSL_CTX *raw) noexcept;

    /**
     * @brief ESCAPE HATCH (SHARE): wrap a raw `SSL_CTX`, taking a NEW reference (`SSL_CTX_up_ref`).
     *        The caller keeps and must still free their own reference.
     * @param raw A raw `SSL_CTX*` co-owned with the caller.
     */
    static Context share(SSL_CTX *raw) noexcept;

    Context(const Context &) noexcept            = default; ///< Share the same `SSL_CTX` (cheap).
    Context(Context &&) noexcept                 = default;
    Context &operator=(const Context &) noexcept = default;
    Context &operator=(Context &&) noexcept      = default;
    ~Context()                                   = default; ///< Drops one reference (RAII).

    // --- fluent configuration (chainable; a no-op once the context is in an error state) ---

    Context &min_version(TlsVersion v);                                       ///< Minimum negotiated TLS version (default v1_2).
    Context &max_version(TlsVersion v);                                       ///< Maximum negotiated TLS version.
    Context &verify(VerifyMode mode);                                         ///< Peer verification policy.
    Context &trust(std::filesystem::path ca_file_or_dir);                     ///< Add a CA file or directory to the trust store.
    Context &trust_system();                                                  ///< (Re)load the OS default trust store (client default).
    Context &identity(std::filesystem::path cert, std::filesystem::path key); ///< This endpoint's cert+key (client mTLS / extra server cert).
    Context &
    alpn(std::vector<std::string> protocols); ///< ALPN: client offer / server accept-preference. Empty list = no-op (leaves ALPN unconfigured).
    Context &ciphers(std::string tls12_list); ///< TLS <= 1.2 cipher list (OpenSSL format).
    Context &ciphersuites(std::string tls13_list);          ///< TLS 1.3 ciphersuites (OpenSSL format).
    Context &curves(std::string groups);                    ///< Supported ECDH groups, e.g. "X25519:P-256".
    Context &dh_params(std::filesystem::path pem);          ///< Server DH parameters (PEM) for DHE suites.
    Context &session_cache(std::size_t entries);            ///< Server session cache size (0 disables).
    Context &session_timeout(std::chrono::seconds timeout); ///< Session lifetime.

    // --- typed callbacks (no raw C function pointer, no void* arg) ---

    /** @brief Receive TLS key material lines (SSLKEYLOGFILE format) for debugging/inspection. */
    Context &on_keylog(std::function<void(std::string_view line)> cb);
    /** @brief Custom certificate verification: return true to accept, false to reject the current step. */
    Context &on_verify(std::function<bool(bool preverified, VerifyContext &ctx)> cb);
    /** @brief Server SNI router: map the client's requested host name to the `Context` to switch to. */
    Context &on_sni(std::function<Context(std::string_view servername)> cb);

    // --- introspection / escape hatch ---

    /** @brief Whether the context is usable (has an `SSL_CTX` and no recorded config error). */
    [[nodiscard]] bool ok() const noexcept;
    /** @brief The first configuration error recorded, or an empty string if `ok()`. */
    [[nodiscard]] std::string error() const;
    /** @brief `ok()`. Lets a `Context` be tested directly: `if (ctx) { ... }`. */
    [[nodiscard]] explicit
    operator bool() const noexcept {
        return ok();
    }
    /** @brief ESCAPE HATCH: borrow the raw `SSL_CTX` (null if falsy). Ownership stays with the `Context`. */
    [[nodiscard]] SSL_CTX *native() const noexcept;

private:
    explicit Context(std::shared_ptr<SSL_CTX> ctx) noexcept;
    /** @brief True when there is an `SSL_CTX` and no error yet — the gate for the fluent setters. */
    [[nodiscard]] bool usable() const noexcept;
    /** @brief Record the first configuration error (subsequent setters become no-ops). */
    void fail(std::string message) noexcept;

    std::shared_ptr<SSL_CTX> _ctx;   ///< Owning, shared handle (deleter = `SSL_CTX_free`).
    std::string              _error; ///< First recorded config error; empty means healthy.
};

} // namespace qb::io::ssl

#endif // QB_IO_TCP_SSL_CONTEXT_H_
