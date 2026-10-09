# Secure TCP with SSL/TLS

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.2.1 (C++20 default, C++23 supported) — 90fa721f

`qb-io` layers OpenSSL-backed SSL/TLS over its TCP stack, with secure-by-default client verification, a context-holding listener, and a stream transport that drains OpenSSL's internal buffers.

**Prerequisites:** [Transports](./transports.md) · [The async runtime](./async_system.md) — **See also:** [Native QUIC transport](./quic_transport.md) · [qb-io utilities](./utilities.md)

## Summary

Secure TCP in `qb-io` is the plain TCP stack with an OpenSSL encryption layer spliced
underneath the same stream and async interfaces. The pieces are:

- `qb::io::ssl::Context` — **the preferred way to configure TLS.** A value-semantic,
  reference-counted, secure-by-default handle over an `SSL_CTX`: copyable (copies share one
  context, freed exactly once — no user `SSL_CTX_free`), fluent (`Context::client()` /
  `Context::server(cert, key).alpn({"h2"})`), fail-closed (a bad cert yields a falsy Context
  whose `error()` explains why). Hand it to a socket or listener; there is no raw context
  lifetime to manage. See [The `ssl::Context` type](#the-sslcontext-type).
- `qb::io::tcp::ssl::socket` — a `tcp::socket` that owns an OpenSSL `SSL*` and runs the
  handshake plus transparent `SSL_read`/`SSL_write`. Build it from a `Context`
  (`ssl::socket{ssl::Context::client()}`) or let `connect()` auto-create a secure client one.
- `qb::io::tcp::ssl::listener` — a `tcp::listener` that holds a `Context` and mints a
  configured `ssl::socket` per accepted connection.
- `qb::io::transport::stcp` — the `stream<ssl::socket>` specialization that backs every
  asynchronous secure session.
- Free functions in `qb::io::ssl` for building and configuring raw `SSL_CTX` objects — the
  advanced/escape-hatch layer, kept permanently but superseded by `ssl::Context` for most uses.

The whole slice compiles only when the build was configured with OpenSSL available; see
[Build gating](#build-gating). Throughout, the socket and listener follow `qb-io`'s
ownership conventions: they are move-only, and the listener holds a **refcounted**
`ssl::Context` rather than owning the raw `SSL_CTX` outright — `~listener()` frees nothing,
and the `SSL_CTX` dies when the last `Context` copy *and* the last `SSL` minted from it are
gone (`src/qb/io/tcp/ssl/listener.cpp:30`; `src/qb/io/tcp/ssl/context.cpp:259`).

```mermaid
flowchart TB
    STCP["transport::stcp<br/>stream&lt;ssl::socket&gt; — is_secure() == true"]
    STCP --> SS["tcp::ssl::socket<br/>owns SSL* — SSL_read / SSL_write, runs the handshake"]
    SS --> TS["tcp::socket<br/>native handle (base)"]
    L["tcp::ssl::listener<br/>holds a refcounted ssl::Context"] -- "mints a configured ssl::socket per accept" --> SS
```

> **Linking the SSL slice changes one process-wide setting.** `qb/io/tcp/ssl/init.cpp` runs a
> static initializer that calls `SSL_library_init()`, `SSL_load_error_strings()`,
> `OpenSSL_add_all_algorithms()` **and `signal(SIGPIPE, SIG_IGN)`**
> (`src/qb/io/tcp/ssl/init.cpp:62-71`). Ignoring `SIGPIPE` is what stops a write to a
> half-closed socket from killing the process, and it is almost always what you want — but it
> is an observable side effect of linking, not of calling anything, so a program that relied on
> the default disposition needs to reinstate it after startup.

## Build gating

The SSL transport is an optional feature. The CMake option `QB_WITH_SSL` defaults to `ON`,
but OpenSSL is found by `find_package(OpenSSL QUIET)` and is **never** fetched. If OpenSSL
is not present, `QB_WITH_SSL` is forced `OFF`, the resolved capability `QB_HAS_SSL` becomes
false, and the entire SSL/TLS slice is absent.

<!-- src: qb/cmake/qbConfig.cmake:160, qb/cmake/qbDependencies.cmake:125-147 -->

| Symbol | Meaning |
| --- | --- |
| `QB_WITH_SSL` | User-facing request (CMake option, default `ON`). Forced `OFF` if OpenSSL is missing. |
| `QB_HAS_SSL` | Resolved capability after dependency probing. Gates the compile definition and the headers below. |

All SSL headers — `qb/io/tcp/ssl/socket.h`, `qb/io/tcp/ssl/context.h`,
`qb/io/tcp/ssl/listener.h`, `qb/io/transport/stcp.h` and `qb/io/transport/saccept.h` — require
an OpenSSL-enabled build. (They are still installed; they simply do not compile without it.)
The `use<>::tcp::ssl` async aliases in `qb/io/async.h` are themselves guarded by
`#ifdef QB_HAS_SSL`, so code that references them must also be compiled under that definition.

<!-- src: qb/src/qb/io/async.h:113-127 -->

> The crypto and JWT toolbox shares this OpenSSL dependency, but does not fail the same way.
> `qb/io/crypto_jwt.h` `#error`s at compile time unless `QB_HAS_SSL` is defined;
> `qb/io/crypto.h` compiles either way, because its gate is per **member** — the class keeps
> everything that needs no OpenSSL (the hex codec a cleartext qbm-pgsql build depends on) and
> drops the rest, so a no-SSL build fails at the call site (`no member named 'md5' in
> 'qb::crypto'`). A non-SSL build has neither secure transport nor the OpenSSL-backed crypto.
> <!-- src: qb/src/qb/io/crypto.h:33-44, qb/src/qb/io/crypto_jwt.h:36-38 -->

## Core components

### The `ssl::Context` type

Declared in `qb/io/tcp/ssl/context.h`. A `Context` is a value: copy it and both copies share the
same reference-counted `SSL_CTX`, destroyed exactly once when the last copy — and the last `SSL`
minted from it — is gone. There is no user-visible `SSL_CTX_free`, so the double-free / leak
footguns of hand-rolled OpenSSL ownership are structurally impossible.

<!-- src: qb/src/qb/io/tcp/ssl/context.h:157 -->

It is **secure by default** and **fails closed**: `Context::client()` pins TLS 1.2+, loads the
system trust store, and verifies the peer; a construction or configuration error (a missing cert, a
bad cipher list) yields a falsy Context whose `error()` explains why — it never silently degrades to
an insecure context.

```cpp
// Server: one shared, fluently-configured context.
auto ctx = qb::io::ssl::Context::server("cert.pem", "key.pem").alpn({"h2", "http/1.1"});
qb::io::tcp::ssl::listener listener{ctx};              // shared by ref-count across every accept

// Client: secure by default; per-connection SNI on top.
qb::io::tcp::ssl::socket client{qb::io::ssl::Context::client().alpn({"h2"})};
client.sni("example.com").connect(ep);
```

The factories are `Context::client()`, `Context::server(cert, key)`, and two escape hatches for
wrapping a raw `SSL_CTX*`: `Context::adopt` (transfer the caller's reference) and `Context::share`
(take a new reference; the caller keeps theirs).

<!-- src: qb/src/qb/io/tcp/ssl/context.h:168-195 -->

**A certificate file is a chain.** `Context::server(cert, key)` and `identity(cert, key)` load every
certificate the PEM file holds -- the leaf first, then the intermediates a peer needs to reach the anchor
it trusts -- and so do the raw `create_server_context` and `configure_client_certificate`. Until 3.3 all
three read the leaf only, so a certificate an intermediate issued failed every verifying peer that did
not already hold that intermediate (Huly QB-611); a file that holds only the leaf is served as before,
and the verification itself is unchanged.

<!-- src: qb/src/qb/io/tcp/ssl/context.cpp:463-464 (identity loads the chain), qb/src/qb/io/tcp/ssl/socket.cpp:196 (create_server_context), :297 (configure_client_certificate) -->

Configuration is a fluent chain (`min_version`/`max_version`, `verify`, `trust`/`trust_system`,
`identity`, `alpn`, `ciphers`/`ciphersuites`/`curves`, `dh_params`, `session_cache`/
`session_timeout`); each call is a no-op once the Context has errored, so a single `ok()` check at
the end suffices. The verification, key-log and SNI hooks are **typed** callbacks (`on_verify` /
`on_keylog` / `on_sni` take `std::function`s, not raw C pointers): the closures live on the
context's `SSL_CTX` ex-data, so they are reachable from every minted `SSL` and are destroyed with
the context. `VerifyMode` is `none` / `peer` / `peer_require` (the last adds fail-if-no-cert, i.e.
mutual TLS); `TlsVersion` is `v1_2` / `v1_3`. Since 3.3 OCSP has its pair of typed callbacks too,
`on_ocsp_staple` and `on_ocsp_response` ([below](#ocsp-stapling)), and every typed callback that
throws fails closed instead of carrying the exception through OpenSSL's C frames, which terminated
the process.

<!-- src: qb/src/qb/io/tcp/ssl/context.h:63 (TlsVersion), :73 (VerifyMode), :232-236 (typed on_keylog/on_verify/on_sni) -->

The raw `qb::io::ssl::` free functions and `socket::init(SSL*)` / `listener::init(SSL_CTX*)` remain
available as an advanced escape hatch for fully hand-built configurations.

#### OCSP stapling

A server staples the OCSP response for its certificate to the handshake, so its clients learn the
certificate is not revoked without asking the CA themselves. Both ends are typed on the `Context`
(3.3, Huly QB-83):

```cpp
// src: derived from qb/src/qb/io/tcp/ssl/context.h:237-248 (on_ocsp_staple, on_ocsp_response), :109-135 (OcspContext)
// Server: hand OpenSSL the DER response your refresher fetched from the CA (empty: staple nothing).
auto server_ctx = qb::io::ssl::Context::server("cert.pem", "key.pem")
                      .on_ocsp_staple([&cache](std::string_view /*servername*/) { return cache.current(); });

// Client: ask every server for its staple and judge it -- false fails the handshake.
auto client_ctx = qb::io::ssl::Context::client().on_ocsp_response([](qb::io::ssl::OcspContext &ocsp) {
    if (ocsp.response().empty())
        return false;   // must-staple: no response, no connection
    return my_ocsp_check(ocsp.response(), ocsp.native());   // d2i_OCSP_RESPONSE, OCSP_basic_verify, OCSP_resp_find_status
});
```

- **The client check asks for the staple itself.** Setting `on_ocsp_response` makes every connection
  of the context request one; it is then called once the server's certificate has arrived, with an
  empty response when none was stapled -- a must-staple client rejects that, a tolerant one accepts.
  Returning `false` fails the handshake.
- **What the server staples must be real.** OpenSSL 3.6 parses the response on both ends: the server
  staples it only when it parses and names the certificate being served (serial and issuer), never
  for a self-signed certificate, and a client fails a staple that does not parse before any check
  runs. Earlier versions pass the bytes through as given.
- **One slot per role pair.** OpenSSL keeps ONE status callback per `SSL_CTX` for both roles, and
  each typed callback shares its slot with the raw setter of the same job
  (`set_ocsp_stapling_client_callback`, `set_ocsp_stapling_responder_server`,
  `listener::set_ocsp_stapling_responder_callback`; likewise `on_verify`, `on_sni`, `on_keylog`): on
  one context, whichever was set last is the one OpenSSL calls.
- **A callback that throws fails closed.** It is caught where OpenSSL calls it: a verify or an OCSP
  check rejects the handshake, an SNI router aborts it, a stapler staples nothing, a keylog line is
  lost. Before 3.3 the exception crossed OpenSSL's C frames and the process terminated.
<!-- src: qb/src/qb/io/tcp/ssl/context.cpp:184-217 (one status trampoline, by role; the staple handed to OpenSSL; the check's verdict), :633-648 (on_ocsp_response asks for the staple); qb/src/qb/io/tcp/ssl/context.h:220-228 (the shared slots, failing closed) -->

### `qb::io::tcp::ssl::socket`

Declared in `qb/io/tcp/ssl/socket.h`. Inherits `qb::io::tcp::socket` and owns the OpenSSL
`SSL*` through a `std::unique_ptr<SSL, …>`, plus an optional `ssl::Context` it was built from.
It is move-only (the copy constructor is deleted, move construction is defaulted, and move
assignment is user-provided — it releases the existing `SSL` before taking over the source; the
`SSL`'s reference to its reference-counted `SSL_CTX` is dropped by `SSL_free`, never by a direct
`SSL_CTX_free`), so ownership of the native handle, the `SSL`, and the context transfers on move.

```cpp
class QB_API socket : public tcp::socket {
    // ...
public:
    constexpr static bool is_secure() noexcept { return true; }

    void init(SSL *handle = nullptr) noexcept;

    int connect(endpoint const &ep, std::string const &hostname = "") noexcept;
    int connect(std::vector<qb::io::endpoint> const &endpoints, std::string const &hostname) noexcept; // 3.3
    int connect(uri const &u) noexcept;
    int connect_v4(std::string const &host, uint16_t port) noexcept;
    int connect_v6(std::string const &host, uint16_t port) noexcept;

    int n_connect(qb::io::endpoint const &ep, std::string const &hostname = "") noexcept;
    int connected() noexcept;          // drives SSL_connect/SSL_accept after non-blocking connect

    int handshake_status() noexcept;   // 1 done, 0 needs I/O, -1 fatal
    [[nodiscard]] bool handshake_wants_write() const noexcept;   // 3.3: which I/O a pending one waits on
    [[nodiscard]] bool handshake_complete() const noexcept;

    int read(void *data, std::size_t size) noexcept;        // SSL_read
    int write(const void *data, std::size_t size) noexcept; // SSL_write
    int disconnect() noexcept;                              // NO close_notify — see below

    void set_insecure() noexcept;                           // opt out of peer verification
    [[nodiscard]] bool verify_peer() const noexcept;

    [[nodiscard]] SSL *ssl_handle() const noexcept;
};
```
<!-- src: qb/src/qb/io/tcp/ssl/socket.h:361-975 -->

Key behaviors verified in the header:

- **Initialization.** A default-constructed `ssl::socket` is uninitialized. `init(SSL*)`
  takes ownership of an `SSL` handle (created with `SSL_new` from an `SSL_CTX`). The
  blocking `connect*` family builds an `SSL_CTX` and handle for you when none is supplied,
  which is why those calls work directly on a default-constructed socket.
- **Async connector with a Context.** For full control the async connectors take a caller-built
  `ssl::Context` socket — a private CA (`trust`), a client certificate (`identity`, mutual TLS), or a
  custom verify mode: `connect(ssl::socket{Context::client()…}, uri, cb)`, and the STARTTLS sibling
  for PostgreSQL `SSLRequest` / SMTP·IMAP `STARTTLS`. The negotiator policy is **not deducible**, so
  that one must be spelled with explicit template arguments —
  `starttls_connect<Socket, Negotiator>(socket, uri, cb)`. Both async connect overloads also take a
  trailing `bool verify_peer = true`, applied as `set_insecure()` before `n_connect` — the
  asynchronous equivalent of calling `set_insecure()` yourself. The `qbm`
  PostgreSQL (`ssl_root_cert`/`ssl_cert`/`ssl_key`) and Redis (`set_ssl_root_cert` /
  `set_ssl_client_certificate`) clients drive exactly this path.
  <!-- src: qb/src/qb/io/async/tcp/connector.h:870-873 (starttls_connect, Negotiator_ not deducible), :756 (connect verify_peer), :780 (connect with existing socket), :527-530 (verify_peer applies set_insecure) -->
- **Return convention.** `connect*` and `n_connect*` return `int`: `0` on success — the
  value of `qb::io::SocketStatus::Done` — and non-zero on failure, generically
  `SocketStatus::Error` (`-1`). `n_connect*` returns the underlying non-blocking TCP
  result, so a connect still in progress is not an error.
  A **failed peer verification is not a distinct return value.** It fails the TLS
  handshake in `handCheck()`, which disconnects and returns `-1`, so `connect()` reports
  `-1` exactly as any other handshake error does. The enum's third enumerator,
  `SocketStatus::CertificateError` (`1`), is part of the public surface but is returned by
  nothing in qb; to tell a verification failure apart, read `SSL_get_verify_result()` or
  the OpenSSL error queue.
  <!-- src: qb/src/qb/io/system/sys__socket.h:1620-1624 (SocketStatus enumerators) -->
  <!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:824-835 (connect return gate), :948-967 (n_connect), :726-752 (handCheck) -->
- **Handshake progress.** `handshake_status()` returns `1` when the TLS handshake is
  complete, `0` when OpenSSL needs more socket readiness (`WANT_READ`/`WANT_WRITE`), and
  `-1` on a fatal error. `handshake_complete()` reports whether it finished successfully.
  `do_handshake()` is an inline alias for the internal handshake check.
- **Which readiness, since 3.3.** After a `0`, `handshake_wants_write()` says which one:
  `true` when OpenSSL's output did not fit the socket, `false` when it waits for the peer's
  bytes (OpenSSL's own `SSL_want_write`; no I/O). Watch that one direction, never both: a
  connected socket is always writable, so a write watch on a handshake that waits to read
  wakes a level-triggered loop on every pass until the peer answers. The async connectors
  do exactly that on all three paths (a direct connect, the turn that completes an
  in-progress one, a STARTTLS upgrade); until 3.3 they watched both, and every client
  handshake spun the loop for a round trip (Huly QB-301).
  <!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:991-994 (handshake_wants_write); qb/src/qb/io/async/tcp/connector.h:321-328 (pending_handshake_events), :424, :575, :717 (the three paths) -->
- **`read()`'s return convention is not the plain socket's.** It returns the number of
  decrypted bytes on success; **`-1` on an orderly peer shutdown** (OpenSSL's
  `SSL_ERROR_ZERO_RETURN` — the peer sent `close_notify`), so the framework's error path
  disposes the session; and **`0` when OpenSSL needs more socket readiness**
  (`WANT_READ` / `WANT_WRITE`) or the handshake is still in progress. That is the inverse of
  `tcp::socket::read`, where `0` means the peer closed. The header's `@return` block says
  otherwise and is wrong.
  <!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:1048-1053 (orderly shutdown returns -1), :1058-1060 (WANT_* returns 0), :1065 (handshake in progress returns 0) -->
- **Read drains less than requested.** Because OpenSSL can hold already-decrypted
  application data internally, generic streaming code should use `transport::stcp`, which
  handles `SSL_pending()` for you (see [The stcp transport](#the-stcp-transport)).
- **`disconnect()` sends no `close_notify`.** It clears the connected flag and calls
  `tcp::socket::disconnect()`; `SSL_shutdown` is not called anywhere in the tree. The
  auto-created client context is put into quiet-shutdown mode at connect time
  (`SSL_set_quiet_shutdown`), which makes that the deliberate behaviour rather than an
  omission — but a peer that requires a graceful TLS closure will see an abrupt one.
  <!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:1030-1034 (disconnect), :925 (SSL_set_quiet_shutdown) -->

#### Secure by default

When `qb-io` builds the client `SSL_CTX` itself — the usual `connect()` / `n_connect()` /
async-connector path — it loads the system trust store, enables `SSL_VERIFY_PEER`, and
verifies the server certificate against the target hostname or IP. A connection to a host
whose certificate does not validate **fails**.

```cpp
// Verifying client (default): rejects a self-signed or untrusted certificate.
qb::io::tcp::ssl::socket c;
int rc = c.connect_v4("example.com", 443);   // 0 only if the chain + hostname verify
```

To opt out — self-signed certificates in tests, pinning handled elsewhere, or a channel
trusted by other means — call `set_insecure()` **before** `connect()` / `n_connect()`.

```cpp
// Opt out of verification for a local self-signed fixture.
qb::io::tcp::ssl::socket c;
c.set_insecure();                            // disables MITM protection — use deliberately
int rc = c.connect_v4("127.0.0.1", 64388);
```
<!-- src: qb/tests/io/system/tls/tls-peer-verification.cpp:196-201 -->

**Supplying your own `SSL` handle through `init(SSL*)` does not opt you out of that policy** — and this is the one place on the page where getting it wrong is a security bug rather than a compile error. `setup_client_ssl()` calls `apply_client_verification_()` unconditionally, and that function branches on whether an `ssl::Context` was supplied, not on whether *you* built the handle. An `init(SSL*)` socket has no `Context`, so it takes the auto-context branch: `SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr)` plus hostname checking. A `SSL_VERIFY_NONE` you set on your own context is overridden to `SSL_VERIFY_PEER`, and a verify callback you installed on the handle is replaced with `nullptr`. `SSL_set_quiet_shutdown` and `SSL_set_connect_state` are applied to your handle too.

To keep an `init(SSL*)` handle unverified — the shape every SSL test in the suite uses for a self-signed fixture — call `set_insecure()` before connecting. To keep *your own* verification policy, build a `qb::io::ssl::Context` and pass that instead: the `Context` path is honoured as written.

<!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:935 (apply_client_verification_ is unconditional), :705-724 (the branch is keyed on _ctx.native() == nullptr), :574-575 (SSL_VERIFY_PEER + hostname on the auto path), :925 (quiet shutdown), :936 (connect state); qb/src/qb/io/tcp/ssl/socket.h:521-528 (init takes ownership of the SSL) -->

#### Pre-handshake configuration

These settings must be applied before the handshake — but only **five of the seven are
cached** on the socket and replayed once the `SSL` handle exists. The other two need a live
handle and are silent no-ops without one, which is the trap on this table:

| Method | Before `connect()`? | Purpose |
| --- | --- | --- |
| `set_sni_hostname(const std::string&)` | cached | Server Name Indication for the next handshake. |
| `set_alpn_protocols(const std::vector<std::string>&)` | cached | Offer ALPN protocols (e.g. `{"h2", "http/1.1"}`). |
| `disable_session_resumption()` | cached | Sets `SSL_OP_NO_TICKET \| SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION` and drops any pending session. |
| `request_ocsp_stapling(bool enable = true)` | cached | Request a stapled OCSP response from the server; `false` withdraws this connection's request, never the context's (`on_ocsp_response` keeps asking: a connection that asks for nothing never reaches its verdict). |
| `set_session(qb::io::ssl::Session&)` | cached | Offer a previously cached session for resumption. |
| `set_verify_callback(int(*)(int, X509_STORE_CTX*), int mode)` | **needs a handle** | Per-connection X.509 verification callback. |
| `set_verify_depth(int)` | **needs a handle** | Maximum verification chain depth. |
| `set_insecure()` | any time before the handshake | Disable peer verification on the auto-created context. |

`set_verify_callback` and `set_verify_depth` both open with `if (!_ssl_handle) return false;`. On a default-constructed or `Context`-built socket the `SSL` is minted *inside* `connect()` / `n_connect()`, so calling either beforehand does nothing and returns `false` — a return value it is easy not to check. The windows in which they work are: after `init(SSL*)`, or between `n_connect()` and `connected()`.

`disable_session_resumption()` and `set_session()` are mutually exclusive when deferred; the last call wins.

<!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:1365-1371 (set_verify_callback needs a handle), :1373-1379 (set_verify_depth), :1353 (sni deferred), :1362 (alpn deferred), :1191-1192 (resumption deferred, drops the session), :1210 (ocsp deferred), :1322-1323 (session deferred), :1184 (the two SSL_OP flags); qb/src/qb/io/tcp/ssl/socket.h:801 (disable_session_resumption), :815 (request_ocsp_stapling), :849 (set_session), :872 (set_sni_hostname), :884 (set_alpn_protocols), :894 (set_verify_callback), :902 (set_verify_depth), :921 (set_insecure) -->

#### Introspection and sessions

After a successful handshake the socket exposes `get_negotiated_cipher_suite()`,
`get_negotiated_tls_version()`, `get_alpn_selected_protocol()`,
`get_peer_certificate_details()` and `get_peer_certificate_chain()` — all five gate on the
connected flag and return nothing before that. `get_last_ssl_error_string()` does **not**:
it needs only an `SSL` handle, which is what makes it the accessor to reach for after a
*failed* handshake.
<!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:1105 (cipher suite gates on _connected), :1137, :1146, :1155, :1217, :1168-1170 (error string gates only on the handle) -->

`get_session()` returns a `qb::io::ssl::Session` for client-side resumption. The caller
owns it and must release it with `qb::io::ssl::free_session()`. Setting a session does not
guarantee resumption — the server must agree.

<!-- src: qb/src/qb/io/tcp/ssl/socket.h:757-782 (introspection), :824 (peer chain), :836 (get_session), :333 (free_session) -->

### `qb::io::tcp::ssl::listener`

Declared in `qb/io/tcp/ssl/listener.h`. Inherits `qb::io::tcp::listener` and holds a
value-semantic `qb::io::ssl::Context` — **not** a raw `std::unique_ptr<SSL_CTX, …>`. The context is
shared by reference count with every connection the listener accepts, so there is no `SSL_CTX_free`
bookkeeping and one context can back several listeners. It is move-only (copy construction and copy
assignment are `= delete`).

```cpp
class QB_API listener : public tcp::listener {
    qb::io::ssl::Context _ctx;   // value-semantic, refcount-shared with every accepted connection
    // _alpn_wire: unused since 3.3, kept for the class layout -- the ALPN list
    // set_supported_alpn_protocols() sets lives on the context (Huly QB-308).
    [[maybe_unused]] mutable std::unique_ptr<std::vector<unsigned char>> _alpn_wire;
public:
    constexpr static bool is_secure() noexcept { return true; }

    listener() noexcept;
    explicit listener(qb::io::ssl::Context ctx) noexcept;   // preferred
    listener(listener const &)            = delete;
    listener(listener &&)                 = default;
    listener &operator=(listener &&)      = default;

    void init(qb::io::ssl::Context ctx) noexcept;   // preferred — no raw lifetime to manage
    void init(SSL_CTX *ctx) noexcept;               // escape hatch; adopts the caller's ref
    [[nodiscard]] bool reload_context(qb::io::ssl::Context ctx) noexcept;   // 3.3: a renewal, for the next accepts

    ssl::socket accept() const noexcept;
    int         accept(ssl::socket &socket) const noexcept;

    [[nodiscard]] SSL_CTX                     *ssl_handle() const noexcept;
    [[nodiscard]] const qb::io::ssl::Context  &context() const noexcept;   // fail-closed via context().ok()
    // plus context configuration: configure_mtls, set_tls_protocol_versions,
    // set_cipher_list, set_supported_alpn_protocols, enable_session_caching, ...
};
```
<!-- src: qb/src/qb/io/tcp/ssl/listener.h:44 (class listener), :45 (the Context member), :82-93 (move-only), :105 (init), :177 (ssl_handle), :183 (context) -->
<!-- src: qb/src/qb/io/tcp/ssl/listener.h:44-380 (the whole class, `class QB_API listener` to its closing brace) -->

- **Two `init` overloads, and the `Context` one is the one to use.** `init(ssl::Context)`
  takes the value-semantic handle and has no lifetime to manage
  (`src/qb/io/tcp/ssl/listener.h:116`); it is what `async::tcp::acceptor::listen_no_start()`
  calls, and what the TLS round-trip test uses. `init(SSL_CTX*)` is the escape hatch: it
  **adopts** the caller's single reference into the refcounted holder
  (`_ctx = ssl::Context::adopt(ctx)`, `src/qb/io/tcp/ssl/listener.cpp:38-42`). Neither makes
  `~listener()` free anything — its body is empty; the `SSL_CTX` goes when the last `Context`
  copy and the last minted `SSL` are gone. Call either **before** `listen()`; to replace the context
  of a listener that is accepting, `reload_context()` ([below](#renewing-the-certificate-while-serving)).
  <!-- src: qb/src/qb/io/tcp/ssl/listener.cpp:30 (empty destructor), :38-42 (adopt), :44-47 (init(Context)); qb/src/qb/io/async/tcp/acceptor.h:163 (the acceptor's call) -->
- **`adopt` and `share` mark the context client-role.** A raw `SSL_CTX` brought in that way
  is treated as a client context, so a later `.alpn(...)` on it configures the *client offer*,
  not the server's selection list — a server that adopts a raw context and then calls `.alpn()`
  gets the wrong behaviour with no diagnostic. Build server contexts with
  `ssl::Context::server(cert, key)` instead. (`src/qb/io/tcp/ssl/context.cpp:389`, `:399`.)
- **Accept.** Both `accept()` overloads first perform a plain TCP accept, then create an
  `SSL` object from `_ctx` and associate it with the accepted descriptor. The returned
  (or filled) `ssl::socket` still needs its handshake driven — by `connected()` /
  `do_handshake()` for manual use, or automatically by the async server machinery.
- **Server configuration.** The listener forwards a wide range of context settings,
  including `configure_mtls()` for client-certificate (mTLS) authentication,
  `set_tls_protocol_versions()`, `set_cipher_list()` / `set_ciphersuites_tls13()`,
  `set_supported_alpn_protocols()`, `enable_session_caching()`,
  `configure_dh_parameters()`, and `configure_ecdh_curves()`. Each returns `false` if the
  context is not initialized.
- **The ALPN list is the context's.** `set_supported_alpn_protocols()` writes the server's
  accept-list where `Context::alpn()` writes it, on the context
  (`qb::io::ssl::set_alpn_protos_server`, the raw sibling of `set_alpn_protos_client`): every
  listener sharing the context serves it, the context a `reload_context()` leaves keeps it for
  the connections minted from it, and it lives as long as the context. Until 3.3 the listener
  owned the list and registered its address with the context's selection callback, so a context
  that outlived the listener selected from freed memory, and the context a reload left
  negotiated the list set on the next one (Huly QB-308). A context has one selection slot:
  this list, `Context::alpn()`'s and `set_alpn_selection_callback()` replace one another.
  <!-- src: qb/src/qb/io/tcp/ssl/listener.cpp:215-222 (the setter delegates); qb/src/qb/io/tcp/ssl/context.cpp:241-244 (one installer), :656-670 (set_alpn_protos_server) -->

### The stcp transport

`qb::io::transport::stcp` (in `qb/io/transport/stcp.h`) is the
`qb::io::stream<qb::io::tcp::ssl::socket>` specialization that backs every asynchronous
secure session. Its `read()` does a socket read, then checks `SSL_pending()` and performs a
second read for any application data OpenSSL has already decrypted and buffered internally.
Without that drain, decrypted bytes would be stranded until the next socket readiness
event. Both `read()` results are bounded by `_max_read_buffer_size` and return
`ErrBufferLimitExceeded` if the cap would be exceeded.

<!-- src: qb/src/qb/io/transport/stcp.h:44-89 -->

### SSL context helpers

Free functions in namespace `qb::io::ssl` (declared in `qb/io/tcp/ssl/socket.h`) build and
configure `SSL_CTX` objects.

```cpp
namespace qb::io::ssl {

SSL_CTX *create_client_context(const SSL_METHOD *method);

SSL_CTX *create_server_context(const SSL_METHOD *method,
                               std::filesystem::path cert_path,
                               std::filesystem::path key_path);

// Context configuration (each returns bool):
bool load_ca_certificates(SSL_CTX *ctx, const std::filesystem::path &ca_file_path);
bool load_ca_directory(SSL_CTX *ctx, const std::filesystem::path &ca_dir_path);
bool set_tls_protocol_versions(SSL_CTX *ctx, int min_version, int max_version);
bool configure_mtls_server_context(SSL_CTX *ctx,
                                   const std::filesystem::path &client_ca_file_path,
                                   int verification_mode = SSL_VERIFY_PEER);
bool configure_client_certificate(SSL_CTX *ctx,
                                  const std::filesystem::path &client_cert_path,
                                  const std::filesystem::path &client_key_path);
bool configure_dh_parameters_server(SSL_CTX *ctx, const std::filesystem::path &dh_param_file_path);
bool set_alpn_protos_client(SSL_CTX *ctx, const std::vector<std::string> &protocols);
bool set_alpn_protos_server(SSL_CTX *ctx, const std::vector<std::string> &protocols); // 3.3: kept on the context
// ... cipher lists, OCSP, ECDH, keylog, session caching, PHA, and more.

} // namespace qb::io::ssl
```
<!-- src: qb/src/qb/io/tcp/ssl/socket.h:37-346 (the whole `namespace qb::io::ssl` free-function block), :84 (create_client_context), :96 (create_server_context), :200 (set_alpn_protos_server), :344 (enable_post_handshake_auth_server) -->

`create_client_context` and `create_server_context` return `nullptr` on failure (for
example when the certificate or key file cannot be loaded). **The caller owns the returned
`SSL_CTX` and must release it with `SSL_CTX_free()`** — except when it is handed to a
`listener` via `init()`, which then owns and frees it.

Every file-path argument across these helpers — the certificate and key for
`create_server_context`, the CA file/directory for `load_ca_certificates` /
`load_ca_directory` / `configure_mtls_server_context`, the client certificate and key for
`configure_client_certificate`, and the DH parameters for
`configure_dh_parameters_server` — is a `std::filesystem::path`, not a raw `const char*`.
Each path is resolved through `qb::io::sys::resolve_resource()` before OpenSSL opens it: an
absolute path is used unchanged, while a relative path is looked up against the current
working directory first and then against the running executable's own directory. A server
shipped next to its `cert.pem` / `key.pem` therefore loads them regardless of the cwd it is
launched from.

<!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:185-205 (create_server_context), :207-310 (the four CA/client-cert helpers), :208 (load_ca_certificates), :221 (load_ca_directory), :274 (configure_mtls_server_context), :292 (configure_client_certificate), :415-420 (configure_dh_parameters_server) -->

## Building an SSL server

Inherit from the async SSL server alias, build a server context from the certificate and key
file paths, and hand it to the transport's listener before listening. The aliases (verified in
`qb/io/async.h`) are:

| Alias | Role |
| --- | --- |
| `use<T>::tcp::ssl::acceptor` | Accepts connections; you handle each accepted `ssl::socket`. |
| `use<T>::tcp::ssl::server<Session>` | Acceptor plus per-client `Session` management. |
| `use<T>::tcp::ssl::io_handler<Session>` | Session-management mixin for custom servers. |
| `use<T>::tcp::ssl::client<Server = void>` | Secure client session over `transport::stcp`. |

<!-- src: qb/src/qb/io/async.h:113-127 -->

```cpp
// src: qb/tests/io/system/tls/tls-text-roundtrip.cpp:78-127 (adapted)
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/ssl/socket.h>   // qb::io::ssl::create_server_context

using namespace qb::io;

class SecureServer;

// One session per accepted client. The transport is transport::stcp.
class SecureSession
    : public use<SecureSession>::tcp::ssl::client<SecureServer> {
public:
    using Protocol = qb::protocol::text::command<SecureSession>;

    explicit SecureSession(IOServer &server) : client(server) {}

    void on(Protocol::message &&msg) {
        *this << msg.text << Protocol::end;   // echo back, still encrypted
    }
};

class SecureServer
    : public use<SecureServer>::tcp::ssl::server<SecureSession> {
public:
    void on(IOSession &) { /* a new secure session was established */ }
};

void run_server(const std::filesystem::path &cert_path,
                const std::filesystem::path &key_path) {
    async::init();

    SecureServer server;
    // create_server_context returns nullptr on failure; the listener takes
    // ownership of the SSL_CTX and frees it on destruction.
    server.transport().init(
        ssl::create_server_context(TLS_server_method(), cert_path, key_path));

    server.transport().listen_v4(64384);   // 0 on success
    server.start();                        // begin accepting on the event loop

    while (true)
        async::run(EVRUN_ONCE);
}
```

The server-side handshake is driven by the async machinery as each connection is accepted;
you never call `SSL_accept` directly. For listeners that need mTLS, protocol pinning, or
specific cipher policy, configure the context (or the listener's forwarding methods) before
`listen()`.

## Renewing the certificate while serving

A certificate expires; a server that must restart to present the renewed one drops every
connection it holds. Since 3.3 the listener takes a replacement context while it serves
(Huly QB-205):

```cpp
// src: derived from qb/src/qb/io/tcp/ssl/listener.h:118-146 (reload_context)
// On the server's own thread -- the actor or the loop that accepts -- once the files are renewed:
auto renewed = qb::io::ssl::Context::server(cert_path, key_path).alpn({"h2", "http/1.1"});
if (!server.transport().reload_context(renewed))
    QB_LOG_WARN("certificate renewal refused: " << renewed.error());   // the previous one still serves
```

- **The next accept presents it; every open connection keeps its own.** `accept()` mints each
  connection's `SSL` from the listener's context, and an `SSL` holds a reference on the `SSL_CTX`
  it came from. A connection accepted before the reload -- established, or with its handshake not
  yet run -- finishes with the previous certificate, and the previous context is freed with the
  last of them. Nothing is dropped, nothing is renegotiated.
- **A renewal that failed to load is refused.** `reload_context` returns `false` and changes nothing
  when the context is not `ok()`: missing files, or a certificate and key that do not match -- the
  half-finished renewal. The server goes on presenting the certificate it had; `init(Context)`, by
  contrast, installs whatever it is given and is meant for the setup before `listen()`.
- **Build the replacement whole.** The listener's raw setters (`configure_mtls`, `set_cipher_list`,
  `set_supported_alpn_protocols`, `enable_session_caching`, ...) wrote into the previous context, and a
  new one does not inherit them: give the replacement its ALPN, verification and cipher policy through
  `Context`, as the snippet does. Sessions resumed after the reload get a full handshake, since the
  session cache and the ticket keys belong to the new context.
- **One thread.** Call it where the listener accepts -- nothing synchronizes it with `accept()`, like
  every other member. Loading the files is the slow part and may run elsewhere: build the `Context`
  inside `co_await qb::io::async::offload(...)` and reload with the result. Never rewrite the served
  context in place through `native()` instead: copies of a `Context` share one `SSL_CTX`, possibly
  across cores, and an in-place change races every one of them.

A complete, runnable program lives in the corpus — `examples/02-io/13-tls-certificate-renewal.cpp`:
it renews a working copy of a certificate in two steps, holds a session open across the renewal,
and has a client that trusts only the renewed certificate refused before it and accepted after.
<!-- src: qb/src/qb/io/tcp/ssl/listener.cpp:49-57 (reload_context: refuses a falsy context, then swaps), :64, :83 (each accept overload mints its SSL from the current context); qb/src/qb/io/tcp/ssl/listener.h:125-132 (who keeps which context), :133-136 (the owning thread) -->

## Building an SSL client

The default client verifies the peer. Against a public CA-signed server, a plain
`connect_v4(host, port)` is sufficient. For a self-signed test fixture, opt out with
`set_insecure()` before connecting.

```cpp
// src: qb/tests/io/system/tls/tls-text-roundtrip.cpp:104-143 (adapted)
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

using namespace qb::io;

class SecureClient : public use<SecureClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::text::command<SecureClient>;

    void on(Protocol::message &&msg) {
        // received a decrypted, framed message
    }
};

void run_client() {
    async::init();

    SecureClient client;

    // Local self-signed server on 127.0.0.1: opt out of the default
    // peer verification. Omit this line for a CA-signed endpoint.
    client.transport().set_insecure();

    if (SocketStatus::Done !=
        client.transport().connect_v4("127.0.0.1", 64384)) {
        throw std::runtime_error("could not connect to secure server");
    }

    client.start();
    client << "ping" << '\n';   // encrypted on the wire

    while (true)
        async::run(EVRUN_ONCE);
}
```

**Connecting by hostname sets SNI for you** — and overwrites anything you set beforehand.
`connect_v4` / `connect_v6` resolve the host and route it through `connect(endpoints, hostname)`
-- every address tried in order, the TLS set up once on the one that answered (3.3) -- and
`connect(uri)` does the same with `u.host()`; `setup_client_ssl` then assigns that host to the
cached SNI value. So this is all a verified client needs:

```cpp
SecureClient client;
if (SocketStatus::Done != client.transport().connect_v4("api.example.com", 443))
    throw std::runtime_error("TLS connect/verify failed");   // SNI + hostname check: automatic
```

`set_sni_hostname()` earns its place in the cases where no hostname reaches the socket: the
`connect(endpoint)` / `n_connect(endpoint)` overloads, whose `hostname` parameter defaults to
empty, and the Unix-domain entry points. Those are the connections where the chain is
validated but the **name is not**, so set it explicitly before connecting — or pass the
hostname to the overload that takes one.
<!-- src: qb/src/qb/io/tcp/ssl/socket.cpp:926-927 (the cached SNI is overwritten with the connect hostname), :803-809 (connect_in), :760-778 (connect(endpoints, hostname)), :852-863 (connect(uri) supplies u.host()); qb/src/qb/io/tcp/ssl/socket.h:547 (the endpoint overload's empty default), :872 (set_sni_hostname) -->

## Generating a test certificate

The framework's own SSL tests generate a self-signed certificate with OpenSSL. The exact
command (RSA-2048, `CN=localhost`, 365-day validity, with a `subjectAltName` so hostname
verification can pass for `localhost`) is:

```bash
# src: qb/tests/io/system/CMakeLists.txt:111-113
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
    -days 365 -nodes \
    -subj "/CN=localhost/O=QB Tests/C=US" \
    -addext "subjectAltName = DNS:localhost"
```

A self-signed certificate is rejected by a default (verifying) client; pair it with
`set_insecure()` on the client, or add the certificate to the client's trust store.

## Pitfalls

- **The slice vanishes without OpenSSL.** If OpenSSL is not found, `QB_WITH_SSL` is forced
  `OFF`, the headers stop compiling, and the `use<>::tcp::ssl` aliases do not exist. Verify
  `QB_HAS_SSL` in the build configuration before depending on any of this.
- **Verification is on by default — including for an `SSL` handle you built yourself.**
  `init(SSL*)` does not exempt you: the socket applies `SSL_VERIFY_PEER` and hostname checking
  to your handle, replacing a `SSL_VERIFY_NONE` and any verify callback you installed. To keep
  a handle unverified, call `set_insecure()`; to keep your own policy, pass an `ssl::Context`
  instead of a raw `SSL*`.
- **`read()` returns `-1`, not `0`, when the peer shuts the TLS session down cleanly** — and
  `0` when OpenSSL merely wants more socket readiness. Do not carry `tcp::socket::read`'s
  convention across.
- **`disconnect()` does not send `close_notify`.** The client context runs in quiet-shutdown
  mode; a peer that requires a graceful TLS closure sees an abrupt one.
- **`set_verify_callback` / `set_verify_depth` are silent no-ops before `connect()`.** They
  need a live `SSL` handle and return `false` without one. Every other pre-handshake setting
  is cached and replayed; these two are not.
- **Connecting by hostname sets SNI itself and discards a prior `set_sni_hostname()`.** Set
  it explicitly only for the `connect(endpoint)` / Unix-domain paths, where no hostname is
  supplied and the chain is validated without the name being checked.
- **Timed connect does not bound the handshake.** The timed `connect(ep, hostname, wtimeout)`
  overloads bound only the underlying TCP connect phase; the TLS handshake itself is not
  separately timed.
  <!-- src: qb/src/qb/io/tcp/ssl/socket.h:552, :578 (timed connect overloads); qb/src/qb/io/tcp/ssl/socket.cpp:781-800 (the timed connect over the addresses), :825-835 (the handshake, untimed) -->
- **`SSL_CTX` ownership splits by path.** A context from `create_*_context` is caller-owned
  and must be `SSL_CTX_free`d — unless it is passed to `listener::init()`, which then owns
  and frees it. A `Session` from `get_session()` is always caller-owned; release it with
  `free_session()`.
- **Use `stcp` for streaming, not raw `read()`.** OpenSSL may buffer decrypted data
  internally. `transport::stcp::read()` drains `SSL_pending()`; the raw `socket::read()`
  does not, so a hand-rolled read loop can strand already-decrypted bytes.

## See also

- [Transports](./transports.md) — how `stcp` fits the stream/transport model.
- [Asynchronous I/O system](./async_system.md) — the event loop and `use<>` helpers.
- [Native QUIC transport](./quic_transport.md) — the alternative encrypted transport.
- [qb-io utilities](./utilities.md) — the OpenSSL-backed crypto toolbox that shares this dependency.
