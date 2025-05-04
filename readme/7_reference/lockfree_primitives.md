# Reference: Lock-Free Primitives

QB utilizes lock-free data structures internally for high-performance inter-core communication and potentially in other areas. While direct use by application developers might be rare, understanding them can be helpful for performance analysis or advanced customization.

## `qb::lockfree::SpinLock`

(`qb/include/qb/system/lockfree/spinlock.h`)

*   **Purpose:** A basic mutual exclusion lock that uses busy-waiting (spinning) instead of yielding the CPU or putting the thread to sleep.
*   **Mechanism:** Based on `std::atomic<bool>` and atomic exchange operations (`_lock.exchange`).
*   **Methods:**
    *   `lock()`: Spins indefinitely until the lock is acquired.
    *   `trylock()`: Attempts to acquire the lock once; returns immediately.
    *   `trylock(spin_count)`: Tries to acquire, spinning up to `spin_count` times.
    *   `trylock_for(duration)` / `trylock_until(timepoint)`: Tries to acquire within a time limit.
    *   `unlock()`: Releases the lock.
    *   `locked()`: Checks if the lock is currently held.
*   **Use Case:** Primarily for very short critical sections where contention is expected to be low and the overhead of a `std::mutex` (involving system calls) is undesirable. Used internally within the MPSC queue implementation.
*   **Caution:** Can waste significant CPU cycles if the lock is held for extended periods or if contention is high.

## SPSC Ring Buffer (`qb::lockfree::spsc::ringbuffer`)

(`qb/include/qb/system/lockfree/spsc.h`)

*   **Purpose:** A Single-Producer, Single-Consumer lock-free circular queue.
*   **Mechanism:** Uses atomic read/write indices (`write_index_`, `read_index_`) with appropriate memory ordering (`std::memory_order_relaxed`, `acquire`, `release`) to coordinate access between the single producer thread and the single consumer thread without locks.
*   **Implementations:**
    *   `ringbuffer<T, MaxSize>`: Fixed-size buffer determined at compile time.
    *   `ringbuffer<T, 0>`: Buffer size determined at runtime via constructor.
*   **Methods:**
    *   `enqueue(T)` / `enqueue(T*, size)`: Producer adds item(s). Returns true/count on success, false/0 if full.
    *   `dequeue(T*)` / `dequeue(T*, size)`: Consumer removes item(s). Returns true/count on success, false/0 if empty.
    *   `dequeue(Func, T*, size)`: Dequeues items and passes them to a function.
    *   `consume_all(Func)`: Processes all currently available items without copying them out first.
    *   `empty()`: Checks if the buffer is empty.
*   **Use Case:** Efficient point-to-point communication between two specific threads where one produces and the other consumes. Used internally by MPSC queue.

## MPSC Ring Buffer (`qb::lockfree::mpsc::ringbuffer`)

(`qb/include/qb/system/lockfree/mpsc.h`)

*   **Purpose:** A Multiple-Producer, Single-Consumer lock-free queue.
*   **Mechanism:** Composed of multiple internal SPSC ring buffers, one for each potential producer. Producers enqueue into their dedicated SPSC buffer (using a `SpinLock` if the producer index isn't known at compile time or if using the time-based random enqueue). The single consumer iterates through all internal SPSC buffers to dequeue items.
*   **Implementations:**
    *   `ringbuffer<T, MaxSize, NumProducers>`: Fixed producer count at compile time.
    *   `ringbuffer<T, MaxSize, 0>`: Producer count determined at runtime via constructor.
*   **Methods:**
    *   `enqueue<Index>(T)` / `enqueue<Index>(T*, size)`: Enqueue via compile-time producer index (lock-free).
    *   `enqueue(index, T)` / `enqueue(index, T*, size)`: Enqueue via runtime producer index (lock-free).
    *   `enqueue(T)` / `enqueue(T*, size)`: Enqueue using a time-based hash for producer selection (uses `SpinLock`).
    *   `dequeue(T*, size)`: Consumer dequeues from all producers.
    *   `dequeue(Func, T*, size)`: Dequeues and processes.
    *   `consume_all(Func)`: Processes all available items.
    *   `ringOf(index)`: Get direct access to a specific producer's internal SPSC ring buffer.
*   **Use Case:** **Core of QB's inter-core communication.** `SharedCoreCommunication` uses an MPSC queue (`Mailbox`) for each `VirtualCore`, where other cores act as producers and the target core is the single consumer.

**Note:** While these lock-free structures are available, direct use in application code should be considered carefully. The actor model itself, using events and message passing (`push`), typically provides sufficient and safer mechanisms for inter-thread communication. 