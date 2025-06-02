@page qb_io_protocols_md QB-IO: Framing Messages with Protocols
@brief Learn how `qb-io` uses protocols to define message boundaries and parse data from byte streams.

# QB-IO: Framing Messages with Protocols

Network transports like TCP deliver a continuous stream of bytes. To make sense of this stream, applications need a way to identify where one logical message ends and the next begins—a process called **message framing**. In `qb-io`, this is the job of **protocols**.

Protocols define the rules for interpreting raw byte streams, enabling `qb-io` components to extract meaningful application-level messages.

## The `AProtocol<IO_Type>` Interface: Your Protocol Blueprint

(`qb/io/async/protocol.h`)

All custom and built-in protocols in `qb-io` are built upon the `qb::io::async::AProtocol<IO_Type>` abstract base class. This class uses the Curiously Recurring Template Pattern (CRTP), where `IO_Type` is the specific I/O component (e.g., your TCP session class) that will use the protocol.

**Key Responsibilities & Methods to Implement in a Custom Protocol:**

1.  **`using message = YourMessageType;`**
    *   Your protocol **must** define a nested type alias named `message`. This `YourMessageType` (often a struct) represents the structure of a fully parsed application message that your protocol will produce.

2.  **`std::size_t getMessageSize() noexcept override;`**
    *   **Purpose:** Inspects the incoming byte stream (available via `this->_io.in()`, which is the input buffer of your `IO_Type` class) to determine if a complete message is present according to the protocol's framing rules.
    *   **Return Value:** If a complete message is found, it returns the **total size of that message in bytes** (including any headers, delimiters, or length fields that are part of the message unit as it exists in the buffer). If a complete message is not yet available, it must return `0`.
    *   **Non-Consuming:** This method should *only inspect* the buffer; it must not remove (flush) data.

3.  **`void onMessage(std::size_t size) noexcept override;`**
    *   **Purpose:** Called by the `qb-io` framework when `getMessageSize()` has returned a non-zero `size`.
    *   **Action:** This method is responsible for actually processing the complete message of `size` bytes from the beginning of the input buffer (`this->_io.in()`). This typically involves:
        *   Parsing the raw bytes into your `Protocol::message` struct.
        *   Dispatching this parsed message to a handler in your `IO_Type` class, usually by calling `this->_io.on(typename Protocol::message{/* parsed data */});`.
    *   The framework will automatically call `this->_io.flush(size)` after `onMessage` returns to remove the processed message from the input buffer.

4.  **`void reset() noexcept override;`**
    *   **Purpose:** Clears any internal parsing state within the protocol instance. This is crucial after successfully parsing a message, encountering a parsing error, or when the connection is reset, to ensure the protocol is ready for the next message from a clean slate.

## Built-in Protocols: Ready for Common Tasks

`qb-io` provides several pre-built protocols for common framing strategies, saving you from reinventing the wheel. These are found in `qb/io/protocol/base.h`, `qb/io/protocol/text.h`, and `qb/io/protocol/json.h`.

### 1. Delimiter-Based Protocols
   (From `qb/io/protocol/base.h` and `qb/io/protocol/text.h`)

*   **Use Case:** Messages are separated by a known character or sequence of bytes.
*   **`qb::protocol::base::byte_terminated<IO_Type, DelimiterChar>`:**
    *   Frames messages based on a single `DelimiterChar` (e.g., `'\0'` for null-terminated strings, `'\n'` for line-based messages).
*   **`qb::protocol::base::bytes_terminated<IO_Type, TraitStruct>`:**
    *   Frames messages based on a sequence of bytes defined in `TraitStruct::_EndBytes` (e.g., `"\r\n\r\n"` for HTTP-like headers).
*   **Convenience Text Aliases (from `qb::protocol::text`):**
    *   `text::string<IO_Type>`: Null-terminated (`'\0'`), produces `std::string` in `Protocol::message`.
    *   `text::command<IO_Type>`: Newline-terminated (`'\n'`), produces `std::string`.
    *   `text::string_view<IO_Type>`: Null-terminated, produces `std::string_view` (zero-copy for payload).
    *   `text::command_view<IO_Type>`: Newline-terminated, produces `std::string_view` (zero-copy for payload).

### 2. Size-Header-Based Protocols
   (From `qb/io/protocol/base.h` and `qb/io/protocol/text.h`)

*   **Use Case:** Each message payload is preceded by a fixed-size integer header indicating the payload's length.
*   **`qb::protocol::base::size_as_header<IO_Type, SizeHeaderType>`:**
    *   `SizeHeaderType` can be `uint8_t`, `uint16_t`, or `uint32_t`.
    *   Automatically handles network byte order conversion (e.g., `ntohs`, `ntohl`) for 16-bit and 32-bit headers.
*   **Convenience Binary Aliases (from `qb::protocol::text`):**
    *   `text::binary8<IO_Type>`: Uses `uint8_t` for payload size. `Protocol::message` contains `const char* data` and `size_t size`.
    *   `text::binary16<IO_Type>`: Uses `uint16_t` for payload size.
    *   `text::binary32<IO_Type>`: Uses `uint32_t` for payload size.

### 3. JSON Protocols
   (From `qb/io/protocol/json.h`)

*   **`qb::protocol::json<IO_Type>`:**
    *   Expects null-terminated JSON strings.
    *   `Protocol::message` contains the raw `const char* data`, `size_t size`, and a `nlohmann::json json` object holding the parsed JSON.
*   **`qb::protocol::json_packed<IO_Type>`:**
    *   Expects null-terminated, MessagePack-encoded JSON data.
    *   `Protocol::message` is the same as for `protocol::json`, with `nlohmann::json::from_msgpack()` used for deserialization.

## Using Protocols with Asynchronous I/O Components

Protocols are typically integrated into classes that handle asynchronous I/O, often those derived using the `qb::io::use<>` helper template.

1.  **Declare the Protocol Type:** Inside your I/O component class (e.g., `MyTCPSession` inheriting from `qb::io::use<MyTCPSession>::tcp::client`), define your chosen protocol as a nested type alias named `Protocol`.
    ```cpp
    #include <qb/io/async.h>      // For qb::io::use<>
    #include <qb/io/protocol/text.h> // For text::command
    #include <iostream>

    class MyTCPSession : public qb::io::use<MyTCPSession>::tcp::client</*optional ServerType*/> {
    public:
        // *** 1. Declare the protocol to be used by this session ***
        using Protocol = qb::protocol::text::command<MyTCPSession>;

        explicit MyTCPSession(/* constructor args */) /* : client(args) */ {
            // The base qb::io::use<...>::tcp::client will often automatically instantiate
            // your declared Protocol if it has a constructor taking MyTCPSession&.
            // If not, or for more control, you might call:
            // this->switch_protocol<Protocol>(*this);
        }

        // ... other methods ...
    };
    ```

2.  **Implement the Message Handler:** Your I/O component class must implement a public method `void on(Protocol::message&& msg)` (or `void on(const Protocol::message& msg)`) to receive and process fully parsed messages from the protocol.
    ```cpp
    // Continuing MyTCPSession from above
    public:
        // *** 2. Implement the handler for messages parsed by your Protocol ***
        void on(Protocol::message&& received_command) { // text::command::message is {size, data, text}
            std::cout << "Received command: " << received_command.text << std::endl;
            if (received_command.text == "QUIT") {
                this->disconnect();
            }
            // Process the command...
        }

        void on(qb::io::async::event::disconnected const& event) {
            std::cout << "Disconnected. Reason: " << event.reason << std::endl;
        }
    };
    ```

3.  **Sending Data According to Protocol:** When sending data, ensure it conforms to the chosen protocol's framing rules.
    ```cpp
    // Inside MyTCPSession or another class interacting with it
    void sendCommandToServer(MyTCPSession& session, const std::string& command_text) {
        // For text::command, messages are newline-terminated.
        // Protocol::end is typically defined by byte_terminated based protocols.
        session << command_text << MyTCPSession::Protocol::end; 
    }

    // For a binary protocol (e.g., text::binary16)
    // void sendBinaryData(MyBinarySession& session, const char* data, uint16_t len) {
    //     auto header = MyBinarySession::Protocol::Header(len); // Get network-byte-order header
    //     session.publish(reinterpret_cast<const char*>(&header), sizeof(header));
    //     session.publish(data, len);
    // }
    ```

**(Reference examples:** `test-session-text.cpp`, `test-session-json.cpp`, `chat_tcp/shared/Protocol.h`, `message_broker/shared/Protocol.h`**)

## Implementing a Custom Protocol: A Step-by-Step Guide

When the built-in protocols don't fit your application's specific message structure, `qb-io` makes it straightforward to define your own. Here's how:

1.  **Define Your Message Structure(s):**
    First, determine what constitutes a "message" in your protocol. This might involve a header part and a payload part. Define C++ structs or classes for these.

    ```cpp
    // In a suitable header, e.g., my_custom_protocol_messages.h
    namespace my_app {
        // Example: A header structure for your messages
        struct MessageHeader {
            uint32_t magic_number; // To identify your protocol
            uint16_t message_type; // To differentiate kinds of messages
            uint32_t payload_length;
            // Add other fixed-size header fields as needed (checksum, sequence no, etc.)
        };
        constexpr uint32_t MY_PROTOCOL_MAGIC = 0xABCD1234;

        // Example: The structure your IO component will receive
        struct ParsedMessage {
            MessageHeader header;
            std::vector<char> payload; // Or std::string, or a shared_ptr to a buffer
            // Add other parsed fields if your payload has structure
        };
    } // namespace my_app
    ```

2.  **Create Your Protocol Class:**
    Inherit from `qb::io::async::AProtocol<YourIOComponent>` and implement the required methods. `YourIOComponent` is the class (e.g., your TCP session handler) that will use this protocol.

    ```cpp
    // In your protocol header, e.g., my_custom_protocol.h
    #include <qb/io/async/protocol.h>
    #include "my_custom_protocol_messages.h" // Your message structs
    #include <cstring> // For std::memcpy
    #include <vector>  // For std::vector in ParsedMessage

    // Forward declare your IO component if necessary, or include its header
    // class YourIOComponent; 

    template<typename YourIOComponent>
    class MyCustomProtocol : public qb::io::async::AProtocol<YourIOComponent> {
    private:
        // Internal state for parsing
        my_app::MessageHeader _current_header;
        bool _reading_header = true;
        static constexpr size_t HEADER_SIZE = sizeof(my_app::MessageHeader);

    public:
        // *** This is crucial: Define what your IO_Type::on() will receive ***
        using message = my_app::ParsedMessage;

        // Constructor: Takes a reference to the IO component that owns it
        explicit MyCustomProtocol(YourIOComponent& io_component) noexcept 
            : qb::io::async::AProtocol<YourIOComponent>(io_component) {}

        // --- Implementation of AProtocol virtual methods ---

        std::size_t getMessageSize() noexcept override {
            auto& input_buffer = this->_io.in(); // Access the IO component's input buffer

            if (_reading_header) {
                if (input_buffer.size() < HEADER_SIZE) {
                    return 0; // Not enough data for the header yet
                }
                // Copy header data from buffer
                std::memcpy(&_current_header, input_buffer.cbegin(), HEADER_SIZE);
                
                // Basic validation (e.g., magic number)
                if (_current_header.magic_number != my_app::MY_PROTOCOL_MAGIC) {
                    //qb::io::cerr() << "Invalid magic number!" << std::endl;
                    // Protocol error: How to handle?
                    // Option 1: Mark protocol as bad, IO component should disconnect.
                    this->not_ok(); // Mark protocol as not okay
                    // Option 2: Try to find next magic number (resynchronize) - more complex.
                    // For now, let's assume we disconnect on error.
                    return 0; // Or indicate an error that leads to flushing this data
                }
                _reading_header = false;
            }

            // Now we have the header, check if the full payload has arrived
            size_t total_message_size = HEADER_SIZE + _current_header.payload_length;
            if (input_buffer.size() < total_message_size) {
                return 0; // Not enough data for the full payload yet
            }

            // A complete message (header + payload) is available
            return total_message_size;
        }

        void onMessage(std::size_t total_message_size) noexcept override {
            auto& input_buffer = this->_io.in();
            
            // Construct the message object to pass to the IO component's handler
            my_app::ParsedMessage received_msg;
            received_msg.header = _current_header; // Header was already read in getMessageSize

            if (_current_header.payload_length > 0) {
                // Copy payload from the input buffer
                const char* payload_start = input_buffer.cbegin() + HEADER_SIZE;
                received_msg.payload.assign(payload_start, payload_start + _current_header.payload_length);
            }
            
            // Dispatch the fully parsed message to the IO component
            // YourIOComponent must have a method: void on(my_app::ParsedMessage&& msg)
            this->_io.on(std::move(received_msg)); 

            // Reset state for the next message
            reset();
        }

        void reset() noexcept override {
            _reading_header = true;
            // Clear any other partial parsing state
            _current_header = {}; 
        }
    };
    ```

3.  **Integrate with Your I/O Component:**
    In your class that handles the I/O (e.g., `MyTCPSession`):
    ```cpp
    class MyTCPSession : public qb::io::use<MyTCPSession>::tcp::client</*...*/> {
    public:
        // Declare your custom protocol
        using Protocol = MyCustomProtocol<MyTCPSession>;

        explicit MyTCPSession(/*...*/) {
            // Protocol is often instantiated by the `use<>` base if it has a constructor
            // taking `YourIOComponent&`. If not, you might need:
            this->switch_protocol<Protocol>(*this);
        }

        // Implement the handler for your parsed message type
        void on(my_app::ParsedMessage&& msg) {
            // Process the structured message
            // qb::io::cout() << "Received message type: " << msg.header.message_type
            //                << " with payload size: " << msg.header.payload_length << std::endl;
        }
        // ... other handlers (disconnected, etc.)
    };
    ```

4.  **Serialization (Sending Data):**
    When sending data, you'll need to construct the byte stream according to your protocol (header + payload).
    *   **Manual Construction:**
        ```cpp
        // Inside MyTCPSession or another class that sends
        void sendMyData(const std::vector<char>& payload_data, uint16_t type) {
            my_app::MessageHeader header;
            header.magic_number = my_app::MY_PROTOCOL_MAGIC;
            header.message_type = type;
            header.payload_length = static_cast<uint32_t>(payload_data.size());
            // header.id = ...; // Set other header fields

            // Send header
            this->publish(reinterpret_cast<const char*>(&header), sizeof(header));
            // Send payload
            if (!payload_data.empty()) {
                this->publish(payload_data.data(), payload_data.size());
            }
        }
        ```
    *   **Helper Functions/Classes:** You might create helper functions or a dedicated "Serializer" class to make this cleaner.
    *   **`qb::allocator::pipe<char>::put` Specialization (Advanced):** For very high performance, you can specialize `qb::allocator::pipe<char>::put<YourMessageType>` to serialize your message type directly into the output buffer without intermediate copies. The `chat_tcp` and `message_broker` examples demonstrate this technique.

**Important Considerations for Custom Protocols:**

*   **Error Handling:** Robustly handle malformed data in `getMessageSize()`. Decide on a strategy: disconnect, try to resynchronize, or log and skip. Marking the protocol as `not_ok()` can signal the I/O component.
*   **State Management:** `reset()` is critical to ensure that partial parsing state from a previous message (or erroneous data) doesn't affect the next one.
*   **Efficiency:** For performance-sensitive protocols, be mindful of data copies. `std::string_view` can be useful for payload if its lifetime is managed carefully.
*   **Endianness:** If your header contains multi-byte integers (like `uint32_t payload_length`), ensure you consistently use network byte order (big-endian) when serializing and deserialize correctly (e.g., using `ntohl`, `htonl`, or `qb::endian` utilities) if your systems might have different native endianness.

**(Reference Examples:** The `example/io/example5_custom_protocol.cpp` provides a good starting point. For more advanced serialization and protocol design, examine the protocols in `example/core_io/chat_tcp/shared/Protocol.h` and `example/core_io/message_broker/shared/Protocol.h`.**)

By understanding and utilizing this protocol framework, you can build robust and flexible communication layers for your `qb-io` based applications.

**(Next:** [QB-IO: Secure TCP (SSL/TLS) Transport](./ssl_transport.md) or [QB-IO: Utilities](./utilities.md)**) 