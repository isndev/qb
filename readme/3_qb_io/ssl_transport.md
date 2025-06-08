@page qb_io_ssl_transport_md QB-IO: Secure TCP with SSL/TLS
@brief Enabling encrypted TCP communication in `qb-io` using OpenSSL.

# QB-IO: Secure TCP with SSL/TLS

`qb-io` provides robust support for secure TCP communication through SSL/TLS, leveraging the OpenSSL library. This allows you to encrypt data streams, ensuring confidentiality and integrity for your networked applications.

**Prerequisites:**

*   The `QB_IO_WITH_SSL` CMake option must be **ON** during the build of the QB framework.
*   The OpenSSL library (development headers and libraries) must be installed and accessible on your system.

## Core SSL/TLS Components in `qb-io`

`qb-io` extends its standard TCP components with SSL/TLS capabilities:

1.  **`qb::io::tcp::ssl::socket`** (from `qb/io/tcp/ssl/socket.h`)
    *   **Purpose:** This class is your secure TCP socket. It wraps a standard `qb::io::tcp::socket` and transparently layers SSL/TLS encryption and decryption over it using an OpenSSL `SSL*` handle.
    *   **Key Functionality:** Manages the SSL handshake process (client-side `connect()`, server-side after `accept()`). All data passed through its `read()` and `write()` methods is automatically encrypted/decrypted.
    *   **Initialization:** Before connecting or accepting, an `ssl::socket` must be associated with an OpenSSL context (`SSL_CTX*`) via its `init(SSL_CTX* ctx)` method or by being constructed/assigned from an already SSL-enabled socket.
    *   **Advanced Access:** `ssl_handle()` provides direct access to the underlying `SSL*` object if you need fine-grained control over OpenSSL settings.

2.  **`qb::io::tcp::ssl::listener`** (from `qb/io/tcp/ssl/listener.h`)
    *   **Purpose:** This class acts as a secure TCP server, listening for incoming connections and performing the server-side SSL/TLS handshake.
    *   **Functionality:** It requires an `SSL_CTX*` configured with the server's certificate and private key. When `accept()` is called, it first accepts a standard TCP connection and then establishes the SSL/TLS session over it, returning a fully secured `qb::io::tcp::ssl::socket` ready for communication.
    *   **Initialization:** Call `init(SSL_CTX* ctx)` with a server-configured SSL context *before* invoking `listen()`.

3.  **SSL Context Creation Helpers** (in `qb/io/tcp/ssl/socket.h` namespace `qb::io::ssl`)
    *   `qb::io::ssl::create_client_context(const SSL_METHOD* method)`: Simplifies creating an `SSL_CTX*` suitable for SSL/TLS clients (e.g., using `TLS_client_method()`).
    *   `qb::io::ssl::create_server_context(const SSL_METHOD* method, const std::string& cert_path, const std::string& key_path)`: Creates an `SSL_CTX*` for servers, loading the server's certificate and private key from the specified PEM-encoded files.
    *   **Note:** The caller is responsible for freeing the returned `SSL_CTX*` using `SSL_CTX_free()` when it's no longer needed (typically managed by the actor or class that owns the listener/socket).

4.  **Secure TCP Transport: `qb::io::transport::stcp`** (from `qb/io/transport/stcp.h`)
    *   **Purpose:** This is the specialized `qb::io::stream` implementation for secure TCP. It uses `qb::io::tcp::ssl::socket` as its underlying I/O mechanism.
    *   **Base Class:** Inherits from `qb::io::stream<qb::io::tcp::ssl::socket>`.
    *   **Key Behavior:** Its `read()` method is aware of OpenSSL's internal buffering. After performing a socket read, it calls `SSL_pending()` to check if OpenSSL has already decrypted and buffered additional data. If so, it performs further reads from the SSL layer to ensure all application data is retrieved promptly and not left in OpenSSL's internal buffers.

## Integrating Secure Transports with Asynchronous Components

For actor-based development or other asynchronous classes, `qb-io` provides `qb::io::use<>` helper templates (in `qb/io/async.h`) to simplify integration:

*   `use<MyActor>::tcp::ssl::client<ServerActor = void>`: Base for an actor that acts as an SSL/TLS client.
*   `use<MyActor>::tcp::ssl::acceptor`: Base for an actor that accepts incoming SSL/TLS connections.
*   `use<MyActor>::tcp::ssl::server<SessionActor>`: A comprehensive base for an actor that acts as an SSL/TLS server, combining an acceptor and managing `SessionActor` instances for each client.
*   `use<MyActor>::tcp::ssl::io_handler<SessionActor>`: If you need more custom server logic, this provides the session management part for SSL sessions.

These helpers ensure that the correct secure transport (`transport::stcp`) and socket (`tcp::ssl::socket`) types are used automatically.

## Implementing an SSL/TLS Server

1.  **Certificates:** Obtain or generate your server certificate (`.pem` file) and corresponding private key (`.pem` file). For testing, self-signed certificates can be created using OpenSSL command-line tools (see `test-io/system/CMakeLists.txt` for an example command: `openssl req -x509 ...`).
2.  **SSL Context (`SSL_CTX`):** In your server actor (or main setup code):
    ```cpp
    #include <qb/io/tcp/ssl/socket.h> // For create_server_context, SSL_CTX_free
    // ... other necessary includes ...

    SSL_CTX* _server_ssl_ctx = nullptr; // Typically a member of your server/acceptor actor

    // In constructor or onInit of your server/acceptor actor:
    const char* my_cert_path = "path/to/your/server_cert.pem";
    const char* my_key_path = "path/to/your/server_key.pem";
    _server_ssl_ctx = qb::io::ssl::create_server_context(TLS_server_method(), my_cert_path, my_key_path);
    if (!_server_ssl_ctx) {
        // Handle error: Failed to create context or load certificate/key files
        throw std::runtime_error("Server SSL context creation failed. Check paths and OpenSSL errors.");
    }
    // Remember to free the context, e.g., in the actor's destructor:
    // if (_server_ssl_ctx) { SSL_CTX_free(_server_ssl_ctx); _server_ssl_ctx = nullptr; }
    ```
3.  **Acceptor/Server Actor:** Inherit from `qb::io::use<MyAcceptor>::tcp::ssl::acceptor` or `qb::io::use<MyServer>::tcp::ssl::server<MySession>`.
4.  **Initialize Listener Transport:** Before listening, associate the `SSL_CTX` with the listener's transport:
    ```cpp
    // Inside your acceptor/server actor's onInit (after creating _server_ssl_ctx):
    this->transport().init(_server_ssl_ctx);
    ```
5.  **Listen:** Call `this->transport().listen_v4(port, host);` (or `listen_v6`, `listen(uri)`).
6.  **Start Accepting:** Call `this->start();` (method from the `qb::io::async` base like `async::input`).
7.  **Handle New Secure Connections:** Implement `on(accepted_socket_type&& new_secure_socket)` (for acceptors) or `on(IOSession& new_secure_session)` (for combined servers). The accepted socket/session will be an `ssl::socket` or a session class using `transport::stcp`.

**(Reference Example:** `test-async-io.cpp` (SSL test), `test-session-text.cpp` (Secure test).**)

## Implementing an SSL/TLS Client

1.  **SSL Context (`SSL_CTX`):** In your client actor (or setup code):
    ```cpp
    #include <qb/io/tcp/ssl/socket.h> // For create_client_context, SSL_CTX_free
    // ... other necessary includes ...

    SSL_CTX* _client_ssl_ctx = nullptr; // Typically a member of your client actor

    // In constructor or onInit of your client actor:
    _client_ssl_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    if (!_client_ssl_ctx) {
        throw std::runtime_error("Client SSL context creation failed.");
    }
    // Optional: Configure server certificate verification (recommended for production)
    // SSL_CTX_set_verify(_client_ssl_ctx, SSL_VERIFY_PEER, nullptr); // custom_verify_callback can be nullptr
    // if (!SSL_CTX_load_verify_locations(_client_ssl_ctx, "/path/to/ca_bundle.pem", nullptr)) {
    //     // Handle error loading CA certificates
    // }
    // Remember to free the context in the actor's destructor.
    ```
2.  **Client Actor:** Inherit from `qb::io::use<MyClient>::tcp::ssl::client`.
3.  **Initialize Client Transport:** Before connecting, associate the `SSL_CTX`:
    ```cpp
    // Inside your client actor's onInit (after creating _client_ssl_ctx):
    this->transport().init(_client_ssl_ctx);
    ```
4.  **Connect:** Use the transport's connect methods, e.g., `this->transport().connect_v4(host, port, sni_hostname);` or `this->transport().n_connect(endpoint, sni_hostname);`. Providing the `sni_hostname` is important for Server Name Indication.
    For fully asynchronous connection (using `n_connect`), you'll typically use `qb::io::async::tcp::connect<qb::io::tcp::ssl::socket>(uri_or_endpoint, sni_hostname, connect_callback_lambda);`.
5.  **Handshake:** The SSL handshake occurs as part of the connection process.
    *   For blocking `connect`, it completes before returning.
    *   For non-blocking `n_connect` (or when using `async::tcp::connect`), the `connect_callback_lambda` receives the TCP-connected `ssl::socket`. You then typically call `this->transport() = std::move(accepted_socket); this->switch_protocol<MyProtocol>(*this); this->start(); this->transport().connected();` where `transport().connected()` finalizes the SSL handshake.
6.  **Communicate Securely:** Once connected and the handshake is complete, use `*this << ...;` or `in()`/`out()` methods as with regular TCP. The `transport::stcp` handles encryption/decryption.

**(Reference Example:** `test-async-io.cpp` (SSL test), `test-session-text.cpp` (Secure test).**)

By following these patterns, you can seamlessly integrate SSL/TLS encryption into your QB applications, enhancing their security.

**(Next:** [QB-IO: Utilities](./utilities.md)**) 