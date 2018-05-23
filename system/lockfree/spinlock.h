
#ifndef CUBE_LOCKFREE_SPINLOCK_H
#define CUBE_LOCKFREE_SPINLOCK_H

#include <atomic>

namespace cube {
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

            bool try_lock() noexcept {
                return !_lock.exchange(true, std::memory_order_acquire);
            }

            bool try_lock(int64_t spin) noexcept {
                // Try to acquire spin-lock at least one time
                do {
                    if (try_lock())
                        return true;
                } while (spin-- > 0);

                // Failed to acquire spin-lock
                return false;
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

    }
} // namespace cube

#endif //CUBE_LOCKFREE_SPINLOCK_H
