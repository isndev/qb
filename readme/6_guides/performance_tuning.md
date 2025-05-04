# QB Framework: Performance Tuning Guide

QB is designed for high performance, but understanding its internals and applying appropriate techniques can further optimize your applications.

## Core & Threading Configuration

*   **Core Count:** By default, `qb::Main` uses `std::thread::hardware_concurrency()` virtual cores. If your application doesn't benefit from that many threads (e.g., it's heavily I/O bound with few actors, or has specific bottlenecks), consider reducing the number of cores during `qb::Main` construction or configuration to reduce context switching overhead. Conversely, if you have isolated, CPU-intensive tasks, ensure they run on dedicated cores.
*   **CPU Affinity (`CoreInitializer::setAffinity`):**
    *   **Benefit:** Pinning a `VirtualCore` thread to specific physical CPU core(s) prevents the OS from migrating the thread. This improves L1/L2 cache hit rates and reduces overhead, especially for CPU-intensive actors or groups of frequently interacting actors.
    *   **How:** Identify critical cores (e.g., the one running a central router or a CPU-bound worker). Create a `qb::CoreIdSet` with the desired physical core IDs and use `main.core(virtual_core_id).setAffinity(affinity_set);` before `main.start()`.
    *   **Caution:** Over-pinning can lead to resource contention if not done carefully. Profile your application.
*   **Event Loop Latency (`CoreInitializer::setLatency`):**
    *   **`0` (Default):** Lowest event latency. The `VirtualCore` spins actively, consuming 100% CPU even when idle, checking for events constantly. Ideal for latency-sensitive applications (e.g., HFT, real-time processing).
    *   `> 0` (Nanoseconds): The core can sleep for up to the specified latency when idle. Reduces CPU usage significantly when the core has no work. A small value (e.g., 1ms = `1000000`) often provides a good balance between responsiveness and CPU usage for typical server applications.
    *   **Tuning:** Measure event processing latency and CPU usage under load with different latency values to find the optimal setting for your specific workload and hardware.

**(Ref:** `[QB-Core: Engine](./../4_qb_core/engine.md)`, `test-main.cpp`**)

## Message Passing Optimization

*   **`push` vs. `send`:**
    *   `push`: Default, ordered delivery (per source->dest pair). Queues events until end of core loop. Use this unless you have a *very* specific reason not to.
    *   `send`: Unordered, potentially lower latency *only* for same-core communication. Requires trivially destructible events. Use cases are rare; profile carefully. Incorrect use can lead to hard-to-debug ordering issues.
*   **Pipes and Builders:**
    *   `to(dest).push<A>().push<B>();`: Slightly more efficient than separate `push<A>(dest); push<B>(dest);` calls if sending multiple events to the *same destination* consecutively, as it avoids repeated pipe lookups.
    *   `getPipe(dest).allocated_push<T>(size, ...)`: **Important for large events.** Pre-allocates buffer space in the underlying communication pipe. This can prevent potentially expensive reallocations and copies if your event contains large data (e.g., large strings, vectors, serialized objects) that might exceed the default event bucket size. Calculate the approximate total size needed (sizeof(Event) + payload size) and pass it.
*   **`reply` and `forward`:** Use these whenever possible for request/response or delegation patterns. They reuse the existing event object, completely avoiding allocation and serialization overhead.
*   **Event Design:**
    *   Keep events small and focused on data.
    *   Pass large, potentially shared data via `std::shared_ptr` within the event to avoid copying the data itself during serialization.
    *   For `send` or `EventQOS0`, ensure events are trivially destructible (`qb::string` is okay, `std::string` is not).

**(Ref:** `[QB-Core: Messaging](./../4_qb_core/messaging.md)`, `test-actor-event.cpp`**)

## Actor Design & Placement

*   **Granularity:** Avoid extremely fine-grained actors if communication overhead becomes significant. Balance task decomposition with messaging costs.
*   **State Locality:** Keep frequently interacting actors on the *same* `VirtualCore` to minimize inter-core communication overhead. Use `main.addActor(core_id, ...)` or `main.core(core_id).builder()` to control placement.
*   **Bottlenecks:** Identify actors that handle a disproportionate amount of traffic or perform CPU-intensive work. Consider placing them on dedicated cores with appropriate affinity and latency settings.
*   **Blocking Operations:** **Never perform blocking operations** (long computations, blocking I/O, waiting on mutexes/conditions for extended periods) directly within `on(Event&)` or `onCallback()`. Use `qb::io::async::callback` to offload blocking work or redesign using non-blocking I/O patterns (often involving helper actors or `qb::io::use<>`). Blocking an actor blocks its entire `VirtualCore`.
*   **Referenced Actors (`addRefActor`):** Can reduce event overhead for tightly coupled parent-child actors *on the same core*. However, direct method calls bypass the mailbox, potentially creating state consistency issues if not carefully managed. Use primarily for performance-critical, simple interactions where the parent closely manages the child.

## I/O Performance

*   **Protocols:** Choose efficient protocols. Binary protocols (`size_as_header`, `binary16/32`) are generally faster than text-based ones. Use `string_view` variants (`text::command_view`) where possible to avoid string copies during parsing.
*   **Buffering (`qb::allocator::pipe`):** `qb-io` streams use `pipe` internally. Understand its behavior (see `qb/system/allocator/pipe.h`) if doing low-level buffer manipulation.
*   **Zero-Copy (Conceptual):** While true zero-copy (passing data without copying between user/kernel space) is complex, QB aims to minimize copies within the framework itself, especially with `reply`, `forward`, and `string_view` protocols.
*   **SSL/TLS:** Encryption adds overhead. Use efficient cipher suites if configurable. Consider session resumption/caching for frequently connecting clients.
*   **Compression:** Balance compression level vs. CPU usage. Lower levels are faster but compress less.

## Profiling

*   **External Tools:** Use standard C++ profiling tools (like `perf` on Linux, VTune, Visual Studio Profiler) to identify CPU hotspots, cache misses, and lock contention (though lock contention should be minimal within well-designed QB applications).
*   **Internal Metrics:** Consider adding atomic counters or statistics within critical actors or `VirtualCore` (if modifying framework code) to track queue lengths, event processing times, message rates, etc. QB itself provides some basic metrics in `VirtualCore::_metrics` (though accessing them directly isn't part of the public API).
*   **Logging:** Use high-performance logging (like `nanolog` if enabled via `QB_LOGGER`) selectively to trace event flow and measure time between specific points, but be mindful of logging overhead in performance-critical paths. 