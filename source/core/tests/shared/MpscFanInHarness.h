/**
 * @file qb/source/core/tests/shared/MpscFanInHarness.h
 * @brief Lock-free MPSC fan-in scaffold for the pseudo-framework mailbox benches
 *
 * bm-mpsc-mailbox-sweep and bm-mpsc-router-mailbox both model a single consumer core draining
 * \c qb::lockfree::mpsc::ringbuffer<EventBucket, Cap, 0> (the same shape as engine mailboxes,
 * one dedicated SPSC slice per producer) under sustained fan-in. They share, verbatim:
 *
 *   - \c quota_for()              — even split of \c total across producers (remainder to the
 *                                   first \c total % nb_producers producers);
 *   - the launch model           — a \c std::barrier start gate, one \c qb::jthread per producer
 *                                   busy-enqueueing its quota (spinning on a full ring and
 *                                   counting the false returns as enqueue failures), one consumer
 *                                   \c qb::jthread draining by batch with the
 *                                   \c dequeue(Func, buf, batch) overload used by
 *                                   \c VirtualCore::__receive__ (per-producer slices into a scratch
 *                                   buffer, functor invoked per batch);
 *   - \c apply_mpsc_fan_in_args() — the cartesian (totals × producers × dequeue_batch) generator.
 *
 * \c run_mpsc_fan_in() factors that out: the caller supplies a \c make_bucket functor (build the
 * \c EventBucket to enqueue, given producer id + per-producer sequence) and a \c consume functor
 * (process one dequeued batch). The plain sweep passes a no-op consumer; the router variant
 * dispatches each bucket through \c qb::router::memh. Returns the total enqueue-failure count.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * @ingroup Core
 */

#ifndef QB_MPSC_FAN_IN_HARNESS_H
#define QB_MPSC_FAN_IN_HARNESS_H

#include <algorithm>
#include <atomic>
#include <barrier>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <qb/system/lockfree/mpsc.h>
#include <qb/utility/compat.h>
#include <qb/utility/prefix.h>

#include "BenchmarkCores.h"

namespace qb::bench {

/**
 * Even split of \c total across \c nb_producers: each producer gets \c total / nb_producers,
 * and the first \c total % nb_producers producers get one extra (so the sum is exactly \c total).
 */
[[nodiscard]] constexpr std::uint64_t
quota_for(std::size_t const producer_index, std::uint64_t const total, std::size_t const nb_producers) noexcept {
    const auto          p    = static_cast<std::uint64_t>(nb_producers);
    const std::uint64_t base = total / p;
    const std::uint64_t rem  = total % p;
    return base + (static_cast<std::uint64_t>(producer_index) < rem ? 1ull : 0ull);
}

/**
 * Fan-in driver: \p nb_producers threads each enqueue their \c quota_for() share of buckets into
 * a per-producer ring slice; one consumer drains all \p total buckets by batch. The start gate
 * releases producers and the consumer together so the run measures sustained pressure, not
 * thread spin-up.
 *
 * \param make_bucket  `EventBucket(std::size_t pid, std::uint64_t sent)` — the bucket producer
 *                      \p pid enqueues for its \p sent-th message (sequence within its quota).
 * \param consume      `void(EventBucket* buffer, std::size_t nb)` — process one dequeued batch.
 *                      Runs on the consumer thread; matches the engine's per-batch functor.
 * \return total enqueue failures (false \c enqueue() returns summed across producers) — the
 *         busy-retry count when a ring was full, i.e. the ring-pressure signal.
 */
template <std::size_t MailboxCap, typename MakeBucket, typename Consume>
[[nodiscard]] std::uint64_t
run_mpsc_fan_in(std::size_t const nb_producers, std::uint64_t const total, std::size_t const dequeue_batch, MakeBucket make_bucket,
                Consume consume) {
    qb::lockfree::mpsc::ringbuffer<EventBucket, MailboxCap, 0> queue(nb_producers);

    const std::size_t batch_cap = std::max<std::size_t>(1u, dequeue_batch);
    auto              batch_buf = std::make_unique<EventBucket[]>(batch_cap);

    std::atomic<std::uint64_t> fail_sum{0};
    std::barrier               start_gate(static_cast<std::ptrdiff_t>(nb_producers + 1u));

    std::vector<qb::jthread> producers;
    producers.reserve(nb_producers);
    for (std::size_t pid = 0; pid < nb_producers; ++pid) {
        const std::uint64_t q = quota_for(pid, total, nb_producers);
        producers.emplace_back([&, pid, q] {
            start_gate.arrive_and_wait();
            std::uint64_t local_fails = 0;
            std::uint64_t sent        = 0;
            while (sent < q) {
                EventBucket bucket = make_bucket(pid, sent);
                if (queue.enqueue(pid, bucket))
                    ++sent;
                else
                    ++local_fails;
            }
            fail_sum.fetch_add(local_fails, std::memory_order_relaxed);
        });
    }

    qb::jthread consumer([&] {
        start_gate.arrive_and_wait();
        std::uint64_t popped = 0;
        while (popped < total) {
            // Match engine: mpsc::dequeue(Func, ret, size) — each producer ring delivers up to
            // `batch_cap` contiguous items into `ret` and invokes the functor (see
            // VirtualCore::__receive__). Avoid mpsc::dequeue(T*, n) in one call across multiple
            // rings, which reuses the same `ret` base without advancing the pointer.
            const std::size_t n = queue.dequeue(consume, batch_buf.get(), batch_cap);
            popped += static_cast<std::uint64_t>(n);
        }
    });

    producers.clear(); // join producers
    consumer.join();
    return fail_sum.load(std::memory_order_relaxed);
}

/**
 * Registers the (producers, total_messages, dequeue_batch) cartesian sweep shared by both
 * mailbox benches: totals {200k, 1M} × producers {1,2,4,8} (capped to \c cappedBenchmarkCores())
 * × dequeue_batch {1,16,64}.
 */
inline void
apply_mpsc_fan_in_args(::benchmark::internal::Benchmark *b) {
    const auto         cap       = qb::bench::cappedBenchmarkCores();
    const std::int64_t totals[]  = {200'000, 1'000'000};
    const std::int64_t batches[] = {1, 16, 64};

    for (std::int64_t total : totals) {
        for (std::uint32_t p = 1u; p <= std::min<std::uint32_t>(8u, cap); p *= 2u) {
            for (std::int64_t batch : batches) {
                b->Args({static_cast<std::int64_t>(p), total, batch});
            }
        }
    }
}

} // namespace qb::bench

#endif // QB_MPSC_FAN_IN_HARNESS_H
