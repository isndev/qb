@page ref_lockfree_primitives_md QB Framework: Lock-Free Primitives Reference
@brief An overview of the lock-free data structures used internally by the QB Actor Framework for high-performance concurrency.

# QB Framework: Lock-Free Primitives Reference

The QB Actor Framework utilizes several lock-free data structures internally to achieve high performance, particularly for inter-core communication and other critical concurrent operations. While direct interaction with these primitives by application developers is typically unnecessary (as the actor model provides higher-level, safer abstractions), understanding their existence and purpose can offer insights into QB's performance characteristics.

**Note:** For most application development needs, rely on `qb::Actor` event passing (`push`, `send`, etc.) for inter-thread and inter-actor communication. The primitives below are primarily for framework internals.

## 1. `qb::lockfree::SpinLock`

*   **Header:** `qb/system/lockfree/spinlock.h`
*   **Purpose:** A basic, lightweight mutual exclusion (mutex) mechanism that employs busy-waiting (or "spinning") rather than yielding the CPU or putting the calling thread to sleep when contending for the lock.
*   **Mechanism:** Implemented using `std::atomic<bool>` and atomic exchange operations (e.g., `_lock.exchange(true, std::memory_order_acquire)`).
*   **Key Public Methods:**
    *   `lock()`: Acquires the lock, spinning indefinitely if necessary.
    *   `trylock() -> bool`: Attempts to acquire the lock once and returns immediately (true if successful).
    *   `trylock(int64_t spin_count) -> bool`: Tries to acquire, spinning up to `spin_count` times.
    *   `trylock_for(const qb::Duration& timespan) -> bool`: Tries to acquire within a specified duration.
    *   `trylock_until(const qb::UtcTimePoint& timestamp) -> bool`: Tries to acquire until a specific time point.
    *   `unlock()`: Releases the lock.
    *   `locked() -> bool`: Checks if the lock is currently held.
*   **Use Case within QB:** Primarily used for extremely short critical sections where contention is expected to be very low and the overhead of a `std::mutex` (which involves system calls) is undesirable. For example, it's used internally in some modes of the MPSC queue.
*   **General Caution:** Spinlocks can be very CPU-intensive if the lock is held for more than a few instructions or if contention is high, as waiting threads will consume 100% CPU on their core. Their use requires careful consideration.

## 2. SPSC Ring Buffer (`qb::lockfree::spsc::ringbuffer<T, MaxSize>` or `ringbuffer<T, 0>`)

*   **Header:** `qb/system/lockfree/spsc.h`
*   **Purpose:** A Single-Producer, Single-Consumer lock-free circular queue (FIFO).
*   **Mechanism:** Employs atomic read and write indices (`std::atomic<size_t>`) with appropriate C++ memory ordering (`std::memory_order_relaxed`, `acquire`, `release`) to coordinate safe access between exactly one producer thread and exactly one consumer thread without traditional locks.
*   **Implementations:**
    *   `ringbuffer<T, MaxSize>`: Buffer size (`MaxSize`) is fixed at compile time.
    *   `ringbuffer<T, 0>` (then constructed with `size_t max_size`): Buffer size is determined at runtime.
*   **Key Public Methods (Conceptual - see header for exact signatures):**
    *   `enqueue(const T& item) -> bool` / `enqueue(const T* items, size_t count) -> size_t`: Producer adds one or more items. Returns success status or count of items added. Fails if the buffer is full.
    *   `dequeue(T* item) -> bool` / `dequeue(T* items, size_t count) -> size_t`: Consumer removes one or more items. Returns success status or count of items removed. Fails if the buffer is empty.
    *   `dequeue(Functor func, T* buffer, size_t count) -> size_t`: Dequeues items into a temporary buffer and then passes them to `func`.
    *   `consume_all(Functor func) -> size_t`: Allows the consumer to process all currently available items directly from the internal ring buffer segments, potentially avoiding intermediate copies. `func` is called with `(T* segment_start, size_t segment_length)`.
    *   `empty() -> bool`: Checks if the buffer is empty.
*   **Use Case within QB:** Forms the building block for the MPSC queue. Not typically used directly by application code relying on actors.

## 3. MPSC Ring Buffer (`qb::lockfree::mpsc::ringbuffer<T, MaxSize, NumProducers>` or `ringbuffer<T, MaxSize, 0>`)

*   **Header:** `qb/system/lockfree/mpsc.h`
*   **Purpose:** A Multiple-Producer, Single-Consumer lock-free queue. Allows many threads to concurrently produce items, while a single designated thread consumes them.
*   **Mechanism:** It is typically implemented as an array of SPSC ring buffers. Each producer thread (or a group of threads hashing to the same SPSC queue) enqueues into its dedicated SPSC buffer. The single consumer thread iterates through all these internal SPSC buffers to dequeue items in a fair manner (e.g., round-robin).
    *   Enqueueing to a specific SPSC queue (if producer index is known) can be fully lock-free.
    *   The generic `enqueue(const T& item)` that distributes among internal SPSC queues might use a `SpinLock` for brief synchronization if contention for selecting an SPSC queue occurs.
*   **Implementations:**
    *   `ringbuffer<T, MaxSize, NumProducers>`: Number of internal SPSC queues (and thus, conceptually, dedicated producer slots) is fixed at compile time.
    *   `ringbuffer<T, MaxSize, 0>` (then constructed with `size_t num_producers`): Number of internal SPSC queues is determined at runtime.
*   **Key Public Methods (Conceptual):**
    *   `enqueue<Index>(const T& item)` / `enqueue<Index>(const T* items, size_t count)`: Producer enqueues to a specific internal SPSC queue identified by compile-time `Index` (often fully lock-free).
    *   `enqueue(size_t index, const T& item)` / `enqueue(size_t index, const T* items, size_t count)`: Producer enqueues to an SPSC queue by runtime `index` (often fully lock-free).
    *   `enqueue(const T& item)` / `enqueue(const T* items, size_t count)`: General enqueue method that may distribute the item to one of the internal SPSC queues (potentially using a brief spinlock for selecting the queue).
    *   `dequeue(T* items, size_t count) -> size_t`: Consumer attempts to dequeue items by checking all internal producer queues.
    *   `dequeue(Functor func, T* buffer, size_t count) -> size_t`: Dequeues and processes with a functor.
    *   `consume_all(Functor func) -> size_t`: Consumer processes all available items from all producer queues.
    *   `ringOf(size_t index) -> spsc::ringbuffer&`: Provides direct access to a specific internal SPSC ring buffer (use with caution).
*   **Primary Use Case in QB:** This is the **core data structure for inter-`VirtualCore` communication**. Each `VirtualCore` has an MPSC ring buffer (`Mailbox` in `SharedCoreCommunication`) that acts as its incoming event queue. Other `VirtualCore`s are the producers, and the `VirtualCore` owning the mailbox is the single consumer.

While these lock-free primitives are fundamental to QB's performance, developers should primarily interact with the higher-level actor messaging system (`qb::Actor::push`, `reply`, etc.), which abstracts these details and provides a safer, more idiomatic programming model for concurrent applications.

**(Next:** This concludes the main reference sections. You might want to revisit the `[Case Studies: QB Core & IO Integration Examples](./../5_core_io_integration/examples/README.md)` or other `[Developer Guides](./../6_guides/README.md)`.)** 