# QB-IO: Protocols

Protocols define how raw byte streams from transports (like TCP) are structured and interpreted as distinct messages. `qb-io` provides a flexible framework for using built-in protocols or defining custom ones.

## The `AProtocol` Interface

(`qb/include/qb/io/async/protocol.h`)

This is the abstract base class all protocols must inherit from. It uses CRTP (`AProtocol<YourIOClass>`).

*   **Role:** To examine the incoming byte stream (in the I/O component's input buffer) and identify complete messages.
*   **Key Methods to Implement:**
    *   **`getMessageSize()`:** Checks the input buffer (`this->_io.in()`) for the next complete message according to the protocol's framing rules. Returns the total message size (including any delimiters or headers) if found, `0` otherwise.
    *   **`onMessage(size_t size)`:** Called by the framework when `getMessageSize()` returns > 0. Processes the complete message of `size` bytes from the buffer's beginning. This method typically parses the data and invokes the I/O component's `on(Protocol::message&&)` handler.
    *   **`reset()`:** Clears any internal parsing state (e.g., partially read header).
*   **`using message = YourMessageType;`:** The protocol must define this type alias to specify the structure passed to the I/O component's `on()` handler.

## Built-in Protocols

`qb-io` offers several ready-to-use protocols:

**(Located in:** `qb/include/qb/io/protocol/base.h`, `text.h`, `json.h`**)**

### 1. Delimiter-Based (`base::byte_terminated`, `base::bytes_terminated`)

*   **Use Case:** Messages separated by a known single character or byte sequence.
*   **`base::byte_terminated<IO_, DelimiterChar>`:** Uses a single character (e.g., `'\n'`, `'\0'`).
*   **`base::bytes_terminated<IO_, Trait>`:** Uses a byte sequence defined in a `Trait` struct (e.g., `"\r\n"`).

### 2. Size-Header-Based (`base::size_as_header`)

*   **Use Case:** Each message payload is prefixed with its length.
*   **`base::size_as_header<IO_, SizeType>`:** `SizeType` can be `uint8_t`, `uint16_t`, `uint32_t`. Handles network byte order conversion (e.g., `htons`).

### 3. Text Protocols (`qb::protocol::text`)

Convenience wrappers around base protocols, providing parsed text.

*   **`text::string<IO_>`:** Null-terminated (`\0`), provides `std::string`.
*   **`text::command<IO_>`:** Newline-terminated (`\n`), provides `std::string`.
*   **`text::string_view<IO_>`:** Null-terminated (`\0`), provides `std::string_view` (zero-copy reading).
*   **`text::command_view<IO_>`:** Newline-terminated (`\n`), provides `std::string_view` (zero-copy reading).

### 4. Binary Protocols (`qb::protocol::text` Aliases)

Convenience wrappers around `size_as_header`.

*   **`text::binary8<IO_>`:** `uint8_t` size prefix. Message data is `const char*`.
*   **`text::binary16<IO_>`:** `uint16_t` size prefix. Message data is `const char*`.
*   **`text::binary32<IO_>`:** `uint32_t` size prefix. Message data is `const char*`.

### 5. JSON Protocols (`qb::protocol::json`)

*   **`protocol::json<IO_>`:** Null-terminated JSON string. Parses into `nlohmann::json`.
*   **`protocol::json_packed<IO_>`:** Null-terminated MessagePack data. Parses into `nlohmann::json`.

## How to Use a Protocol with Async IO Components

Protocols are typically used within classes inheriting from `qb::io::async` bases (often via `qb::io::use<>`).

1.  **Declare:** Inside your class (e.g., `MyTcpSession`), define the protocol type:
    ```cpp
    #include <qb/io/async.h>
    #include <qb/io/protocol/text.h>

    class MyTcpSession : public qb::io::use<MyTcpSession>::tcp::client<MyServer> {
    public:
        // *** Declare the protocol to use ***
        using Protocol = qb::protocol::text::command<MyTcpSession>;

        explicit MyTcpSession(MyServer& server) : client(server) {
            // Protocol is automatically instantiated by the base class
        }
        // ...
    };
    ```
2.  **Implement Handler:** Implement the `on()` method matching the protocol's `message` type:
    ```cpp
    void MyTcpSession::on(Protocol::message&& msg) { // Note: msg often passed by rvalue-ref
        // msg.text contains the received command (string without newline)
        std::cout << "Received: " << msg.text << std::endl;
        processCommand(msg.text);
    }
    ```
3.  **Send Data:** When sending, format data according to the protocol rules. For delimiter-based protocols, append the delimiter. For size-based, prepend the size header.
    ```cpp
    void MyTcpSession::sendCommand(const std::string& cmd) {
        // For text::command (newline delimited)
        *this << cmd << Protocol::end; // Protocol::end is '\n' here
    }

    void MyBinarySession::sendData(const char* data, uint16_t size) {
        // For text::binary16 (uint16_t size prefix)
        // The base protocol provides a static helper for the header
        auto header = Protocol::Header(size);
        // Send header then data
        publish(reinterpret_cast<const char*>(&header), sizeof(header));
        publish(data, size);
    }
    ```

**(Ref:** `test-session-text.cpp`, `test-session-json.cpp`, `chat_tcp/shared/Protocol.h`, `message_broker/shared/Protocol.h`**)

## How to Implement a Custom Protocol

Referenced `example6_custom_protocol.cpp` and `chat_tcp/shared/Protocol.h` / `message_broker/shared/Protocol.h` are excellent starting points.

1.  **Define Header/Message Structs:** Define C++ structs representing your message header (if any) and the data structure your `on(message&&)` handler will receive.
    ```cpp
    namespace my_protocol {
        struct MyHeader { uint32_t length; uint16_t type; /* ... */ };
        struct MyMessage { MyHeader header; std::string payload; };
    }
    ```
2.  **Create Protocol Class:**
    ```cpp
    #include <qb/io/async/protocol.h>

    template<typename IO_>
    class MyCustomProtocol : public qb::io::async::AProtocol<IO_> {
    private:
        my_protocol::MyHeader _current_header;
        bool _reading_header = true;
        // Other state needed for parsing...

    public:
        using message = my_protocol::MyMessage; // Your message struct

        explicit MyCustomProtocol(IO_& io) noexcept : AProtocol<IO_>(io) {}

        std::size_t getMessageSize() noexcept override {
            auto& buffer = this->_io.in();
            constexpr size_t HEADER_SIZE = sizeof(my_protocol::MyHeader);

            if (_reading_header) {
                if (buffer.size() < HEADER_SIZE) return 0; // Need more data for header
                std::memcpy(&_current_header, buffer.cbegin(), HEADER_SIZE);
                // TODO: Validate header fields (e.g., magic number, version)
                // If invalid: call reset(); return 0;
                _reading_header = false;
            }

            // Now we have the header, check if full payload is available
            size_t total_message_size = HEADER_SIZE + _current_header.length;
            if (buffer.size() < total_message_size) return 0; // Need more payload data

            // Full message is available
            return total_message_size;
        }

        void onMessage(std::size_t size) noexcept override {
            auto& buffer = this->_io.in();
            constexpr size_t HEADER_SIZE = sizeof(my_protocol::MyHeader);

            // Construct your message object
            message msg;
            msg.header = _current_header; // Copy header read in getMessageSize
            if (_current_header.length > 0) {
                // Copy payload (adjust if using string_view)
                msg.payload.assign(buffer.cbegin() + HEADER_SIZE, _current_header.length);
            }

            // Dispatch to the IO object's handler
            this->_io.on(std::move(msg)); // Use move if appropriate

            // Reset state for the next message
            reset();
        }

        void reset() noexcept override {
            _reading_header = true;
            // Reset other internal state if any
        }
    };
    ```
3.  **Serialization (Sending):** Provide a way to serialize your `MyMessage` struct into bytes (header + payload) when sending.
    *   You can overload `operator<<` for your I/O component or provide a static `serialize` function.
    *   Consider specializing `qb::allocator::pipe<char>::put<MyMessage>` for efficient serialization directly into the output buffer (see `chat_tcp/shared/Protocol.cpp` or `message_broker/shared/Protocol.cpp`).

**(Ref:** `example6_custom_protocol.cpp`, `chat_tcp/shared/Protocol.*`, `message_broker/shared/Protocol.*`**) 