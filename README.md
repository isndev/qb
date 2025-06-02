# QB Actor Framework: High-Performance C++17 for Concurrent & Distributed Systems

<p align="center"><img src="./resources/logo.svg" width="180px" alt="QB Actor Framework Logo" /></p>

**Unlock the power of modern C++ for complex concurrent applications. QB is an actor-based framework meticulously engineered for developers seeking exceptional performance, scalability, and a more intuitive way to manage concurrency.**

QB simplifies the art of building responsive, real-time systems, network services, and distributed computations by harmonizing the robust **Actor Model** with a high-efficiency **Asynchronous I/O Engine**. Focus on your application's logic; let QB handle the intricacies of parallelism and non-blocking I/O.

## Why QB? The QB Advantage

QB isn't just another concurrency library; it's a comprehensive solution built on clear design philosophies that directly translate into developer and application benefits:

1.  **Simplified Concurrency with the Actor Model:** Build your system from isolated, stateful actors that communicate via asynchronous messages. This dramatically reduces the complexity of shared-state synchronization, leading to more robust, easier-to-reason-about code. *Spend less time chasing deadlocks and race conditions, more time building features.*
2.  **Blazing-Fast Asynchronous I/O (`qb-io`):** Our non-blocking I/O engine ensures your application never idles waiting for network or file operations. Handle thousands of concurrent connections and I/O tasks with minimal thread overhead and maximum responsiveness. *Keep your application swift and your users happy.*
3.  **Engineered for Extreme Performance & Scalability:** From lock-free inter-core messaging to cache-friendly data structures and minimal-copy event handling, QB is optimized for speed. It's designed to harness the full power of multi-core processors, allowing your applications to scale effortlessly. *Build systems that perform under pressure.*
4.  **Modular & Adaptable Architecture:** QB's layered design means flexibility. Use the full actor system (`qb-core` + `qb-io`) or leverage the powerful `qb-io` library as a standalone toolkit for your asynchronous I/O needs. With extensible protocols, QB adapts to your project, not the other way around. *Integrate QB your way.*
5.  **Modern C++ for Productive Development:** Embrace C++17 with type-safe messaging, clear APIs, and a rich set of utilities (`qb::string`, `qb::allocator::pipe`, crypto, compression) that reduce boilerplate and accelerate development. *Write cleaner, safer, more maintainable C++.* 

**(Explore the guiding principles:** [QB Framework Philosophy: Building Modern C++ Systems](./readme/1_introduction/philosophy.md)**)**

## Key Features at a Glance

QB provides a comprehensive suite of tools for demanding applications:

*   **Core Actor System (`qb-core`):**
    *   **`qb::Actor`:** Lightweight, powerful base for your concurrent components.
    *   **Lifecycle Management:** Managed initialization (`onInit`), termination (`kill`), and unique `qb::ActorId` for every actor.
    *   **Type-Safe Event System:** Asynchronous `qb::Event` messaging with guaranteed ordering (`push`), unordered options (`send`), and broadcast.
    *   **Efficient Inter-Core Communication:** Transparent, high-speed messaging across CPU cores using lock-free MPSC queues.
    *   **Built-in Patterns:** Support for Service Actors (core-local singletons via `qb::ServiceActor`) and periodic tasks (`qb::ICallback`).
*   **High-Performance Asynchronous I/O (`qb-io`):**
    *   **Event Loop:** `libev`-powered `qb::io::async::listener` drives asynchronous operations on each `VirtualCore`.
    *   **Network Transports:** Non-blocking TCP, UDP, and SSL/TLS client/server components.
    *   **Protocol Framework:** Extensible (`qb::io::async::AProtocol`) with built-in parsers for text, binary, and JSON messages.
    *   **Async Utilities:** Timers (`qb::io::async::callback`), file/directory watching, and OS signal handling.
*   **Scalability & Performance:**
    *   **Multi-Core by Design:** `qb::Main` and `qb::VirtualCore`s enable effortless distribution of actors for parallel execution.
    *   **Fine-Grained Tuning:** Control CPU core affinity and event loop latency for optimal performance.
*   **Rich Utility Suite (within `qb-io`):**
    *   **Data Handling:** URI parsing, cryptography (OpenSSL-backed), compression (Zlib-backed).
    *   **Core Utilities:** High-precision time (`qb::TimePoint`, `qb::Duration`), optimized containers (`qb::allocator::pipe`, `qb::string<N>`, `qb::unordered_map`), system information (CPU, endianness).

**(See the full list:** [QB-IO: Feature Showcase](./readme/3_qb_io/features.md) and [QB-Core: Key Features & Capabilities](./readme/4_qb_core/features.md)**)**

## A Quick Peek: Actor Interaction in QB

Here's a very basic example to illustrate the fundamental actor communication style:

```cpp
#include <qb/main.h>    // Main engine controller
#include <qb/actor.h>   // Actor base class, ActorId
#include <qb/event.h>   // Event base class
#include <qb/io.h>      // For qb::io::cout (thread-safe console output)
#include <qb/string.h>  // For qb::string

// Define a simple event
struct GreetingEvent : qb::Event {
    qb::string<64> text;
    
    explicit GreetingEvent(const char* message_text)
        : text(message_text) {}
};

// Define a simple Greeter actor
class GreeterActor : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<GreetingEvent>(*this);
        qb::io::cout() << "GreeterActor [" << id() << "] is ready!\n";
        return true;
    }

    void on(const GreetingEvent& event) {
        qb::io::cout() << "GreeterActor [" << id() << "] received: '" 
                       << event.text.c_str() << "' from Actor [" << event.getSource() << "]\n";
        kill(); // Terminate after receiving one greeting
    }
};

// A simple actor to send the initial greeting
class StarterActor : public qb::Actor {
private:
    qb::ActorId _greeter_id;
public:
    StarterActor(qb::ActorId greeter_id_to_send_to) : _greeter_id(greeter_id_to_send_to) {}

    bool onInit() override {
        // When StarterActor calls push, its own ID will be automatically set as the event's source
        push<GreetingEvent>(_greeter_id, "Hello from StarterActor!");
        kill(); // StarterActor finishes its job and terminates
        return true;
    }
};

int main() {
    qb::Main engine;

    qb::ActorId greeter_id = engine.addActor<GreeterActor>(0);
    engine.addActor<StarterActor>(0, greeter_id);
    
    qb::io::cout() << "Starting QB engine...\n";
    engine.start(false); 

    qb::io::cout() << "QB engine has stopped.\n";
    if (engine.hasError()) {
        qb::io::cout() << "Engine stopped with an error!\n";
        return 1;
    }
    return 0;
}
```
*This snippet is a simplified illustration. In real applications, robust error handling (e.g., checking ActorId validity after creation if critical) and more complex event interactions are typical. Actors are automatically cleaned up on termination; explicit `KillEvent` handling is for custom pre-destruction logic.* 

## Getting Started & Diving Deeper

Ready to harness the power of QB for your C++ projects?

1.  **[Installation & First Application](./readme/6_guides/getting_started.md):** Your primary guide to setting up QB, building the framework, and running your first complete actor-based program.
2.  **[Understanding Core Concepts](./readme/2_core_concepts/README.md):** Grasp the fundamentals: Actors, Events, the Asynchronous I/O Model, and Concurrency in QB.
3.  **Explore the QB Ecosystem:**
    *   **[QB-IO Module In-Depth](./readme/3_qb_io/README.md):** Detailed documentation for the asynchronous I/O library and its utilities.
    *   **[QB-Core Module In-Depth](./readme/4_qb_core/README.md):** Comprehensive information on the actor engine, messaging, and lifecycle.
4.  **Practical Integration & Examples (`Core & IO Integration`):**
    *   **[How Actors Use Asynchronous I/O](./readme/5_core_io_integration/README.md):** Learn how `qb-core` and `qb-io` work together seamlessly.
    *   **[Detailed Example Analyses](./readme/5_core_io_integration/examples/README.md):** Walkthroughs of non-trivial applications like TCP chat servers and distributed task processors.
5.  **Developer Guides & Cookbooks (`Guides`):
    *   **[Practical Design Patterns & Advanced Usage](./readme/6_guides/README.md):** Learn to implement common actor patterns, tune for performance, handle errors robustly, and manage resources effectively.
6.  **Quick Reference (`Reference`):
    *   **[API Overview, Build Instructions, FAQ & More](./readme/7_reference/README.md):** Your go-to for specific API details, build options, testing procedures, and quick answers.

## Documentation Structure

The full documentation is organized into the following main sections within the `readme` directory:

*   `1_introduction/` (Overview, Philosophy)
*   `2_core_concepts/` (Actors, Events, Async I/O, Concurrency)
*   `3_qb_io/` (QB-IO Module Details)
*   `4_qb_core/` (QB-Core Module Details)
*   `5_core_io_integration/` (How Core & IO work together, including `examples/` analyses)
*   `6_guides/` (Getting Started, Patterns, Performance, Error Handling, etc.)
*   `7_reference/` (API Overview, Building, Testing, FAQ, Glossary)

**(The main Doxygen entry point is typically generated from:** [docs/mainpage.h](./docs/mainpage.h)**)**

## Contributing

Contributions to the QB Actor Framework are highly welcome! Whether it's reporting bugs, suggesting new features, improving documentation, or submitting code enhancements, your input is valuable.

Please refer to our `CONTRIBUTING.md` file for detailed guidelines on how to get involved.

## License

QB Actor Framework is licensed under the Apache License, Version 2.0. Please see the `LICENSE` file for the full text.

