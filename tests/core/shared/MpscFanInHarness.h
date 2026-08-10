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
 * dispatches each bucket through \c qb::router::memh. It returns a \c fan_in_result: the total
 * enqueue-failure count, the buckets actually drained, and whether the drain STALLED — the drain
 * loop is bounded, so a lost bucket ends the run with a reportable error instead of spinning
 * forever and making a defect look like a job that hung.
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
#include <chrono>
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

/// Consecutive EMPTY dequeue turns between two consultations of the clock. The stall guard must not
/// show up in the measurement, so it never reads the clock on a turn that made progress, and only
/// every 65536th turn that did not. A power of two so the producer side can mask instead of divide.
inline constexpr std::uint64_t kFanInIdleProbe = 1ull << 16;

/// How long the consumer tolerates ZERO progress before declaring the run broken. Orders of
/// magnitude above any legitimate quiet period (a full 1M-bucket run costs tens of milliseconds),
/// so it can only fire on a drain that will never finish.
inline constexpr std::chrono::seconds kFanInStallBudget{20};

/**
 * Outcome of one \c run_mpsc_fan_in.
 *
 * \c stalled is the anti-hang witness. The consumer drains until it holds \p total buckets, so a
 * ring or harness defect that loses even one bucket leaves it spinning forever — and that is
 * precisely how both benches were "detecting" a broken drain: the process hangs and whatever is
 * running it eventually kills the job, naming nothing. A defect must not be reported as
 * infrastructure. The consumer now gives up after \c kFanInStallBudget without progress, releases
 * the producers (who would otherwise busy-retry forever against a ring nobody drains) and reports
 * what it managed to take, so the caller can \c SkipWithError with real numbers.
 */
struct fan_in_result {
    std::uint64_t enqueue_failures = 0;     ///< busy-retry count when a ring was full — the ring-pressure signal
    std::uint64_t drained          = 0;     ///< buckets the consumer actually received (== total on a healthy run)
    bool          stalled          = false; ///< the consumer made no progress for kFanInStallBudget
};

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
 * \return a \ref fan_in_result: the enqueue-failure total, the number of buckets actually drained,
 *         and whether the drain stalled (in which case \c drained < \p total).
 */
template <std::size_t MailboxCap, typename MakeBucket, typename Consume>
[[nodiscard]] fan_in_result
run_mpsc_fan_in(std::size_t const nb_producers, std::uint64_t const total, std::size_t const dequeue_batch, MakeBucket make_bucket,
                Consume consume) {
    qb::lockfree::mpsc::ringbuffer<EventBucket, MailboxCap, 0> queue(nb_producers);

    const std::size_t batch_cap = std::max<std::size_t>(1u, dequeue_batch);
    auto              batch_buf = std::make_unique<EventBucket[]>(batch_cap);

    std::atomic<std::uint64_t> fail_sum{0};
    std::atomic<bool>          give_up{false}; // set by the consumer; read by the producers on their retry path
    std::uint64_t              drained = 0;    // written by the consumer thread, read after its join
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
                if (queue.enqueue(pid, bucket)) {
                    ++sent;
                    continue;
                }
                // Ring full: busy-retry — this count IS the measured pressure signal. The abort
                // check lives only on this path, and only every kFanInIdleProbe failures, so a
                // producer that is keeping up pays nothing for it.
                if ((++local_fails & (kFanInIdleProbe - 1u)) == 0u && give_up.load(std::memory_order_acquire))
                    break;
            }
            fail_sum.fetch_add(local_fails, std::memory_order_relaxed);
        });
    }

    qb::jthread consumer([&] {
        start_gate.arrive_and_wait();
        std::uint64_t                         popped     = 0;
        std::uint64_t                         idle_turns = 0;
        bool                                  idle_timed = false;
        std::chrono::steady_clock::time_point idle_since{};
        while (popped < total) {
            // Match engine: mpsc::dequeue(Func, ret, size) — each producer ring delivers up to
            // `batch_cap` contiguous items into `ret` and invokes the functor (see
            // VirtualCore::__receive__). Avoid mpsc::dequeue(T*, n) in one call across multiple
            // rings, which reuses the same `ret` base without advancing the pointer.
            const std::size_t n = queue.dequeue(consume, batch_buf.get(), batch_cap);
            if (n != 0u) {
                popped += static_cast<std::uint64_t>(n);
                idle_turns = 0;
                idle_timed = false;
                continue;
            }
            // Empty turn. Spin cheaply; consult the clock only once every kFanInIdleProbe
            // CONSECUTIVE empty turns, so the guard cannot perturb a healthy run's timing.
            if (++idle_turns < kFanInIdleProbe)
                continue;
            idle_turns     = 0;
            const auto now = std::chrono::steady_clock::now();
            if (!idle_timed) {
                idle_timed = true;
                idle_since = now;
                continue;
            }
            if (now - idle_since >= kFanInStallBudget) {
                give_up.store(true, std::memory_order_release);
                break;
            }
        }
        drained = popped;
    });

    producers.clear(); // join producers
    consumer.join();
    return {fail_sum.load(std::memory_order_relaxed), drained, drained < total};
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
