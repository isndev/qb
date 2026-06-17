/**
 * @file qb/core/tests/benchmark/bm-mpsc-mailbox-sweep.cpp
 * @brief Lock-free MPSC mailbox sweep (pseudo-framework workload)
 *
 * Exercises \c qb::lockfree::mpsc::ringbuffer with the same element type and per-producer
 * routing model as engine mailboxes (\c EventBucket, one dedicated SPSC slice per producer).
 * Google Benchmark template parameter \c MailboxCap matches the compile-time capacity passed
 * to \c mpsc::ringbuffer (usable slots per producer ring, consistent with \c Main.h
 * \c MaxRingEvents).
 *
 * Scenarios register several \c MailboxCap values so you can compare throughput and
 * \c enqueue_failures (busy-retry count when the ring is full) across sizes under fan-in load.
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
 * WITHOUT WARRANTIES OR ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Core
 */

#include <algorithm>
#include <atomic>
#include <barrier>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <qb/system/lockfree/mpsc.h>
#include <qb/utility/prefix.h>

#include "../shared/BenchmarkIterationSink.h"

namespace {

[[nodiscard]] constexpr std::uint64_t
quota_for(std::size_t const producer_index, std::uint64_t const total,
            std::size_t const nb_producers) noexcept {
    const auto       p  = static_cast<std::uint64_t>(nb_producers);
    const std::uint64_t base = total / p;
    const std::uint64_t rem  = total % p;
    return base + (static_cast<std::uint64_t>(producer_index) < rem ? 1ull : 0ull);
}

/**
 * Fan-in: \p nb_producers threads each call \c enqueue(producer_id, ...) for a fixed quota;
 * one consumer thread drains with the same \c dequeue(Func, buf, batch) overload as
 * \c VirtualCore::__receive__ (per-producer slices into a scratch buffer, functor per batch).
 * Failed enqueues spin (models sustained load); \p enqueue_failures counts false returns.
 */
template <std::size_t MailboxCap>
void
run_mpsc_fan_in(std::size_t const nb_producers, std::uint64_t const total,
                std::size_t const dequeue_batch, std::uint64_t &enqueue_failures) {
    enqueue_failures = 0;
    qb::lockfree::mpsc::ringbuffer<EventBucket, MailboxCap, 0> queue(nb_producers);

    const std::size_t batch_cap = std::max<std::size_t>(1u, dequeue_batch);
    auto              batch_buf = std::make_unique<EventBucket[]>(batch_cap);

    std::atomic<std::uint64_t> fail_sum{0};
    std::barrier               start_gate(static_cast<std::ptrdiff_t>(nb_producers + 1u));

    std::vector<std::jthread> producers;
    producers.reserve(nb_producers);
    for (std::size_t pid = 0; pid < nb_producers; ++pid) {
        const std::uint64_t q = quota_for(pid, total, nb_producers);
        producers.emplace_back([&, pid, q] {
            start_gate.arrive_and_wait();
            EventBucket ev{};
            std::uint64_t   local_fails = 0;
            std::uint64_t   sent        = 0;
            while (sent < q) {
                if (queue.enqueue(pid, ev))
                    ++sent;
                else
                    ++local_fails;
            }
            fail_sum.fetch_add(local_fails, std::memory_order_relaxed);
        });
    }

    std::jthread consumer([&] {
        start_gate.arrive_and_wait();
        std::uint64_t popped = 0;
        while (popped < total) {
            // Match engine: mpsc::dequeue(Func, ret, size) — each producer ring delivers up to
            // `batch_cap` contiguous items into `ret` and invokes the functor (see
            // VirtualCore::__receive__). Avoid mpsc::dequeue(T*, n) in one call across multiple
            // rings, which reuses the same `ret` base without advancing the pointer.
            const std::size_t n = queue.dequeue(
                [](EventBucket * /*buffer*/, std::size_t /*nb*/) noexcept {
                    // Bench only measures ring pressure / throughput; no payload inspection.
                },
                batch_buf.get(),
                batch_cap);
            popped += static_cast<std::uint64_t>(n);
        }
    });

    producers.clear(); // join producers
    consumer.join();
    enqueue_failures = fail_sum.load(std::memory_order_relaxed);
}

template <std::size_t MailboxCap>
void
BM_MpscMailbox_FanInDrain(benchmark::State &state) {
    const auto nb_producers   = static_cast<std::size_t>(state.range(0));
    const auto total          = static_cast<std::uint64_t>(state.range(1));
    const auto dequeue_batch  = static_cast<std::size_t>(state.range(2));

    if (nb_producers == 0u || total == 0ull) {
        state.SkipWithError("invalid range: producers or total is zero");
        return;
    }

    for (auto _ : state) {
        std::uint64_t fails = 0;
        run_mpsc_fan_in<MailboxCap>(nb_producers, total, dequeue_batch, fails);

        state.counters["items_per_s"] =
            benchmark::Counter(static_cast<double>(total),
                               benchmark::Counter::kIsIterationInvariantRate);
        state.counters["enqueue_failures"] =
            benchmark::Counter(static_cast<double>(fails), benchmark::Counter::kIsIterationInvariant);
        state.counters["failures_per_item"] =
            benchmark::Counter(static_cast<double>(fails) / static_cast<double>(total),
                               benchmark::Counter::kIsIterationInvariant);
    }
}

void
apply_fan_in_args(benchmark::internal::Benchmark *b) {
    const auto          cap = qb::bench::cappedBenchmarkCores();
    const std::int64_t  totals[] = {200'000, 1'000'000};
    const std::int64_t  batches[] = {1, 16, 64};

    for (std::int64_t total : totals) {
        for (std::uint32_t p = 1u; p <= std::min<std::uint32_t>(8u, cap); p *= 2u) {
            for (std::int64_t batch : batches) {
                b->Args({static_cast<std::int64_t>(p), total, batch});
            }
        }
    }
}

} // namespace

#define QB_REGISTER_MPSC_MAILBOX_BENCH(Cap)                                                       \
    BENCHMARK_TEMPLATE(BM_MpscMailbox_FanInDrain, Cap)                                            \
        ->Name("BM_MpscMailbox_FanInDrain/capacity_" #Cap)                                         \
        ->Apply(apply_fan_in_args)                                                               \
        ->ArgNames({"producers", "total_messages", "dequeue_batch"})                              \
        ->Unit(benchmark::kMillisecond)                                                          \
        ->UseRealTime()

// Sizes: small rings, powers of two, and ~framework mailbox depth (1023).
QB_REGISTER_MPSC_MAILBOX_BENCH(64);
QB_REGISTER_MPSC_MAILBOX_BENCH(128);
QB_REGISTER_MPSC_MAILBOX_BENCH(256);
QB_REGISTER_MPSC_MAILBOX_BENCH(512);
QB_REGISTER_MPSC_MAILBOX_BENCH(1023);
QB_REGISTER_MPSC_MAILBOX_BENCH(1024);
QB_REGISTER_MPSC_MAILBOX_BENCH(2048);

#undef QB_REGISTER_MPSC_MAILBOX_BENCH

BENCHMARK_MAIN();
