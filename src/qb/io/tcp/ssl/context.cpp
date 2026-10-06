/**
 * @file qb/io/tcp/ssl/context.cpp
 * @brief Implementation of the value-semantic `qb::io::ssl::Context` TLS context.
 *
 * Ownership is a `std::shared_ptr<SSL_CTX>` (deleter `SSL_CTX_free`): copies share one context,
 * freed exactly once when the last copy and the last minted `SSL` are gone. The typed callbacks
 * and the server ALPN wire buffer live on state attached to the `SSL_CTX` via ex-data, so they
 * are reachable from every `SSL` (`SSL_get_SSL_CTX` -> ex-data) and are destroyed with the
 * context — no dangling `void*` arg, no stable-address hacks.
 *
 * NOTE: every file-local helper here is `ctx_`-prefixed because qb-io is a unity build
 * (this file is `#include`d into io.cpp alongside socket.cpp / listener.cpp, which share one
 * anonymous namespace) — the prefix prevents ODR/redefinition clashes with their helpers.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <qb/io/tcp/ssl/context.h>
#include <qb/io/tcp/ssl/socket.h> // qb::io::ssl::Certificate (for VerifyContext::current_certificate)
#include <qb/io/system/file.h>    // qb::io::sys::resolve_resource
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <cstring>
#include <system_error>
#include <utility>

namespace qb::io::ssl {

namespace {

// The reason OpenSSL queued for the call that just failed, appended to `what` -- "SSL_CTX_use_PrivateKey_file
// failed for key.pem: key values mismatch" says what a renewal got wrong, where "failed for key.pem" alone sends
// the reader to check a file that is there -- and the thread's error queue emptied, so the entries of a refused
// configuration are not left behind on the thread that goes on serving.
std::string
ctx_with_reason(std::string what) {
    const unsigned long code = ERR_peek_last_error();
    if (const char *reason = code != 0 ? ERR_reason_error_string(code) : nullptr)
        what.append(": ").append(reason);
    ERR_clear_error();
    return what;
}

// ---------------------------------------------------------------------------
// Per-SSL_CTX state, attached via ex-data. Holds the typed callbacks and the
// server ALPN wire buffer at a stable heap address reachable from any SSL.
// ---------------------------------------------------------------------------
struct ctx_state {
    std::function<void(std::string_view)>                       keylog;
    std::function<bool(bool, VerifyContext &)>                  verify;
    std::function<Context(std::string_view)>                    sni;
    std::function<std::vector<unsigned char>(std::string_view)> ocsp_staple; ///< Server: the response to staple.
    std::function<bool(OcspContext &)>                          ocsp_check;  ///< Client: the stapled response judged.
    std::vector<unsigned char>                                  alpn_wire;   ///< Server accept-list (length-prefixed).
    bool                                                        is_server = false;
};

// OpenSSL calls this when an SSL_CTX carrying our ex-data slot is finally freed.
void
ctx_state_free(void *, void *ptr, CRYPTO_EX_DATA *, int, long, void *) {
    delete static_cast<ctx_state *>(ptr);
}

// One process-wide ex-data index, registered once (magic-static + OpenSSL's own lock make this
// thread-safe). The registered free-func ties ctx_state's lifetime to the SSL_CTX's.
int
ctx_state_index() noexcept {
    static const int idx = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, &ctx_state_free);
    return idx;
}

ctx_state *
ctx_get_state(SSL_CTX *c) noexcept {
    return c ? static_cast<ctx_state *>(SSL_CTX_get_ex_data(c, ctx_state_index())) : nullptr;
}

// Get the state attached to `c`, creating+attaching it if absent (role defaults to client).
ctx_state *
ctx_require_state(SSL_CTX *c) {
    if (!c)
        return nullptr;
    if (auto *st = ctx_get_state(c))
        return st;
    auto *st = new ctx_state();
    if (SSL_CTX_set_ex_data(c, ctx_state_index(), st) != 1) {
        delete st; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return nullptr;
    }
    return st;
}

int
ctx_ossl_version(TlsVersion v) noexcept {
    switch (v) {
        case TlsVersion::v1_2:
            return TLS1_2_VERSION;
        case TlsVersion::v1_3:
            return TLS1_3_VERSION;
    }
    return 0; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

int
ctx_ossl_verify_mode(VerifyMode m) noexcept {
    switch (m) {
        case VerifyMode::none:
            return SSL_VERIFY_NONE;
        case VerifyMode::peer:
            return SSL_VERIFY_PEER;
        case VerifyMode::peer_require:
            return SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    }
    return SSL_VERIFY_NONE; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

// Serialize {"h2","http/1.1"} into OpenSSL's wire format: <len><proto><len><proto>...
std::vector<unsigned char>
ctx_serialize_alpn(const std::vector<std::string> &protocols) {
    std::vector<unsigned char> out;
    for (const auto &p : protocols) {
        if (p.empty() || p.size() > 255)
            continue;
        out.push_back(static_cast<unsigned char>(p.size()));
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// ---- trampolines: recover ctx_state from the SSL_CTX ex-data, dispatch to the std::function ----

int
ctx_verify_trampoline(int preverify_ok, X509_STORE_CTX *store) {
    SSL *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (!ssl)
        return preverify_ok; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    auto *st = ctx_get_state(SSL_get_SSL_CTX(ssl));
    if (!st || !st->verify)
        return preverify_ok;
    VerifyContext vc{store};
    try {
        return st->verify(preverify_ok != 0, vc) ? 1 : 0;
    } catch (...) {
        return 0; // an exception must not cross OpenSSL's C frames: a verify that throws rejects
    }
}

void
ctx_keylog_trampoline(const SSL *ssl, const char *line) {
    auto *st = ctx_get_state(SSL_get_SSL_CTX(const_cast<SSL *>(ssl)));
    if (st && st->keylog && line) {
        try {
            st->keylog(std::string_view{line});
        } catch (...) {
            // the line is lost; the handshake is not the sink's to stop
        }
    }
}

int
ctx_sni_trampoline(SSL *ssl, int *alert, void *) {
    auto *st = ctx_get_state(SSL_get_SSL_CTX(ssl));
    if (!st || !st->sni)
        return SSL_TLSEXT_ERR_OK;
    const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    Context     selected;
    try {
        selected = st->sni(name ? std::string_view{name} : std::string_view{});
    } catch (...) {
        // A router that throws decided nothing: serving the default certificate could be the wrong one.
        if (alert)
            *alert = SSL_AD_INTERNAL_ERROR;
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    // SSL_set_SSL_CTX up-refs the selected context, so it stays alive even after `selected` dies;
    // its ex-data state rides along on that context. Callers typically keep per-host Contexts anyway.
    if (selected && selected.native())
        SSL_set_SSL_CTX(ssl, selected.native());
    return SSL_TLSEXT_ERR_OK;
}

// OpenSSL keeps ONE status callback per SSL_CTX and calls it in both roles -- on a server when a client asks for
// a staple, on a client once the server's certificate (and its staple, if any) arrived -- so one trampoline
// serves `on_ocsp_staple` and `on_ocsp_response`, by the role of the SSL at hand.
int
ctx_ocsp_trampoline(SSL *ssl, void *) {
    auto *st = ctx_get_state(SSL_get_SSL_CTX(ssl));
    if (SSL_is_server(ssl)) {
        if (!st || !st->ocsp_staple)
            return SSL_TLSEXT_ERR_NOACK;
        std::vector<unsigned char> der;
        try {
            const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
            der              = st->ocsp_staple(name ? std::string_view{name} : std::string_view{});
        } catch (...) {
            return SSL_TLSEXT_ERR_NOACK; // a stapler that throws staples nothing
        }
        if (der.empty())
            return SSL_TLSEXT_ERR_NOACK;
        auto *copy = static_cast<unsigned char *>(OPENSSL_malloc(der.size()));
        if (!copy)
            return SSL_TLSEXT_ERR_NOACK; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        std::memcpy(copy, der.data(), der.size());
        SSL_set_tlsext_status_ocsp_resp(ssl, copy, static_cast<long>(der.size())); // the SSL owns `copy` now
        return SSL_TLSEXT_ERR_OK;
    }
    if (!st || !st->ocsp_check)
        return 1;
    try {
        OcspContext view{ssl};
        return st->ocsp_check(view) ? 1 : 0;
    } catch (...) {
        return 0; // a check that throws rejects
    }
}

// (Re)install the status trampoline while either role's callback is set.
void
ctx_install_ocsp(SSL_CTX *c, const ctx_state &st) {
    // SSL_CTX_set_tlsext_status_cb is a macro that C-casts its argument: pick the callback into a typed variable.
    int (*cb)(SSL *, void *) = (st.ocsp_staple || st.ocsp_check) ? &ctx_ocsp_trampoline : nullptr;
    SSL_CTX_set_tlsext_status_cb(c, cb);
}

int
ctx_alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *) {
    auto *st = ctx_get_state(SSL_get_SSL_CTX(ssl));
    if (!st || st->alpn_wire.empty())
        return SSL_TLSEXT_ERR_NOACK;
    const int r = SSL_select_next_proto(const_cast<unsigned char **>(out), outlen, st->alpn_wire.data(),
                                        static_cast<unsigned int>(st->alpn_wire.size()), in, inlen);
    return (r == OPENSSL_NPN_NEGOTIATED) ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_NOACK;
}

// Wrap a freshly-created SSL_CTX (single owning ref) into a shared_ptr with the SSL_CTX_free deleter
// and attach the ex-data state carrying the client/server role.
std::shared_ptr<SSL_CTX>
ctx_own(SSL_CTX *c, bool is_server) {
    if (!c)
        return {};
    // Set the role only when we CREATE the state — never clobber an existing role. Otherwise
    // share()/adopt() wrapping a ctx that already carries state from another Context (both share
    // the same ex-data) would flip that Context's client/server role out from under it.
    if (!ctx_get_state(c)) {
        if (auto *st = ctx_require_state(c))
            st->is_server = is_server;
    }
    return std::shared_ptr<SSL_CTX>(c, SSL_CTX_free);
}

} // namespace

// ---------------------------------------------------------------------------
// VerifyContext
// ---------------------------------------------------------------------------

int
VerifyContext::depth() const noexcept {
    return _raw ? X509_STORE_CTX_get_error_depth(_raw) : 0;
}

int
VerifyContext::error() const noexcept {
    return _raw ? X509_STORE_CTX_get_error(_raw) : X509_V_ERR_UNSPECIFIED;
}

std::string
VerifyContext::error_string() const {
    const char *s = X509_verify_cert_error_string(error());
    return s ? std::string(s) : std::string();
}

Certificate
VerifyContext::current_certificate() const {
    Certificate ret{};
    if (!_raw)
        return ret;
    X509 *cert = X509_STORE_CTX_get_current_cert(_raw); // borrowed; do not free
    if (!cert)
        return ret;
    if (char *line = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0)) {
        ret.subject = line;
        OPENSSL_free(line);
    }
    if (char *line = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0)) {
        ret.issuer = line;
        OPENSSL_free(line);
    }
    ret.version = X509_get_version(cert);
    return ret;
}

// ---------------------------------------------------------------------------
// OcspContext
// ---------------------------------------------------------------------------

std::span<const unsigned char>
OcspContext::response() const noexcept {
    unsigned char *der = nullptr; // borrowed: the SSL keeps it
    const long     len = _ssl ? SSL_get_tlsext_status_ocsp_resp(_ssl, &der) : -1;
    if (len <= 0 || !der)
        return {};
    return {der, static_cast<std::size_t>(len)};
}

std::string_view
OcspContext::servername() const noexcept {
    const char *name = _ssl ? SSL_get_servername(_ssl, TLSEXT_NAMETYPE_host_name) : nullptr;
    return name ? std::string_view{name} : std::string_view{};
}

// ---------------------------------------------------------------------------
// Context — internals
// ---------------------------------------------------------------------------

Context::Context(std::shared_ptr<SSL_CTX> ctx) noexcept
    : _ctx(std::move(ctx)) {}

bool
Context::usable() const noexcept {
    // Same predicate as the public ok(); kept as a named private gate for the fluent setters, but
    // defined in terms of ok() so the "healthy context" invariant lives in exactly one place.
    return ok();
}

void
Context::fail(std::string message) noexcept {
    if (_error.empty())
        _error = std::move(message);
}

bool
Context::ok() const noexcept {
    return _ctx != nullptr && _error.empty();
}

std::string
Context::error() const {
    return _error;
}

SSL_CTX *
Context::native() const noexcept {
    return _ctx.get();
}

// ---------------------------------------------------------------------------
// Context — factories
// ---------------------------------------------------------------------------

Context
Context::client() {
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (!c)
        return {}; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    Context ctx{ctx_own(c, /*is_server*/ false)};
    // Secure by default: TLS 1.2+, system trust store, verify the peer.
    ctx.min_version(TlsVersion::v1_2).trust_system().verify(VerifyMode::peer);
    return ctx;
}

Context
Context::server(std::filesystem::path cert, std::filesystem::path key) {
    SSL_CTX *c = SSL_CTX_new(TLS_server_method());
    if (!c)
        return {}; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    Context ctx{ctx_own(c, /*is_server*/ true)};
    ctx.min_version(TlsVersion::v1_2).identity(std::move(cert), std::move(key));
    return ctx;
}

Context
Context::adopt(SSL_CTX *raw) noexcept {
    if (!raw)
        return {};
    // TRANSFER: take over the caller's single reference (no up-ref). Attach our ex-data state if the
    // context does not already carry it (e.g. it came from another Context::native()).
    return Context{ctx_own(raw, /*is_server*/ false)};
}

Context
Context::share(SSL_CTX *raw) noexcept {
    if (!raw)
        return {};
    // SHARE: take a NEW reference; the caller keeps and must still free their own.
    if (SSL_CTX_up_ref(raw) != 1)
        return {}; // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return Context{ctx_own(raw, /*is_server*/ false)};
}

// ---------------------------------------------------------------------------
// Context — fluent configuration
// ---------------------------------------------------------------------------

Context &
Context::min_version(TlsVersion v) {
    if (usable() && SSL_CTX_set_min_proto_version(_ctx.get(), ctx_ossl_version(v)) != 1)
        fail(ctx_with_reason("SSL_CTX_set_min_proto_version failed"));
    return *this;
}

Context &
Context::max_version(TlsVersion v) {
    if (usable() && SSL_CTX_set_max_proto_version(_ctx.get(), ctx_ossl_version(v)) != 1)
        fail(ctx_with_reason("SSL_CTX_set_max_proto_version failed"));
    return *this;
}

Context &
Context::verify(VerifyMode mode) {
    if (usable()) {
        // Preserve any installed on_verify trampoline when changing the mode.
        auto         *st = ctx_get_state(_ctx.get());
        SSL_verify_cb cb = (st && st->verify) ? &ctx_verify_trampoline : nullptr;
        SSL_CTX_set_verify(_ctx.get(), ctx_ossl_verify_mode(mode), cb);
    }
    return *this;
}

Context &
Context::trust(std::filesystem::path ca_file_or_dir) {
    if (!usable())
        return *this;
    if (ca_file_or_dir.empty()) {
        fail("trust: empty path");
        return *this;
    }
    const auto      resolved = qb::io::sys::resolve_resource(ca_file_or_dir);
    std::error_code ec;
    const bool      is_dir = std::filesystem::is_directory(resolved, ec);
    const auto      s      = resolved.string();
    if (SSL_CTX_load_verify_locations(_ctx.get(), is_dir ? nullptr : s.c_str(), is_dir ? s.c_str() : nullptr) != 1)
        fail(ctx_with_reason("SSL_CTX_load_verify_locations failed for " + s));
    return *this;
}

Context &
Context::trust_system() {
    if (usable() && SSL_CTX_set_default_verify_paths(_ctx.get()) != 1)
        fail(ctx_with_reason("SSL_CTX_set_default_verify_paths failed"));
    return *this;
}

Context &
Context::identity(std::filesystem::path cert, std::filesystem::path key) {
    if (!usable())
        return *this;
    const auto cert_s = qb::io::sys::resolve_resource(cert).string();
    const auto key_s  = qb::io::sys::resolve_resource(key).string();
    if (SSL_CTX_use_certificate_file(_ctx.get(), cert_s.c_str(), SSL_FILETYPE_PEM) <= 0)
        fail(ctx_with_reason("SSL_CTX_use_certificate_file failed for " + cert_s));
    else if (SSL_CTX_use_PrivateKey_file(_ctx.get(), key_s.c_str(), SSL_FILETYPE_PEM) <= 0)
        fail(ctx_with_reason("SSL_CTX_use_PrivateKey_file failed for " + key_s));
    else if (SSL_CTX_check_private_key(_ctx.get()) <= 0)
        fail(ctx_with_reason("SSL_CTX_check_private_key failed (cert/key mismatch)"));
    return *this;
}

Context &
Context::alpn(std::vector<std::string> protocols) {
    if (!usable())
        return *this;
    auto wire = ctx_serialize_alpn(protocols);
    if (wire.empty())
        return *this; // no protocols to advertise -> leave ALPN unconfigured (a no-op, not an error): a
                      // server forwarding an optional/empty ALPN list must still start.
    auto *st = ctx_require_state(_ctx.get());
    if (!st) {
        fail("alpn: could not attach context state"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return *this;
    }
    if (st->is_server) {
        // Server: keep the accept-list at a stable address and select via the ex-data-driven callback.
        st->alpn_wire = std::move(wire);
        SSL_CTX_set_alpn_select_cb(_ctx.get(), &ctx_alpn_select_cb, nullptr);
    } else {
        // Client: offer the protocol list (SSL_CTX_set_alpn_protos returns 0 on success).
        if (SSL_CTX_set_alpn_protos(_ctx.get(), wire.data(), static_cast<unsigned int>(wire.size())) != 0)
            fail(ctx_with_reason("SSL_CTX_set_alpn_protos failed"));
    }
    return *this;
}

Context &
Context::ciphers(std::string tls12_list) {
    if (usable() && SSL_CTX_set_cipher_list(_ctx.get(), tls12_list.c_str()) != 1)
        fail(ctx_with_reason("SSL_CTX_set_cipher_list failed"));
    return *this;
}

Context &
Context::ciphersuites(std::string tls13_list) {
    if (usable() && SSL_CTX_set_ciphersuites(_ctx.get(), tls13_list.c_str()) != 1)
        fail(ctx_with_reason("SSL_CTX_set_ciphersuites failed"));
    return *this;
}

Context &
Context::curves(std::string groups) {
    if (usable() && SSL_CTX_set1_curves_list(_ctx.get(), groups.c_str()) != 1)
        fail(ctx_with_reason("SSL_CTX_set1_curves_list failed"));
    return *this;
}

Context &
Context::dh_params(std::filesystem::path pem) {
    if (!usable())
        return *this;
    const auto native = qb::io::sys::resolve_resource(pem).string();
    BIO       *bio    = BIO_new_file(native.c_str(), "r");
    if (!bio) {
        fail(ctx_with_reason("dh_params: cannot open " + native));
        return *this;
    }
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY *pkey = PEM_read_bio_Parameters(bio, nullptr);
    BIO_free(bio);
    if (!pkey) {
        fail(ctx_with_reason("dh_params: PEM_read_bio_Parameters failed for " + native));
        return *this;
    }
    if (SSL_CTX_set0_tmp_dh_pkey(_ctx.get(), pkey) != 1) {
        EVP_PKEY_free(pkey);
        fail(ctx_with_reason("dh_params: SSL_CTX_set0_tmp_dh_pkey failed")); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
#else
    DH *dh = PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!dh) {
        fail(ctx_with_reason("dh_params: PEM_read_bio_DHparams failed for " + native));
        return *this;
    }
    if (SSL_CTX_set_tmp_dh(_ctx.get(), dh) != 1)
        fail(ctx_with_reason("dh_params: SSL_CTX_set_tmp_dh failed"));
    DH_free(dh);
#endif
    return *this;
}

Context &
Context::session_cache(std::size_t entries) {
    if (!usable())
        return *this;
    SSL_CTX_set_session_cache_mode(_ctx.get(), entries == 0 ? SSL_SESS_CACHE_OFF : SSL_SESS_CACHE_SERVER);
    if (entries != 0)
        SSL_CTX_sess_set_cache_size(_ctx.get(), static_cast<long>(entries));
    return *this;
}

Context &
Context::session_timeout(std::chrono::seconds timeout) {
    if (usable())
        SSL_CTX_set_timeout(_ctx.get(), static_cast<long>(timeout.count()));
    return *this;
}

// ---------------------------------------------------------------------------
// Context — typed callbacks
// ---------------------------------------------------------------------------

Context &
Context::on_keylog(std::function<void(std::string_view)> cb) {
    if (!usable())
        return *this;
    auto *st = ctx_require_state(_ctx.get());
    if (!st) {
        fail("on_keylog: could not attach context state"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return *this;
    }
    st->keylog = std::move(cb);
    SSL_CTX_set_keylog_callback(_ctx.get(), st->keylog ? &ctx_keylog_trampoline : nullptr);
    return *this;
}

Context &
Context::on_verify(std::function<bool(bool, VerifyContext &)> cb) {
    if (!usable())
        return *this;
    auto *st = ctx_require_state(_ctx.get());
    if (!st) {
        fail("on_verify: could not attach context state"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return *this;
    }
    st->verify = std::move(cb);
    // Keep the current mode; install/remove the trampoline according to whether a callback is set.
    SSL_CTX_set_verify(_ctx.get(), SSL_CTX_get_verify_mode(_ctx.get()), st->verify ? &ctx_verify_trampoline : nullptr);
    return *this;
}

Context &
Context::on_sni(std::function<Context(std::string_view)> cb) {
    if (!usable())
        return *this;
    auto *st = ctx_require_state(_ctx.get());
    if (!st) {
        fail("on_sni: could not attach context state"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return *this;
    }
    st->sni = std::move(cb);
    // SSL_CTX_set_tlsext_servername_callback is a macro that C-casts its argument, so a ternary would
    // bind to the cast by precedence — pick the callback into a typed variable first.
    int (*sni_cb)(SSL *, int *, void *) = st->sni ? &ctx_sni_trampoline : nullptr;
    SSL_CTX_set_tlsext_servername_callback(_ctx.get(), sni_cb);
    return *this;
}

Context &
Context::on_ocsp_staple(std::function<std::vector<unsigned char>(std::string_view)> cb) {
    if (!usable())
        return *this;
    auto *st = ctx_require_state(_ctx.get());
    if (!st) {
        fail("on_ocsp_staple: could not attach context state"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return *this;
    }
    st->ocsp_staple = std::move(cb);
    ctx_install_ocsp(_ctx.get(), *st);
    return *this;
}

Context &
Context::on_ocsp_response(std::function<bool(OcspContext &)> cb) {
    if (!usable())
        return *this;
    auto *st = ctx_require_state(_ctx.get());
    if (!st) {
        fail("on_ocsp_response: could not attach context state"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return *this;
    }
    st->ocsp_check = std::move(cb);
    // A check needs something to judge: every client of this context asks for a staple while it is set. -1 is
    // OpenSSL's "nothing" status type (TLSEXT_STATUSTYPE_nothing in its sources, not exported by 3.x headers).
    SSL_CTX_set_tlsext_status_type(_ctx.get(), st->ocsp_check ? TLSEXT_STATUSTYPE_ocsp : -1);
    ctx_install_ocsp(_ctx.get(), *st);
    return *this;
}

} // namespace qb::io::ssl
