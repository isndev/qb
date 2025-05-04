# QB-IO: Secure TCP (SSL/TLS) Transport

**Note:** This functionality requires the `QB_IO_WITH_SSL` CMake option to be enabled during build and depends on the OpenSSL library being available on the system.

`qb-io` facilitates secure TCP communication using SSL/TLS through dedicated components that integrate with the OpenSSL library.

## Core SSL Components

*   **`qb::io::tcp::ssl::socket` (`socket.h`):**
    *   **Purpose:** Wraps a standard `tcp::socket` with an SSL/TLS layer (`SSL*` handle).
    *   **Inheritance:** Derives from `qb::io::tcp::socket`, inheriting basic socket operations.
    *   **Functionality:** Manages the SSL handshake process during connection establishment (`connect`, `n_connect`, `connected`). Automatically handles encryption/decryption during `read` and `write` operations.
    *   **Key Method:** `init(SSL_CTX* ctx)`: Associates the socket with an OpenSSL context *before* connecting or accepting.
    *   **Access:** `ssl_handle()` returns the underlying `SSL*` for advanced configuration if needed.
*   **`qb::io::tcp::ssl::listener` (`listener.h`):**
    *   **Purpose:** Listens for incoming TCP connections and performs the server-side SSL handshake.
    *   **Inheritance:** Derives from `qb::io::tcp::listener`.
    *   **Functionality:** Requires an `SSL_CTX` (configured with server certificate and key). Its `accept()` methods return an established, secure `qb::io::tcp::ssl::socket`.
    *   **Key Method:** `init(SSL_CTX* ctx)`: Associates the listener with a server SSL context *before* calling `listen()`.
*   **SSL Context Helpers (`qb::io::ssl::create_*_context` in `socket.h`):**
    *   **Purpose:** Simplify the creation and basic configuration of OpenSSL `SSL_CTX` objects.
    *   `create_client_context(method)`: Creates a context suitable for clients (e.g., using `TLS_client_method()`).
    *   `create_server_context(method, cert_path, key_path)`: Creates a context suitable for servers, loading the necessary certificate and private key files. Requires valid paths to PEM-encoded files.
*   **`qb::io::transport::stcp` (`stcp.h`):**
    *   **Purpose:** The specific `qb::io::stream` implementation for SSL/TLS sockets.
    *   **Base:** `qb::io::stream<qb::io::tcp::ssl::socket>`.
    *   **Key Feature:** Its `read()` method is aware of OpenSSL's internal buffering. It calls `SSL_pending()` after a socket read to check for any remaining decrypted bytes held by OpenSSL and reads them to ensure all application data is retrieved promptly.

## Asynchronous Integration (`qb::io::use<...>::tcp::ssl::*`)

(`async.h`)

These templates simplify building actors or other classes with integrated async SSL capabilities:

*   `use<T>::tcp::ssl::client<Server = void>`: For actors acting as SSL clients.
*   `use<T>::tcp::ssl::acceptor`: For actors accepting SSL connections.
*   `use<T>::tcp::ssl::server<Session>`: Combines acceptor and session management for SSL servers.
*   `use<T>::tcp::ssl::io_handler<Session>`: Base for managing multiple SSL sessions.

These `use` templates ensure the correct underlying transport (`transport::stcp`) and socket (`tcp::ssl::socket`) types are used.

## How to Use: Server

1.  **Generate Certificate/Key:** Obtain or generate a server certificate (`cert.pem`) and private key (`key.pem`). For testing, self-signed certificates can be generated using OpenSSL command-line tools (see `test-io/system/CMakeLists.txt` for an example command).
2.  **Create SSL Context:** In your server actor's constructor or `onInit`, create the `SSL_CTX`:
    ```cpp
    #include <qb/io/tcp/ssl/socket.h>
    // ... inside server actor ...
    SSL_CTX* _ssl_ctx = nullptr; // Member variable (manage lifetime!)

    // In constructor or onInit:
    const char* cert_path = "./cert.pem";
    const char* key_path = "./key.pem";
    _ssl_ctx = qb::io::ssl::create_server_context(TLS_server_method(), cert_path, key_path);
    if (!_ssl_ctx) {
        // Handle error: Failed to create context or load cert/key
        throw std::runtime_error("SSL context creation failed");
    }
    // Remember to free the context in the destructor: if (_ssl_ctx) SSL_CTX_free(_ssl_ctx);
    ```
3.  **Create Server/Listener Actor:** Inherit from `qb::io::use<...>::tcp::ssl::server` or `::acceptor`.
4.  **Initialize Transport:** Pass the created `SSL_CTX` to the transport:
    ```cpp
    // Inside server actor constructor or onInit if using combined server base:
    transport().init(_ssl_ctx);
    ```
5.  **Listen:** Call `transport().listen_v4(...)` or similar.
6.  **Start:** Call `start()` to begin accepting connections.
7.  **Handle Sessions:** Implement `on(IOSession&)` (if using combined server) or `on(accepted_socket_type&&)` (if using acceptor) to handle new secure connections.

**(Ref:** `test-async-io.cpp` (SSL test), `test-session-text.cpp` (Secure test)**)

## How to Use: Client

1.  **Create SSL Context:** In your client actor's constructor or `onInit`:
    ```cpp
    #include <qb/io/tcp/ssl/socket.h>
    // ... inside client actor ...
    SSL_CTX* _ssl_ctx = nullptr; // Member variable (manage lifetime!)

    // In constructor or onInit:
    _ssl_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    if (!_ssl_ctx) {
        throw std::runtime_error("SSL client context creation failed");
    }
    // Optional: Configure certificate verification
    // SSL_CTX_set_verify(_ssl_ctx, SSL_VERIFY_PEER, nullptr);
    // if (!SSL_CTX_load_verify_locations(_ssl_ctx, "/path/to/ca/bundle.pem", nullptr)) {
    //     // Handle error
    // }
    // Remember to free context in destructor
    ```
2.  **Create Client Actor:** Inherit from `qb::io::use<...>::tcp::ssl::client`.
3.  **Initialize Transport:** Pass the context to the transport:
    ```cpp
    // Inside client actor constructor or onInit:
    transport().init(_ssl_ctx);
    ```
4.  **Connect:** Call `transport().connect_v4(...)` or `async::tcp::connect<qb::io::tcp::ssl::socket>(...)`. Provide the hostname if Server Name Indication (SNI) is required.
5.  **Handshake:** The SSL handshake happens automatically during the connection process (synchronously for blocking connect, asynchronously for async connect via the `connected()` method).
6.  **Start:** Call `start()` to begin processing I/O events on the secure connection.
7.  **Communicate:** Use `*this << ...` or `in()`/`out()` as with regular TCP; encryption/decryption is handled by the transport.

**(Ref:** `test-async-io.cpp` (SSL test), `test-session-text.cpp` (Secure test)**) 