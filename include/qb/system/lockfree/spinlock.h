
#ifndef QB_LOCKFREE_SPINLOCK_H
# define QB_LOCKFREE_SPINLOCK_H
# include <atomic>
# include <qb/system/timestamp.h>


namespace qb {
    namespace lockfree {

        class SpinLock {
        public:
            SpinLock() noexcept : _lock(false) {}
            SpinLock(const SpinLock &) = delete;
            SpinLock(SpinLock &&) = default;

            ~SpinLock() = default;

            SpinLock &operator=(const SpinLock &) = delete;
            SpinLock &operator=(SpinLock &&) = default;

            bool locked() noexcept {
                return _lock.load(std::memory_order_acquire);
            }

            bool trylock() noexcept {
                return !_lock.exchange(true, std::memory_order_acquire);
            }

            bool trylock(int64_t spin) noexcept {
                // Try to acquire spin-lock at least one time
                do {
                    if (trylock())
                        return true;
                } while (spin-- > 0);

                // Failed to acquire spin-lock
                return false;
            }

            bool trylock_for(const Timespan &timespan) noexcept {
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

            bool trylock_until(const UtcTimestamp &timestamp) noexcept {
                return trylock_for(timestamp - UtcTimestamp());
            }

            void lock() noexcept {
                while (_lock.exchange(true, std::memory_order_acquire));
            }

            void unlock() noexcept {
                _lock.store(false, std::memory_order_release);
            }

        private:
            std::atomic<bool> _lock;
        };

    } /* namespace lockfree */
} /* namespace qb */

#endif //QB_LOCKFREE_SPINLOCK_H
