/**
 * @file qb/system/lockfree/mpsc_unbounded_queue.h
 * @brief Unbounded lock-free Multiple-Producer Single-Consumer queue
 *
 * Michael-Scott style linked-list queue. Multiple producers can push concurrently
 * without locks; a single consumer pops. Unbounded (heap-allocated nodes).
 * Suitable for coroutine ready-queues, task queues, and work-stealing tails.
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
 * @ingroup LockFree
 */

#ifndef QB_LOCKFREE_MPSC_UNBOUNDED_QUEUE_H
#define QB_LOCKFREE_MPSC_UNBOUNDED_QUEUE_H

#include <atomic>
#include <qb/utility/nocopy.h>

namespace qb::lockfree {

/**
 * @brief Unbounded lock-free MPSC queue (Michael-Scott algorithm)
 *
 * - push(): lock-free, can be called from multiple threads or from one thread
 *   from different call contexts (e.g. callbacks, coroutines).
 * - pop(): only the single consumer thread; returns true and fills @a out if an
 *   item was available.
 * - empty(): approximate; only the consumer should call it for consistency.
 *
 * @tparam T Movable type to store (will be moved on push/pop).
 */
template <typename T>
class mpsc_unbounded_queue : public nocopy {
    struct Node {
        std::atomic<Node*> next{nullptr};
        T value;

        explicit Node(T&& v) : value(std::move(v)) {}
    };

    alignas(64) std::atomic<Node*> head_{nullptr};
    alignas(64) std::atomic<Node*> tail_{nullptr};
    std::atomic<std::size_t> count_{0};
    Node* sentinel_{nullptr};

public:
    mpsc_unbounded_queue() {
        sentinel_ = new Node(T{});
        head_.store(sentinel_, std::memory_order_relaxed);
        tail_.store(sentinel_, std::memory_order_relaxed);
    }

    ~mpsc_unbounded_queue() {
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) {
            Node* next = n->next.load(std::memory_order_relaxed);
            if (n != sentinel_) {
                delete n;
            }
            n = next;
        }
    }

    /** @brief Enqueue one item; lock-free, safe from multiple producers. */
    void push(T item) {
        Node* node = new Node(std::move(item));
        node->next.store(nullptr, std::memory_order_relaxed);
        Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Dequeue one item; only the single consumer thread may call this.
     * @param out Receives the popped value if the queue was non-empty.
     * @return true if an item was popped, false if the queue was empty.
     */
    bool pop(T& out) {
        Node* node = head_.load(std::memory_order_acquire);
        Node* next = node->next.load(std::memory_order_acquire);

        if (next) {
            head_.store(next, std::memory_order_release);
            out = std::move(next->value);
            count_.fetch_sub(1, std::memory_order_relaxed);
            if (node != sentinel_) {
                delete node;
            }
            return true;
        }

        if (tail_.load(std::memory_order_acquire) == node) {
            return false;
        }

        while (!(next = node->next.load(std::memory_order_acquire))) {
            /* spin until producer links the node */
        }
        head_.store(next, std::memory_order_release);
        out = std::move(next->value);
        count_.fetch_sub(1, std::memory_order_relaxed);
        if (node != sentinel_) {
            delete node;
        }
        return true;
    }

    /** @brief Approximate number of items (consumer may call; can change immediately). */
    [[nodiscard]] std::size_t size() const {
        return count_.load(std::memory_order_acquire);
    }

    /**
     * @brief Check if the queue appears empty (only consumer should use this).
     * Approximate: a producer might push immediately after.
     */
    [[nodiscard]] bool empty() const {
        return head_.load(std::memory_order_acquire)->next.load(std::memory_order_acquire) == nullptr
            && tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }
};

} // namespace qb::lockfree

#endif // QB_LOCKFREE_MPSC_UNBOUNDED_QUEUE_H
