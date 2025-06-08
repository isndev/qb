@page guides_performance_tuning_md QB Framework: Performance Tuning Guide
@brief Optimize your QB actor applications for maximum speed, scalability, and efficiency with these targeted tuning strategies.

# QB Framework: Performance Tuning Guide

The QB Actor Framework is engineered for high performance out-of-the-box. However, to extract the maximum potential for your specific application and hardware, consider these targeted tuning strategies. Always remember to **profile your application first** to identify actual bottlenecks before applying optimizations prematurely.

## 1. `VirtualCore` and Threading Configuration

Fine-tuning how `qb::Main` manages its `VirtualCore` worker threads can significantly impact performance.

*   **Optimal Core Count:**
    *   **Default:** `qb::Main` typically defaults to `std::thread::hardware_concurrency()` `VirtualCore`s.
    *   **Tuning:** If your application is I/O-bound with few active actors, or if profiling shows excessive context switching, consider reducing the number of cores during `qb::Main` instantiation. Conversely, ensure enough cores are available for CPU-bound actors to run in parallel.
*   **CPU Affinity (`CoreInitializer::setAffinity(qb::CoreIdSet)`):
    *   **Purpose:** Pin a `VirtualCore` thread to one or more specific physical CPU cores.
    *   **Benefit:** Prevents OS thread migration, significantly improving L1/L2 cache hit rates and reducing context-switching overhead. This is especially beneficial for CPU-intensive actors or tightly communicating groups of actors that frequently access shared cache lines (though direct data sharing is discouraged by actors).
    *   **How:** Before calling `main.start()`, use `main.core(virtual_core_id).setAffinity(affinity_set);`.
    *   **Caution:** Requires careful planning. Incorrect affinity settings can lead to core overloading or underutilization. Profile to determine optimal pinning for critical actors/cores.
*   **Event Loop Latency (`CoreInitializer::setLatency(nanoseconds)`):
    *   **Purpose:** Controls the idle behavior of a `VirtualCore`'s event loop.
    *   **`0` (Default for new cores):** Lowest possible event processing latency. The `VirtualCore` will busy-spin (consume 100% CPU on its assigned core) even when idle, constantly checking for new events. Ideal for ultra-latency-sensitive applications (e.g., high-frequency trading, real-time control systems).
    *   **`> 0` (e.g., `1000000` for 1ms):** Allows the `VirtualCore` to sleep for up to the specified duration (in nanoseconds) if its event queues are empty. This drastically reduces CPU consumption during idle periods at the cost of potentially introducing a slight delay (up to the specified latency) in picking up the next event.
    *   **Tuning:** For most server applications or systems with intermittent workloads, a small positive latency (e.g., 100µs to 5ms) often provides an excellent balance between responsiveness and CPU efficiency. Measure your application's specific latency requirements and CPU usage under load to find the sweet spot.

**(Reference:** [QB-Core: Engine - qb::Main & VirtualCore](./../4_qb_core/engine.md), `test-main.cpp` for examples.)**

## 2. Optimizing Event Messaging

Efficient message passing is key to actor system performance.

*   **`push` (Ordered) vs. `send` (Unordered):
    *   **`push<Event>(...)`:** The **default and strongly recommended** method. Ensures ordered delivery between a specific sender/receiver pair and correctly handles non-trivially destructible event payloads.
    *   **`send<Event>(...)`:** Use **only in rare, performance-critical scenarios** where: (a) event order absolutely does not matter, (b) communication is guaranteed to be same-core, AND (c) the event type is **trivially destructible** (e.g., contains only PODs or `qb::string<N>`). Incorrect use can lead to very hard-to-debug ordering issues and is generally not worth the marginal potential gain.
*   **`EventBuilder` (`actor.to(dest).push<...>(...)`):
    *   If sending *multiple* events to the *same destination actor* consecutively from within a single event handler, using the `EventBuilder` can provide a minor performance improvement by avoiding repeated lookups for the communication pipe.
*   **Direct Pipe Access & `allocated_push` (`actor.getPipe(dest).allocated_push<Event>(size_hint, ...)`):
    *   **Crucial for Large Events:** When an event contains a large payload (e.g., a sizeable buffer passed via `std::shared_ptr` or `std::unique_ptr`), use `allocated_push`. Provide a `size_hint` (approximately `sizeof(Event) + actual_payload_size`) to pre-allocate a contiguous block in the underlying communication pipe. This can significantly reduce or eliminate expensive reallocations and memory copies within the pipe during event construction and enqueuing.
*   **`reply(Event&)` and `forward(ActorId, Event&)`:
    *   **Always prefer these** for request-response or delegation patterns. They reuse the existing event object, completely avoiding the overhead of new event allocation, construction, and data copying.
*   **Event Payload Design:**
    *   Keep event structures lean. They are primarily data carriers.
    *   **Pass Large Data by Smart Pointer:** For substantial data (e.g., image buffers, large text blocks, collections), include a `std::shared_ptr<DataType>` or `std::unique_ptr<DataType>` in your event. This ensures only the pointer is copied/moved, not the entire data block.
    *   **Use `qb::string<N>` for Strings:** Prefer `qb::string<N>` over `std::string` for string data within events to avoid potential ABI issues and heap allocations for small-to-medium strings. For very large strings, `std::shared_ptr<std::string>` is an option if `qb::string<N>` is insufficient.

**(Reference:** [QB-Core: Event Messaging Between Actors](./../4_qb_core/messaging.md), `test-actor-event.cpp`.)**

## 3. Actor Design and Placement Strategies

How you design and distribute your actors impacts system performance.

*   **Actor Granularity:** Avoid overly fine-grained actors if the communication overhead for simple tasks outweighs the benefits of decomposition. Conversely, overly coarse-grained actors can become bottlenecks or limit parallelism.
*   **State Locality & Communication Patterns:**
    *   **Co-locate Frequent Communicators:** Place actors that exchange many messages frequently on the *same `VirtualCore`* if possible. This minimizes inter-core communication overhead (which, while efficient in QB, is still more expensive than same-core).
    *   Use `main.addActor<MyActor>(core_id, ...)` or the `core(core_id).builder()` to explicitly assign actors to cores.
*   **Identifying and Isolating Bottlenecks:**
    *   If an actor consistently handles a high volume of messages or performs CPU-intensive computations, consider placing it on a dedicated `VirtualCore` with appropriate affinity and low (or zero) latency settings.
    *   Distribute work by sharding data or requests across multiple instances of worker actors if a single actor type becomes a bottleneck.
*   **Non-Blocking Behavior is Paramount:** **Strictly avoid any blocking operations** (long computations, synchronous/blocking I/O calls, extended waits on mutexes or condition variables) directly within an actor's `on(Event&)` handlers or `onCallback()` method. A blocked actor stalls its entire `VirtualCore`! Offload such tasks using:
    *   `qb::io::async::callback` for simpler, non-CPU-intensive blocking calls.
    *   Dedicated worker actors (potentially on different cores) for more complex or frequent blocking operations. These workers perform the blocking task and send a result event back.
*   **Referenced Actors (`addRefActor`):
    *   Can reduce event-passing overhead for tightly-coupled parent-child actors that reside on the **same core** and interact very frequently with simple calls.
    *   **Use with Caution:** Direct method calls bypass the child's mailbox, breaking standard actor isolation and potentially creating state consistency issues if not managed meticulously. Prefer events unless a significant, *measured* performance gain is demonstrated for a critical interaction.

## 4. I/O Performance Considerations (for `qb-io` usage)

*   **Protocol Efficiency:** Binary protocols (e.g., `text::binary16/32` which use `base::size_as_header`) are generally more performant than text-based protocols due to less parsing overhead.
*   **Zero-Copy Read Views:** When parsing, if your protocol supports it (like `text::string_view` or `text::command_view`), using `std::string_view` for message payloads can avoid copying data from the input buffer into new strings, provided the view is used before the buffer is modified.
*   **Buffer Management (`qb::allocator::pipe`):** `qb-io` streams use `qb::allocator::pipe` internally. While generally efficient, understanding its `reorder()` behavior can be useful if you are doing very low-level custom buffer manipulation (rarely needed).
*   **SSL/TLS Overhead:** Encryption/decryption inherently adds CPU overhead. Ensure you are using modern, efficient cipher suites. For applications with many short-lived connections, SSL session resumption/caching can significantly reduce handshake latency.
*   **Compression Trade-offs:** When using `qb::compression`, balance the desired compression level against CPU cost. Higher compression levels save more bandwidth/storage but consume more CPU cycles.

## 5. Profiling: Your Most Important Tool

Theoretical optimizations are useful, but **always profile your application** under realistic load conditions to identify actual bottlenecks.

*   **System Profilers:** Use platform-specific tools (`perf` on Linux, Instruments on macOS, VTune, Visual Studio Profiler) to find CPU hotspots, cache misses, and contention points.
*   **Application-Level Metrics:** Instrument your critical actors with `std::atomic` counters or use `qb::LogTimer` / `qb::ScopedTimer` to measure:
    *   Event processing times.
    *   Mailbox queue lengths (if accessible or instrumented).
    *   Message throughput per actor/core.
    *   Latency for specific request-response cycles.
*   **Logging:** Use a high-performance logger (like `nanolog` if `QB_LOGGER` is enabled) *selectively* for tracing event flow or timing critical sections. Excessive logging in hot paths can itself become a bottleneck.

By systematically identifying bottlenecks through profiling and applying these QB-specific tuning techniques judiciously, you can build highly performant and scalable actor-based systems.

**(Next:** Learn about [QB Framework: Error Handling & Resilience Strategies](./error_handling.md) or [QB Framework: Effective Resource Management](./resource_management.md)**) 