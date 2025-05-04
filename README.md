<p align="center"><img src="./ressources/logo.svg" width="180px" alt="QB Actor Framework Logo" /></p>

<h1 align="center">QB C++ Actor Framework</h1>

<p align="center">
  <strong>Build High-Performance, Scalable Concurrent C++ Applications with Ease.</strong>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-Apache%202.0-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17/20-blue.svg" alt="Cpp Standard">
  <!-- TODO: Add build status, latest release badges -->
</p>

---

**Tired of the complexities of traditional C++ concurrency?** Mutexes, condition variables, deadlocks, and race conditions making development slow and error-prone? **QB offers a better way.**

QB is a modern C++17/20 **Actor Framework** built from the ground up for **performance and developer productivity**. It embraces the Actor Model to provide:

*   **Simplified Concurrency:** Manage state and interactions through isolated actors communicating via asynchronous messages. Forget manual locking.
*   **High Throughput & Low Latency:** Achieve exceptional performance thanks to an efficient, non-blocking asynchronous I/O core (`qb-io`) and optimized message passing.
*   **Scalability:** Naturally scale your application across multiple CPU cores.
*   **Resilience:** Build robust systems where failures can be isolated and managed.

QB empowers you to write complex concurrent and distributed systems that are easier to reason about, test, and maintain.

## Why Choose QB? 🤔

*   🚀 **Built for Speed:** Lock-free inter-core communication, efficient event loop (`libev` based), minimal overhead abstractions.
*   🎭 **Intuitive Actor Model:** Focus on your application logic, not low-level threading details. Actors encapsulate state and behavior cleanly.
*   🛡️ **Modern & Type-Safe C++:** Leverage C++17/20 features for expressive, safe, and maintainable code.
*   🔌 **Batteries Included:** Integrated asynchronous I/O for TCP, UDP, SSL/TLS, Files, and Timers.
*   🧱 **Modular:** Use the core actor engine (`qb-core`) or leverage the underlying `qb-io` library independently.
*   🔧 **Extensible:** Supports custom protocols, transports, and provides building blocks for common patterns (Pub/Sub, FSM, Request/Reply).
*   ✅ **Production Ready:** Tested and designed for building reliable, high-performance applications.

## Quick Look: Actor Interaction

```cpp
// Define Events (Messages)
struct CalculateSum : qb::Event { int a, b; };
struct SumResult : qb::Event { int result; };

// Define an Actor
class Calculator : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<CalculateSum>(*this); // Subscribe to CalculateSum events
        return true;
    }

    void on(const CalculateSum& event) {
        int sum = event.a + event.b;
        qb::io::cout() << "Calculator [" << id() << "] calculated " << event.a << " + " << event.b << " = " << sum << "\n";
        // Send the result back to the original sender
        reply(SumResult{sum}); // Efficiently send back using the original event's source
    }
};

// --- In your main setup ---
qb::Main engine;
qb::ActorId calculator_id = engine.addActor<Calculator>(0);

// To send a request (e.g., from another actor or a controller)
// Assuming 'requester_id' is the ActorId of the sender
// engine.core(0).push<CalculateSum>(calculator_id, requester_id, 5, 7);

// The Calculator actor will process the event and send SumResult back
```

## Getting Started 🚀

1.  **Build:** Follow the [Building Guide](./readme/7_reference/building.md).
2.  **Explore:** Run the [Getting Started Example](./readme/6_guides/getting_started.md).
3.  **Learn:** Read the [Core Concepts](./readme/2_core_concepts/README.md).

## Dive Deeper 📚

Explore the comprehensive documentation to master QB:

➡️ **[Full Documentation Index](./readme/README.md)**

*   **[QB-IO Module](./readme/3_qb_io/README.md):** Async I/O, Networking, Protocols
*   **[QB-Core Module](./readme/4_qb_core/README.md):** Actor Engine, Messaging, Lifecycle
*   **[Guides](./readme/6_guides/README.md):** Tutorials, Patterns (FSM, Pub/Sub), Performance Tuning, Error Handling
*   **[Reference](./readme/7_reference/README.md):** API Overview, Build Options, Testing, Glossary

## Contributing ❤️

We welcome contributions! Whether it's reporting a bug, suggesting a feature, improving documentation, or submitting code changes, your help is valued.

Please refer to our **[Contribution Guidelines](./CONTRIBUTING.md)** for details on how to get involved.

## License 📄

QB Actor Framework is licensed under the **Apache License 2.0**.

See the [LICENSE](./LICENSE) file for details.

