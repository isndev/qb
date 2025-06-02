@page ref_faq_md QB Actor Framework: Frequently Asked Questions (FAQ)
@brief Quick answers to common questions about using and understanding the QB Actor Framework.

# QB Actor Framework: Frequently Asked Questions (FAQ)

This FAQ provides answers to common questions about the QB Actor Framework, its design, and usage.

**Q: Is QB thread-safe? How do I manage state in actors?**

A: The QB Actor Model provides inherent thread-safety for an actor's *internal state* (its member variables). Because each actor processes events sequentially on its assigned `VirtualCore`, you don't need traditional locks (mutexes) to protect an actor's own data from concurrent access *by its own event handlers or callbacks*. State encapsulation is key: keep actor state private and modify it only in response to events.

   However, if your actor interacts with: 
   *   **External shared resources** (global variables, data structures shared between actors or threads outside QB's management).
   *   **Third-party libraries not designed for this actor model.**
   Then, *you* are responsible for ensuring thread-safe access to those external resources. Consider using a dedicated "manager actor" to serialize access to such shared resources or use standard C++ synchronization primitives (`std::mutex`, `std::atomic`) for those specific external interactions.
   **(See:** [Core Concept: The Actor Model in QB](./../2_core_concepts/actor_model.md), [QB Framework: Effective Resource Management](./../6_guides/resource_management.md)**)**

**Q: What's the difference between `push<Event>()` and `send<Event>()` for sending events? Which should I use?**

A: **Almost always use `push<Event>()`.**
   *   **`push<Event>(dest, ...)`:** This is the default and recommended method. It guarantees **ordered delivery** of events from a specific sender to a specific receiver (FIFO per sender-receiver pair). It also correctly handles events with non-trivially destructible members (like `std::string`, `std::vector`, `std::shared_ptr`).
   *   **`send<Event>(dest, ...)`:** This method provides **unordered delivery**. It *may* offer slightly lower latency for same-core communication in highly specific scenarios but offers no ordering guarantees. Crucially, events sent via `send()` **must be trivially destructible** (e.g., contain only POD types or `qb::string<N>`).
   **Use `send()` with extreme caution** and only if all its conditions are met and profiling shows a significant, necessary benefit. Incorrect use can lead to very difficult-to-debug ordering issues.
   **(See:** [QB-Core: Event Messaging Between Actors](./../4_qb_core/messaging.md)**)**

**Q: Why is `qb::string<N>` recommended over `std::string` for event members?**

A: `qb::string<N>` is recommended for direct string members in events primarily for **ABI (Application Binary Interface) stability and performance for small strings.**
   *   `std::string`'s internal layout (especially with Small String Optimization - SSO) can vary between compilers, standard library versions, or build configurations. If QB's event system copies/moves event objects containing raw `std::string` members across such ABI boundaries (e.g., between different modules, or in advanced inter-process scenarios), it could lead to data corruption or crashes.
   *   `qb::string<N>` has a predictable, self-contained layout (it's based on `std::array<char, N+1>`), avoiding these ABI issues for event data. It also avoids heap allocations for strings up to size `N`.
   *   If you need to pass dynamically sized or very large string data, it's best to use `std::shared_ptr<std::string>` or `std::unique_ptr<std::string>` within your event. In this case, `std::string` is fine for the heap-allocated data because QB interacts with the smart pointer object itself.
   **(See:** [QB-Core: Event Messaging Between Actors](./../4_qb_core/messaging.md) for detailed data handling in events.**)**

**Q: My actor seems unresponsive or my system is slow. What are common causes?**

A:
1.  **Blocking Operations in Actors:** The most common culprit. Ensure **no actor performs long-running computations or blocking I/O calls** (e.g., traditional file reads, synchronous network calls, `std::this_thread::sleep_for`) directly within its `on(Event&)` handlers or `onCallback()` method. A blocked actor stalls its entire `VirtualCore`.
    *   **Solution:** Offload blocking work using `qb::io::async::callback`, or delegate to dedicated worker actors (potentially on different cores with higher latency settings). Use `qb-io` for all network/file I/O from actors.
2.  **`VirtualCore` Latency Settings:** If `CoreInitializer::setLatency()` is set to a high value, event processing will be delayed when the core is idle. For latency-sensitive actors, ensure their core has `0` or very low latency.
3.  **CPU Saturation:** Check if any `VirtualCore` is consistently at 100% CPU. This could indicate an actor in a tight loop, or a core with `0` latency that is under-utilized (paradoxically, a 0-latency core with little work still spins). Profile to identify the busy actor(s).
4.  **Deadlocks (External or Logical):** While the actor model prevents data races on actor state, logical deadlocks can still occur if actors are waiting for responses from each other in a cyclic dependency without timeouts.
5.  **Resource Exhaustion:** Check for system-level issues like running out of memory, file descriptors, or network sockets.
6.  **Inefficient Event/Data Handling:** Passing very large event objects by value frequently, or inefficient algorithms within event handlers, can degrade performance.
**(See:** [QB Framework: Performance Tuning Guide](./../6_guides/performance_tuning.md), [QB-Core: Engine - qb::Main & VirtualCore](./../4_qb_core/engine.md)**)**

**Q: How do actors discover each other if they don't share memory?**

A: Actors discover each other's `qb::ActorId` (their "address") through several mechanisms:
1.  **At Creation Time:** When an actor is created (e.g., `main.addActor<MyChild>(...)`), its `ActorId` can be passed to other actors (e.g., its parent or a manager) that need to communicate with it.
2.  **In Event Payloads:** An actor can include its `id()` in an event it sends, allowing the recipient to know who the sender was and reply.
3.  **`ServiceActor<Tag>` Pattern:** For well-known, core-local services.
    *   Actors on the same core can get a direct pointer: `MyService* svc = getService<MyService>();`
    *   Actors on any core can get the ID: `qb::ActorId svc_id = getServiceId<MyServiceTag>(target_core_id);`
4.  **`require<ActorType>()` Mechanism:** An actor can call `require<OtherActorType>();` to request the framework to notify it of any live instances of `OtherActorType`. It will receive `qb::RequireEvent` messages containing the `ActorId`s of found instances.
5.  **Custom Registry/Discovery Actor:** You can implement a dedicated actor that acts as a naming service or registry where other actors can register themselves (with a logical name or type) and query for others.
**(See:** [QB-Core: Common Actor Patterns & Utilities](./../4_qb_core/patterns.md)**)**

**Q: How should I handle large data payloads in events?**

A: **Avoid copying large data directly within event structs.** Instead, pass the data using smart pointers when sending events via `push()`:
   *   **`std::shared_ptr<MyLargeData>`:** If the data might be referenced by multiple actors or its lifetime is shared.
   *   **`std::unique_ptr<MyLargeData>`:** If you want to transfer exclusive ownership of the data to the recipient. The sender's `unique_ptr` will become null.
   This ensures only the small smart pointer object is copied/moved during event transfer, not the potentially massive underlying data block.
   When sending very large events, also consider using `pipe.allocated_push<MyEvent>(size_hint, ...)` to pre-allocate buffer space in the communication pipe, potentially reducing reallocations.
```cpp
struct LargeDataEvent : qb::Event {
    std::shared_ptr<std::vector<char>> data_buffer;
    // ... constructor ...
};

// Sender:
auto my_payload = std::make_shared<std::vector<char>>(1024 * 1024); // 1MB
// ... fill payload ...
push<LargeDataEvent>(recipient_id, my_payload);

// Receiver:
void on(const LargeDataEvent& event) {
    if (event.data_buffer) { /* process *event.data_buffer */ }
}
```
**(See:** [QB-Core: Event Messaging Between Actors](./../4_qb_core/messaging.md)**)**

**Q: Can I use blocking third-party libraries with QB actors?**

A: **Not directly within an actor's `on(Event&)` handlers or `onCallback()` methods.** Any call that blocks the thread will stall the actor's `VirtualCore`, impacting all other actors on that core.
   **Solutions:**
   1.  **Wrap in `qb::io::async::callback`:** For short, infrequent blocking calls. The callback itself will execute the blocking call, and while that specific callback is running, it will occupy its turn on the `VirtualCore`. However, the original event handler that scheduled it returns immediately, allowing other events to be processed sooner.
   2.  **Dedicated Worker Actors:** Create a pool of specialized worker actors (potentially on `VirtualCore`s configured with higher latency) to handle interactions with blocking libraries. Your primary actors send request events to these workers, which perform the blocking operation and then send response events back.
   **(See:** [Integrating Core & IO: Asynchronous Operations within Actors](./../5_core_io_integration/async_in_actors.md), `example/core_io/file_processor/`**)**

**Q: Does QB provide built-in actor supervision (like Erlang/Akka)?**

A: No, QB-Core does not provide a built-in, ready-to-use supervision hierarchy like Erlang/OTP or Akka. Developers are responsible for implementing supervision strategies using standard actor patterns. This typically involves a supervisor actor monitoring its children (e.g., via heartbeats or explicit failure notifications) and deciding on recovery actions (restart, delegate, escalate).
   **(See:** [Guides: Error Handling & Resilience Strategies](./../6_guides/error_handling.md)**)**

**Q: How is QB different from other C++ actor frameworks?**

A: (This is a general comparison; specific features vary widely across frameworks.)
*   **Deep `qb-io` Integration:** QB is built with its own high-performance asynchronous I/O library (`qb-io`) as a core, inseparable foundation, influencing its design for network and I/O-intensive tasks.
*   **Performance Focus:** Emphasis on low-level control where needed (e.g., core affinity, latency settings, `qb::string`, MPSC queues for inter-core) combined with high-level actor abstractions.
*   **C++17 Idioms:** Leverages modern C++ features for type safety and efficiency.
*   **Flexibility:** While providing the Actor Model, it doesn't enforce overly rigid lifecycle or supervision patterns by default, giving developers control.
*   **Lean Dependencies:** Relies on a few carefully selected, often bundled, high-quality C/C++ libraries (`libev`, `ska_hash`, `stduuid`, `nlohmann/json`) with optional dependencies for SSL and Zlib.

**Q: Where can I find the most detailed API documentation?**

A: The **source code header files** (`.h`, `.hpp`, `.tpp`, `.inl`) are the definitive reference for all class members, method signatures, template parameters, and internal implementation details. The Doxygen-generated documentation (which you are reading) aims to make this information accessible and understandable, but the headers are the ground truth.
   The [QB Framework: Detailed API Overview](./api_overview.md) page also serves as a good starting map. 