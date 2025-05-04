# Frequently Asked Questions (FAQ)

**Q: Is QB thread-safe?**

A: The actor model itself provides inherent thread safety for an actor's *internal state*. Since only one thread (the actor's `VirtualCore`) executes an actor's event handlers and callbacks at a time, and state is not shared directly, you don't need locks to protect member variables *within* an actor. However, if an actor interacts with external resources (global variables, non-actor-managed shared data structures, external libraries not designed for this model), you are responsible for ensuring thread safety for those interactions, potentially using a dedicated manager actor or standard C++ synchronization primitives.

**Q: When should I use `push` vs. `send` for events?**

A: **Almost always use `push`.** `push` guarantees that events sent from actor A to actor B arrive in the order A sent them. `send` offers no ordering guarantees and is primarily an optimization for rare, same-core, unordered, trivially-destructible event scenarios where absolute minimal latency is critical. Using `send` inappropriately can lead to very difficult-to-debug race conditions and ordering issues.

**Q: My actor seems unresponsive or the system is slow. What should I check?**

A:
1.  **Blocking Calls:** Ensure no actor is performing long-running computations or blocking I/O calls directly within `on(Event&)` or `onCallback()`. Offload blocking work using `qb::io::async::callback` or redesign using non-blocking I/O.
2.  **Core Latency:** If using non-zero latency (`setLatency`), ensure it's appropriate for your workload. Very high latency might delay event processing.
3.  **CPU Usage:** Check if any `VirtualCore` is pegged at 100% CPU. This might indicate an actor stuck in a tight loop or a core configured with zero latency that has little work to do (consider increasing latency if appropriate).
4.  **Deadlocks (External):** While the actor model prevents internal deadlocks over state, ensure actors aren't involved in deadlock cycles waiting for responses from each other *without timeouts*.
5.  **Resource Limits:** Check for system resource exhaustion (memory, file descriptors, network sockets).
6.  **Profiling:** Use system profiling tools to identify bottlenecks.

**Q: How do actors discover each other?**

A:
1.  **Construction:** Pass `ActorId`s in constructors.
2.  **Events:** Send an actor's ID (`id()`) in an event to another actor.
3.  **Service Actors:** Use `getService<T>()` (same core) or `getServiceId<Tag>(core_id)` (any core) for well-known singleton services.
4.  **Dependency Resolution:** Use `require<T>()` and handle `RequireEvent` for dynamic discovery.
5.  **Manager/Registry Actor:** Implement a central registry actor where other actors can register themselves and query for others.

**Q: How do I handle large data in events?**

A: Avoid copying large data directly into event structs. Instead, pass it using `std::shared_ptr` or `std::unique_ptr` (if ownership transfer is intended and possible, though `shared_ptr` is often safer for asynchronous passing).
```cpp
struct LargeDataEvent : qb::Event {
    std::shared_ptr<std::vector<char>> data;
    // ...
};

// Sender
auto my_large_data = std::make_shared<std::vector<char>>(1024*1024);
// ... fill data ...
push<LargeDataEvent>(dest_id, my_large_data);

// Receiver
void on(const LargeDataEvent& event) {
    // Access data via event.data pointer
    process(*event.data);
}
```
Consider using `pipe.allocated_push<T>(size, ...)` if you know the approximate size beforehand to potentially optimize buffer allocation in the communication pipe.

**Q: Can I use blocking libraries with QB actors?**

A: **Not directly within actor event handlers or callbacks.** Blocking calls will stall the actor's `VirtualCore`. Wrap blocking calls in `qb::io::async::callback` to execute them asynchronously, or create dedicated worker actors (potentially on separate cores with higher latency settings) to handle interactions with blocking libraries, communicating results back via events.

**Q: Is there built-in actor supervision (like Erlang/Akka)?**

A: No, QB Core does not provide a built-in supervision hierarchy. Developers need to implement supervision strategies manually using patterns like parent monitoring, health checks (ping/pong with timeouts), and potentially restarting failed actors. See the [Error Handling Guide](./../6_guides/error_handling.md).

**Q: How is QB different from other C++ actor frameworks?**

A: (General comparison points - specific differences require deeper comparison)
*   **Integration with `qb-io`:** Tightly integrated with its own high-performance asynchronous I/O library.
*   **Focus:** Aims for high performance and relatively low-level control compared to some higher-abstraction frameworks.
*   **Simplicity:** Provides core actor primitives without enforcing complex lifecycle or supervision patterns by default.
*   **Dependencies:** Relatively few core dependencies (libev internally, optionally OpenSSL/Zlib).

**Q: Where can I find the most detailed API documentation?**

A: The source code headers (`qb/include/`) are the definitive reference for class members, method signatures, and template parameters. 