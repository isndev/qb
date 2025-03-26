/**
 * @file qb/system/lockfree/spinlock.h
 * @brief Spinlock synchronization primitives
 * 
 * This file provides various spinlock implementations for low-latency synchronization.
 * Spinlocks are a type of mutex that uses busy-waiting instead of context switching,
 * making them efficient for short critical sections where the expected contention is low.
 * 
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Lockfree
 */

#ifndef QB_SPINLOCK_H
#define QB_SPINLOCK_H
#include <atomic>
#include <qb/system/timestamp.h>

namespace qb::lockfree {

/**
 * @brief A spinlock implementation for lightweight thread synchronization
 *
 * A spinlock is a lock that causes a thread trying to acquire it to wait in a loop
 * ("spin") while repeatedly checking if the lock is available. This implementation
 * provides methods to acquire the lock with various strategies such as trying
 * for a specific duration or spin count.
 */
class SpinLock {
public:
    /**
     * @brief Default constructor that initializes the lock to unlocked state
     */
    SpinLock() noexcept
        : _lock(false) {}
    
    /**
     * @brief Copy constructor is deleted to prevent copying of locks
     */
    SpinLock(const SpinLock &) = delete;
    
    /**
     * @brief Move constructor is deleted to prevent moving of locks
     */
    SpinLock(SpinLock &&) = delete;

    /**
     * @brief Default destructor
     */
    ~SpinLock() = default;

    /**
     * @brief Copy assignment operator is deleted to prevent copying of locks
     */
    SpinLock &operator=(const SpinLock &) = delete;
    
    /**
     * @brief Move assignment operator is deleted to prevent moving of locks
     */
    SpinLock &operator=(SpinLock &&) = delete;

    /**
     * @brief Check if the spinlock is currently locked
     *
     * @return true if the lock is held, false otherwise
     */
    bool
    locked() noexcept {
        return _lock.load(std::memory_order_acquire);
    }

    /**
     * @brief Try to acquire the lock without spinning
     *
     * This method attempts to acquire the lock once and returns immediately
     * regardless of success.
     *
     * @return true if the lock was acquired, false if it was already locked
     */
    bool
    trylock() noexcept {
        return !_lock.exchange(true, std::memory_order_acquire);
    }

    /**
     * @brief Try to acquire the lock with a maximum number of spin attempts
     *
     * This method attempts to acquire the lock, spinning for up to 'spin'
     * iterations if necessary.
     *
     * @param spin Maximum number of spin iterations
     * @return true if the lock was acquired, false if maximum spins exceeded
     */
    bool
    trylock(int64_t spin) noexcept {
        // Try to acquire spin-lock at least one time
        do {
            if (trylock())
                return true;
        } while (spin-- > 0);

        // Failed to acquire spin-lock
        return false;
    }

    /**
     * @brief Try to acquire the lock for a specified time duration
     *
     * This method attempts to acquire the lock, spinning until either the lock
     * is acquired or the specified duration has elapsed.
     *
     * @param timespan Maximum duration to try acquiring the lock
     * @return true if the lock was acquired, false if the timeout expired
     */
    bool
    trylock_for(const Timespan &timespan) noexcept {
        // Calculate a finish timestamp
        Timestamp finish = NanoTimestamp() + timespan;

        // Try to acquire spin-lock at least one time
        do {
            if (trylock())
                return true;
        } while (NanoTimestamp() < finish);

        // Failed to acquire spin-lock
        return false;
    }

    /**
     * @brief Try to acquire the lock until a specified point in time
     *
     * This method attempts to acquire the lock, spinning until either the lock
     * is acquired or the specified timestamp is reached.
     *
     * @param timestamp Point in time until which to try acquiring the lock
     * @return true if the lock was acquired, false if the timeout expired
     */
    bool
    trylock_until(const UtcTimestamp &timestamp) noexcept {
        return trylock_for(timestamp - UtcTimestamp());
    }

    /**
     * @brief Acquire the lock, waiting indefinitely if necessary
     *
     * This method spins until the lock is acquired, with no timeout.
     * It should be used with caution as it can potentially cause deadlocks
     * or waste CPU resources if the lock is held for a long time.
     */
    void
    lock() noexcept {
        while (_lock.exchange(true, std::memory_order_acquire))
            ;
    }

    /**
     * @brief Release the lock
     *
     * This method releases the lock, allowing other threads to acquire it.
     */
    void
    unlock() noexcept {
        _lock.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> _lock;  ///< The underlying atomic flag used for the lock state
};

} // namespace qb::lockfree

#endif // QB_SPINLOCK_H
